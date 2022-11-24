#include "head.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/param.h>
#include <unistd.h>
#include <signal.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <assert.h>
#include <time.h>

#include "global.h"
#include "thread.h"
#include "value.h"
#include "strbuf.h"
#include "ops.h"
#include "dot.h"
#include "iface/iface.h"
#include "hashdict.h"
#include "dfa.h"
#include "thread.h"
#include "spawn.h"

#define WALLOC_CHUNK    (1024 * 1024)

static unsigned int oldpid = 0;

// For -d option
unsigned int run_count;  // counter of #threads
mutex_t run_mutex;       // to protect count
mutex_t run_waiting;     // for main thread to wait on

// One of these per worker thread
struct worker {
    struct global *global;     // global state
    double timeout;
    barrier_t *start_barrier, *middle_barrier, *end_barrier;

    struct dict *visited;

    unsigned int index;          // index of worker
    struct worker *workers;      // points to list of workers
    unsigned int nworkers;       // total number of workers
    int timecnt;                 // to reduce gettime() overhead
    struct step inv_step;        // for evaluating invariants

    unsigned int dequeued;      // total number of dequeued states
    unsigned int enqueued;      // total number of enqueued states

    struct node *results;       // list of resulting states
    unsigned int count;         // number of resulting states
    struct edge **edges;        // lists of edges to fix, one for each worker
    unsigned int node_id;       // node_ids to use for resulting states
    struct failure *failures;   // list of failures

    char *alloc_buf;            // allocated buffer
    char *alloc_ptr;            // pointer into allocated buffer
    unsigned long allocated;    // keeps track of how much was allocated

    struct allocator allocator; // mostly for hashdict

    unsigned int *profile;      // one integer for every instruction in the HVM code

    void *scc_cache;            // for SCC alloc/free

    // These need to be next to one another
    struct context ctx;
    hvalue_t stack[MAX_CONTEXT_STACK];
};

// Per thread one-time memory allocator (no free())
static void *walloc(void *ctx, unsigned int size, bool zero){
    struct worker *w = ctx;

    w->allocated += size;
    if (size > WALLOC_CHUNK) {
        return zero ? calloc(1, size) : malloc(size);
    }
    size = (size + 0xF) & ~0xF;     // align to 16 bytes
    if (w->alloc_ptr + size > w->alloc_buf + WALLOC_CHUNK) {
        w->alloc_buf = malloc(WALLOC_CHUNK);
        w->alloc_ptr = w->alloc_buf;
    }
    void *result = w->alloc_ptr;
    w->alloc_ptr += size;
    if (zero) {
        memset(result, 0, size);
    }
    return result;
}

static void run_thread(struct global *global, struct state *state, struct context *ctx){
    struct step step;
    memset(&step, 0, sizeof(step));
    step.ctx = ctx;
    step.engine.values = &global->values;

    for (;;) {
        int pc = step.ctx->pc;
        struct instr *instrs = global->code.instrs;
        struct op_info *oi = instrs[pc].oi;
        (*oi->op)(instrs[pc].env, state, &step, global);
        if (step.ctx->terminated) {
            break;
        }
        if (step.ctx->failed) {
            char *s = value_string(ctx_failure(step.ctx));
            printf("Failure: %s\n", s);
            free(s);
            break;
        }
        if (step.ctx->stopped) {
            printf("Context has stopped\n");
            break;
        }

        if (step.ctx->pc == pc) {
            fprintf(stderr, ">>> %s\n", oi->name);
        }
        assert(step.ctx->pc != pc);
		assert(step.ctx->pc >= 0);
		assert(step.ctx->pc < global->code.len);
    }

    mutex_acquire(&run_mutex);
    if (--run_count == 0) {
        mutex_release(&run_waiting);
    }
    mutex_release(&run_mutex);
}

static void wrap_thread(void *arg){
    struct spawn_info *si = arg;
    run_thread(si->global, si->state, si->ctx);
}

void spawn_thread(struct global *global, struct state *state, struct context *ctx){
    mutex_acquire(&run_mutex);
    run_count++;
    mutex_release(&run_mutex);

    struct spawn_info *si = new_alloc(struct spawn_info);
    si->global = global;
    si->state = state;
    si->ctx = ctx;
    thread_create(wrap_thread, si);
}

// Similar to onestep, but just for some invariant
bool invariant_check(struct global *global, struct state *sc, struct step *step){
    assert(!step->ctx->failed);
    assert(step->ctx->sp == 1);     // just the (pre, post) argument
    while (!step->ctx->terminated) {
        struct op_info *oi = global->code.instrs[step->ctx->pc].oi;
        (*oi->op)(global->code.instrs[step->ctx->pc].env, sc, step, global);
        if (step->ctx->failed) {
            step->ctx->sp = 0;
            return false;
        }
    }
    assert(step->ctx->sp == 1);     // result
    value_ctx_pop(step->ctx);
    return true;
}

// Returns 0 if there are no issues, or the pc of the invariant if it failed.
unsigned int check_invariants(struct worker *w, struct node *node,
                        struct node *before, struct step *step){
    struct global *global = w->global;
    struct state *state = node->state;

    assert(step->ctx->sp == 0);

    // pre == 0 means it is a non-initialized state.
    hvalue_t args[2];   // (pre, post)
    if (before->state->pre == 0) {
        args[0] = state->vars;
    }
    else {
        args[0] = before->state->pre;
    }
    args[1] = state->vars;

    // Check each invariant
    for (unsigned int i = 0; i < global->ninvs; i++) {
        // No need to check edges other than self-loops
        if (!global->invs[i].pre && node != before) {
            continue;
        }

        assert(step->ctx->sp == 0);
        step->ctx->terminated = step->ctx->failed = false;
        ctx_failure(step->ctx) = 0;
        step->ctx->pc = global->invs[i].pc;
        step->ctx->vars = VALUE_DICT;
        value_ctx_push(step->ctx, value_put_list(&step->engine, args, sizeof(args)));

        assert(strcmp(global->code.instrs[step->ctx->pc].oi->name, "Frame") == 0);
        bool b = invariant_check(global, state, step);
        if (step->ctx->failed) {
            // printf("Invariant evaluation failed: %s\n", value_string(ctx_failure(step->ctx)));
            b = false;
        }
        if (!b) {
            // printf("INV %u %u failed\n", i, global->invs[i].pc);
            return global->invs[i].pc;
        }
    }
    return 0;
}

// For tracking data races
static struct access_info *ai_alloc(struct worker *w, int multiplicity,
                        int atomic, int pc) {
    struct access_info *ai = walloc(w, sizeof(*ai), true);
    ai->multiplicity = multiplicity;
    ai->atomic = atomic;
    ai->pc = pc;
    return ai;
}

static bool onestep(
    struct worker *w,       // thread info
    struct node *node,      // starting node
    struct state *sc,       // actual state
    hvalue_t ctx,           // context identifier
    struct step *step,      // step info
    hvalue_t choice,        // if about to make a choice, which choice?
    bool interrupt,         // start with invoking interrupt handler
    bool infloop_detect,    // try to detect infloop from the start
    int multiplicity        // #contexts that are in the current state
) {
    assert(!step->ctx->terminated);
    assert(!step->ctx->failed);
    assert(step->engine.allocator == &w->allocator);

    struct global *global = w->global;

    // See if we should also try an interrupt.
    if (interrupt) {
        assert(step->ctx->extended);
		assert(ctx_trap_pc(step->ctx) != 0);
        interrupt_invoke(step);
    }

    // Copy the choice
    hvalue_t choice_copy = choice;

    bool choosing = false, infinite_loop = false;
    struct dict *infloop = NULL;        // infinite loop detector
    unsigned int instrcnt = 0;
#ifdef HEAP_ALLOC
    char *as_state = malloc(sizeof(struct state) + MAX_CONTEXT_BAG * (sizeof(hvalue_t) + 1));
#else
    char as_state[sizeof(struct state) + MAX_CONTEXT_BAG * (sizeof(hvalue_t) + 1)];
#endif
    hvalue_t as_context = 0;
    unsigned int as_instrcnt = 0;
    bool rollback = false, failure = false, stopped = false;
    bool terminated = false;
    for (;;) {
        int pc = step->ctx->pc;

        // If I'm pthread 0 and it's time, print some stats
        if (w->index == 0 && w->timecnt-- == 0) {
            double now = gettime();
            if (now - global->lasttime > 1) {
                if (global->lasttime != 0) {
                    unsigned int enqueued = 0, dequeued = 0;
                    unsigned long allocated = 0;
                    for (unsigned int i = 0; i < w->nworkers; i++) {
                        struct worker *w2 = &w->workers[i];
                        enqueued += w2->enqueued;
                        dequeued += w2->dequeued;
                        allocated += w2->allocated;
                    }
                    double gigs = (double) allocated / (1 << 30);
#ifdef INCLUDE_RATE
                    fprintf(stderr, "pc=%d states=%u diam=%u q=%d rate=%d mem=%.2lfGB\n",
                            step->ctx->pc, enqueued, global->diameter, enqueued - dequeued,
                            (unsigned int) ((enqueued - global->last_nstates) / (now - global->lasttime)),
                            gigs);
#else
                    fprintf(stderr, "pc=%d states=%u diam=%u q=%d mem=%.2lfGB\n",
                            step->ctx->pc, enqueued, global->diameter,
                            enqueued - dequeued, gigs);
#endif
                    global->last_nstates = enqueued;
                }
                global->lasttime = now;
                if (now > w->timeout) {
                    fprintf(stderr, "charm: timeout exceeded\n");
                    exit(1);
                }
            }
            w->timecnt = 100;
        }

        w->profile[pc]++;       // for profiling
        struct instr *instrs = global->code.instrs;
        struct op_info *oi = instrs[pc].oi;
        if (instrs[pc].choose) {
            assert(step->ctx->sp > 0);
            assert(choice != 0);
            ctx_stack(step->ctx)[step->ctx->sp - 1] = choice;
            step->ctx->pc++;
        }
        else if (instrs[pc].atomicinc) {
            if (instrcnt == 0) {
                step->ctx->atomicFlag = true;
            }
            else if (step->ctx->atomic == 0) {
                // Save the current state in case it needs restoring
                memcpy(as_state, sc, state_size(sc));
                as_context = value_put_context(&step->engine, step->ctx);
                as_instrcnt = instrcnt;
            }
            (*oi->op)(instrs[pc].env, sc, step, global);
        }
        else if (instrs[pc].atomicdec) {
            (*oi->op)(instrs[pc].env, sc, step, global);
            if (step->ctx->atomic == 0) {
                as_context = 0;
                as_instrcnt = 0;
            }
        }
        else {
            // Keep track of access for data race detection
            if (instrs[pc].load || instrs[pc].store || instrs[pc].del) {
                struct access_info *ai = ai_alloc(w, multiplicity, step->ctx->atomic, pc);
                ai->next = step->ai;
                step->ai = ai;
            }
            assert(step->engine.allocator == &w->allocator);
            (*oi->op)(instrs[pc].env, sc, step, global);
            assert(step->engine.allocator == &w->allocator);
        }
		assert(step->ctx->pc >= 0);
		assert(step->ctx->pc < global->code.len);

        instrcnt++;

        if (step->ctx->terminated) {
            terminated = true;
            break;
        }
        if (step->ctx->failed) {
            failure = true;
            break;
        }
        if (step->ctx->stopped) {
            stopped = true;
            break;
        }

        if (infloop_detect || instrcnt > 1000) {
            if (infloop == NULL) {
                infloop = dict_new("infloop1", 0, 0, 0, NULL, NULL);
            }

            int ctxsize = ctx_size(step->ctx);
            int combosize = ctxsize + state_size(sc);
            char *combo = calloc(1, combosize);
            memcpy(combo, step->ctx, ctxsize);
            memcpy(combo + ctxsize, sc, state_size(sc));
            bool new;
            dict_insert(infloop, NULL, combo, combosize, &new);
            free(combo);
            if (!new) {
                if (infloop_detect) {
                    value_ctx_failure(step->ctx, &step->engine, "infinite loop");
                    failure = infinite_loop = true;
                    break;
                }
                else {
                    // start over, as twostep does not have instrcnt optimization
                    return false;
                }
            }
        }

        if (step->ctx->pc == pc) {
            fprintf(stderr, ">>> %s\n", oi->name);
        }
        assert(step->ctx->pc != pc);
		assert(step->ctx->pc >= 0);
		assert(step->ctx->pc < global->code.len);

        /* Peek at the next instruction.
         */
        struct instr *next_instr = &global->code.instrs[step->ctx->pc];
        if (next_instr->choose) {
            assert(step->ctx->sp > 0);
#ifdef TODO
            if (0 && step->ctx->readonly > 0) {    // TODO
                value_ctx_failure(step->ctx, &step->engine, "can't choose in assertion or invariant");
                instrcnt++;
                failure = true;
                break;
            }
#endif
            hvalue_t s = ctx_stack(step->ctx)[step->ctx->sp - 1];
            if (VALUE_TYPE(s) != VALUE_SET) {
                value_ctx_failure(step->ctx, &step->engine, "choose operation requires a set");
                instrcnt++;
                failure = true;
                break;
            }
            unsigned int size;
#ifdef OBSOLETE
            hvalue_t *vals =
#endif
            value_get(s, &size);
            size /= sizeof(hvalue_t);
            if (size == 0) {
                value_ctx_failure(step->ctx, &step->engine, "choose operation requires a non-empty set");
                instrcnt++;
                failure = true;
                break;
            }
            if (step->ctx->atomic > 0 && !step->ctx->atomicFlag) {
                rollback = true;
            }
            else {
                choosing = true;
            }
            break;
        }

        // See if we need to break out of this step.  If the atomicFlag is
        // set, then definitely not.  If it is not set, then it gets
        // complicated.  If the atomic count > 0, then we may have delayed
        // breaking until strictly necessary (lazy atomic), in the hopes
        // of not having to at all (because breaking causes an expensive
        // context switch).  If the instruction is not "breakable" (Load,
        // Store, Del, Print, eager AtomicInc), then there's no need to
        // break yet.  Otherwise, if the atomic count > 0, we should set
        // the atomicFlag and break.  Otherwise  if it's a breakable
        // instruction, we should just break.
        else if (!step->ctx->atomicFlag) {
            bool breakable = next_instr->breakable;

            // If this is a Load operation, it's only breakable if it
            // accesses a global variable
            // TODO.  Can this be made more efficient?
            if (next_instr->load && next_instr->env == NULL) {
                hvalue_t addr = ctx_stack(step->ctx)[step->ctx->sp - 1];
                assert(VALUE_TYPE(addr) == VALUE_ADDRESS);
                assert(addr != VALUE_ADDRESS);
                hvalue_t *func = value_get(addr, NULL);
                if (*func != VALUE_TO_PC(-1)) {
                    breakable = false;
                }
            }

            // Deal with interrupts if enabled
            if (step->ctx->extended && ctx_trap_pc(step->ctx) != 0 &&
                                !step->ctx->interruptlevel) {
                // If this is a thread exit, break so we can invoke the
                // interrupt handler one more time
                if (next_instr->retop && step->ctx->sp == 1) {
                    breakable = true;
                }

                // If this is a setintlevel(True), should try interrupt
                // For simplicity, always try when setintlevel
                else if (next_instr->setintlevel) {
                    breakable = true;
                }
            }

            if (breakable) {
                // If the step is breakable and we're in an atomic section,
                // we should have broken at the beginning of the atomic
                // section.  Restore that state.
                if (step->ctx->atomic > 0 && !step->ctx->atomicFlag) {
                    rollback = true;
                }
                break;
            }
        }
    }

    if (infloop != NULL) {
        dict_delete(infloop);
    }

    hvalue_t after;
    if (rollback) {
        struct state *state = (struct state *) as_state;
        memcpy(sc, state, state_size(state));
        after = as_context;
        instrcnt = as_instrcnt;
    }
    else {
        // Store new context in value directory.  Must be immutable now.
        after = value_put_context(&step->engine, step->ctx);
    }

#ifdef HEAP_ALLOC
    free(as_state);
#endif

    // Remove old context from the bag
    context_remove(sc, ctx);

    // If choosing, save in state.  If some invariant uses "pre", then
    // also keep track of "pre" state.
    if (choosing) {
        sc->choosing = after;
        sc->pre = global->inv_pre ? node->state->pre : sc->vars;
    }
    else {
        sc->pre = sc->vars;
    }

    // Add new context to state unless it's terminated or stopped
    if (stopped) {
        sc->stopbag = value_bag_add(&step->engine, sc->stopbag, after, 1);
    }
    else if (!terminated) {
        context_add(sc, after);
    }

    // Weight of this step
    unsigned int weight = (node->to_parent == NULL || ctx == node->to_parent->after) ? 0 : 1;

    // Allocate edge now
    struct edge *edge = walloc(w, sizeof(struct edge) + step->nlog * sizeof(hvalue_t), false);
    edge->src = node;
    edge->ctx = ctx;
    edge->choice = choice_copy;
    edge->interrupt = interrupt;
    edge->weight = weight;
    edge->after = after;
    edge->ai = step->ai;
    memcpy(edge_log(edge), step->log, step->nlog * sizeof(hvalue_t));
    edge->nlog = step->nlog;
    edge->nsteps = instrcnt;

    // See if this state has been computed before
    bool new;
    mutex_t *lock;
    unsigned int size = state_size(sc);
    struct dict_assoc *da = dict_find_lock(w->visited, &w->allocator,
                sc, size, &new, &lock);
    struct state *state = (struct state *) &da[1];
    struct node *next = (struct node *) ((char *) state + size);
    if (new) {
        memset(next, 0, sizeof(*next));
        next->len = node->len + weight;
        next->steps = node->steps + instrcnt;
        next->to_parent = edge;
        next->state = state;
    }
    else {
        unsigned int len = node->len + weight;
        unsigned int steps = node->steps + instrcnt;
        // TODO: not sure how to minimize.  For some cases, this works better than
        //   if (len < next->len || (len == next->len && steps < next->steps)) {
        if (len < next->len || (len == next->len && steps <= next->steps)) {
            next->len = len;
            next->steps = steps;
            next->to_parent = edge;
        }
    }

    // Backward edge from node to parent.
    edge->bwdnext = next->bwd;
    next->bwd = edge;

    mutex_release(lock);

    // Don't do the forward edge at this time as that would involve lockng
    // the parent node.  Instead assign that task to one of the workers
    // in the next phase.
    struct edge **pe = &w->edges[node->id % w->nworkers];
    edge->fwdnext = *pe;
    *pe = edge;
    edge->dst = next;

    if (new) {
        next->next = w->results;
        w->results = next;
        w->count++;
        w->enqueued++;
    }

    if (failure) {
        struct failure *f = new_alloc(struct failure);
        f->type = infinite_loop ? FAIL_TERMINATION : FAIL_SAFETY;
        f->edge = edge;
        f->next = w->failures;
        w->failures = f;
    }
    else if (sc->choosing == 0 && global->ninvs != 0) {
        unsigned int inv = 0;
        if (new) {      // try self-loop if a new node
            inv = check_invariants(w, next, next, &w->inv_step);
        }
        if (inv == 0) { // try new edge
            inv = check_invariants(w, next, node, &w->inv_step);
        }
        if (inv != 0) {
            struct failure *f = new_alloc(struct failure);
            f->type = FAIL_INVARIANT;
            f->edge = edge;
            f->next = w->failures;
            f->address = VALUE_TO_PC(inv);
            w->failures = f;
        }
    }

    // We stole the access info and log
    step->ai = NULL;
    // TODO step->log = NULL;
    step->nlog = 0;

    return true;
}

static void make_step(
    struct worker *w,
    struct node *node,
    hvalue_t ctx,
    hvalue_t choice,       // if about to make a choice, which choice?
    int multiplicity       // #contexts that are in the current state
) {
    struct step step;
    memset(&step, 0, sizeof(step));
    step.engine.allocator = &w->allocator;
    step.engine.values = &w->global->values;

    // Make a copy of the state
    unsigned int statesz = state_size(node->state);
    // Room to grown in copy for op_Spawn
#ifdef HEAP_ALLOC
    char *copy = malloc(statesz + 64*sizeof(hvalue_t));
#else
    char copy[statesz + 64*sizeof(hvalue_t)];
#endif
    struct state *sc = (struct state *) copy;
    memcpy(sc, node->state, statesz);
    assert(step.engine.allocator == &w->allocator);

    // Make a copy of the context
    unsigned int size;
    struct context *cc = value_get(ctx, &size);
    assert(ctx_size(cc) == size);
    memcpy(&w->ctx, cc, size);
    assert(step.engine.allocator == &w->allocator);
    step.ctx = &w->ctx;

    // See if we need to interrupt
    if (sc->choosing == 0 && cc->extended && ctx_trap_pc(cc) != 0 && !cc->interruptlevel) {
        bool succ = onestep(w, node, sc, ctx, &step, choice, true, false, multiplicity);
        assert(step.engine.allocator == &w->allocator);
        if (!succ) {        // ran into an infinite loop
            memcpy(sc, node->state, statesz);
            memcpy(&w->ctx, cc, size);
            assert(step.engine.allocator == &w->allocator);
            (void) onestep(w, node, sc, ctx, &step, choice, true, true, multiplicity);
            assert(step.engine.allocator == &w->allocator);
        }

        memcpy(sc, node->state, statesz);
        memcpy(&w->ctx, cc, size);
        assert(step.engine.allocator == &w->allocator);
    }

    sc->choosing = 0;
    bool succ = onestep(w, node, sc, ctx, &step, choice, false, false, multiplicity);
    assert(step.engine.allocator == &w->allocator);
    if (!succ) {        // ran into an infinite loop
        memcpy(sc, node->state, statesz);
        memcpy(&w->ctx, cc, size);
        assert(step.engine.allocator == &w->allocator);
        (void) onestep(w, node, sc, ctx, &step, choice, false, true, multiplicity);
        assert(step.engine.allocator == &w->allocator);
    }

#ifdef HEAP_ALLOC
    free(copy);
#endif
}

char *ctx_status(struct node *node, hvalue_t ctx) {
    if (node->state->choosing == ctx) {
        return "choosing";
    }
    while (node->state->choosing != 0) {
        node = node->to_parent->src;
    }
    struct edge *edge;
    for (edge = node->fwd; edge != NULL; edge = edge->fwdnext) {
        if (edge->ctx == ctx) {
            break;
        }
    }
    if (edge != NULL && edge->dst == node) {
        return "blocked";
    }
    return "runnable";
}

void print_context(
    struct global *global,
    FILE *file,
    hvalue_t ctx,
    struct callstack *cs,
    int tid,
    struct node *node,
    char *prefix
) {
    fprintf(file, "%s\"tid\": \"%d\",\n", prefix, tid);
    fprintf(file, "%s\"hvalue\": \"%"PRI_HVAL"\",\n", prefix, ctx);

    unsigned int size;
    struct context *c = value_get(ctx, &size);

    fprintf(file, "%s\"fp\": \"%d\",\n", prefix, cs->sp + 1);

    struct callstack *ecs = cs;
    while (ecs->parent != NULL) {
        ecs = ecs->parent;
    }

    assert(strcmp(global->code.instrs[ecs->pc].oi->name, "Frame") == 0);
    const struct env_Frame *ef = global->code.instrs[ecs->pc].env;
    char *s = value_string(ef->name);
	int len = strlen(s);
    char *a = json_escape_value(ecs->arg);
    if (*a == '(') {
        fprintf(file, "%s\"name\": \"%.*s%s\",\n", prefix, len - 2, s + 1, a);
    }
    else {
        fprintf(file, "%s\"name\": \"%.*s(%s)\",\n", prefix, len - 2, s + 1, a);
    }
    free(s);
    free(a);

    // TODO.  Backwards compatibility
    struct callstack *lcs = cs;
    while (lcs->parent != NULL) {
        lcs = lcs->parent;
    }
    fprintf(file, "%s\"entry\": \"%u\",\n", prefix, lcs->pc);

    fprintf(file, "%s\"pc\": \"%u\",\n", prefix, c->pc);
    fprintf(file, "%s\"sp\": \"%u\",\n", prefix, c->sp);

    fprintf(file, "%s\"stack\": [", prefix);
    for (unsigned int x = cs->sp; x < c->sp; x++) {
        if (x != cs->sp) {
            fprintf(file, ", ");
        }
        char *v = value_json(ctx_stack(c)[x], global);
        fprintf(file, "%s", v);
        free(v);
    }
    fprintf(file, "],\n");

    fprintf(file, "%s\"trace\": [\n", prefix);
    value_trace(global, file, cs, c->pc, c->vars, prefix);
    fprintf(file, "\n");
    fprintf(file, "%s],\n", prefix);

    if (c->failed) {
        s = value_string(ctx_failure(c));
        fprintf(file, "%s\"failure\": %s,\n", prefix, s);
        free(s);
    }

    if (c->extended && ctx_trap_pc(c) != 0) {
        s = value_string(ctx_trap_pc(c));
        a = value_string(ctx_trap_arg(c));
        if (*a == '(') {
            fprintf(file, "%s\"trap\": \"%s%s\",\n", prefix, s, a);
        }
        else {
            fprintf(file, "%s\"trap\": \"%s(%s)\",\n", prefix, s, a);
        }
        free(a);
        free(s);
    }

    if (c->interruptlevel) {
        fprintf(file, "%s\"interruptlevel\": \"1\",\n", prefix);
    }

    if (c->extended) {
        s = value_json(ctx_this(c), global);
        fprintf(file, "%s\"this\": %s,\n", prefix, s);
        free(s);
    }

    if (c->atomic != 0) {
        fprintf(file, "%s\"atomic\": \"%d\",\n", prefix, c->atomic);
    }
    if (c->readonly != 0) {
        fprintf(file, "%s\"readonly\": \"%d\",\n", prefix, c->readonly);
    }
    if (!c->terminated && !c->failed) {
        struct instr *instr = &global->code.instrs[c->pc];
        struct op_info *oi = instr->oi;
        if (oi->next == NULL) {
            fprintf(file, "%s\"next\": { \"type\": \"%s\" },\n", prefix, oi->name);
        }
        else {
            fprintf(file, "%s\"next\": ", prefix);
            (*oi->next)(instr->env, c, global, file);
            fprintf(file, ",\n");
        }
    }

    if (c->terminated) {
        fprintf(file, "%s\"mode\": \"terminated\"", prefix);
    }
    else {
        if (c->failed) {
            fprintf(file, "%s\"mode\": \"failed\"", prefix);
        }
        else if (c->stopped) {
            fprintf(file, "%s\"mode\": \"stopped\"", prefix);
        }
        else {
            fprintf(file, "%s\"mode\": \"%s\"", prefix, ctx_status(node, ctx));
        }
    }
    fprintf(file, "\n");

#ifdef notdef
    fprintf(file, "          \"stack\": [\n");
    for (int i = 0; i < c->sp; i++) {
        s = value_string(ctx_stack(k)[i]);
        if (i < c->sp - 1) {
            fprintf(file, "            \"%s\",\n", s);
        }
        else {
            fprintf(file, "            \"%s\"\n", s);
        }
        free(s);
    }
    fprintf(file, "          ],\n");
#endif
}

#ifdef PATH_DUMP

void print_state(
    struct global *global,
    FILE *file,
    struct node *node
) {
    fprintf(file, "      \"ctxbag\": {\n");
    for (unsigned int i = 0; i < node->state->bagsize; i++) {
        if (i > 0) {
            fprintf(file, ",\n");
        }
        assert(VALUE_TYPE(state_contexts(node->state)[i]) == VALUE_CONTEXT);
        fprintf(file, "          \"%"PRIx64"\": \"%u\"", state_contexts(node->state)[i],
                multiplicities(node->state)[i]);
    }
    fprintf(file, "\n      },\n");

    fprintf(file, "      \"contexts\": [\n");
    for (unsigned int i = 0; i < global->nprocesses; i++) {
        fprintf(file, "        {\n");
        print_context(global, file, global->processes[i], global->callstacks[i], i, node, "          ");
        fprintf(file, "        }");
        if (i < global->nprocesses - 1) {
            fprintf(file, ",");
        }
        fprintf(file, "\n");
    }
    fprintf(file, "      ]\n");
}

void diff_state(
    struct global *global,
    FILE *file,
    struct state *oldstate,
    struct state *newstate,
    struct context *oldctx,
    struct context *newctx,
    struct callstack *oldcs,
    struct callstack *newcs,
    bool interrupt,
    bool choose,
    hvalue_t choice,
    char *print,
    struct step *step
) {
    if (global->dumpfirst) {
        global->dumpfirst = false;
    }
    else {
        fprintf(file, ",");
    }
    fprintf(file, "\n        {\n");
    struct json_value *next = global->pretty->u.list.vals[oldctx->pc];
    assert(next->type == JV_LIST);
    assert(next->u.list.nvals == 2);
    struct json_value *opstr = next->u.list.vals[0];
    assert(opstr->type == JV_ATOM);
    char *op = json_escape(opstr->u.atom.base, opstr->u.atom.len);
    fprintf(file, "          \"code\": \"%s\",\n", op);
    free(op);
    if (strbuf_getlen(&step->explain) > 0) {
        char *v = json_escape(step->explain.buf, step->explain.len);
        fprintf(file, "          \"explain\": \"%s\",\n", v);
        free(v);
        step->explain.len = 0;
    }
    else {
        struct json_value *codestr = next->u.list.vals[1];
        assert(codestr->type == JV_ATOM);
		char *v = json_escape(codestr->u.atom.base, codestr->u.atom.len);
        fprintf(file, "          \"explain\": \"%s\",\n", v);
        free(v);
    }
    if (newstate->vars != oldstate->vars) {
        fprintf(file, "          \"shared\": ");
        print_vars(global, file, newstate->vars);
        fprintf(file, ",\n");
    }
    if (interrupt) {
        fprintf(file, "          \"interrupt\": \"True\",\n");
    }
    if (choose) {
        char *val = value_json(choice, global);
        fprintf(file, "          \"choose\": %s,\n", val);
        free(val);
    }
    if (print != NULL) {
        fprintf(file, "          \"print\": %s,\n", print);
    }
    fprintf(file, "          \"npc\": \"%d\",\n", newctx->pc);
    if (newcs != NULL && newcs != oldcs) {
        fprintf(file, "          \"fp\": \"%d\",\n", newcs->sp + 1);
#ifdef notdef
        {
            fprintf(stderr, "STACK2 %d:\n", newctx->fp);
            for (int x = 0; x < newctx->sp; x++) {
                fprintf(stderr, "    %d: %s\n", x, value_string(ctx_stack(newctx)[x]));
            }
        }
#endif

        fprintf(file, "          \"trace\": [\n");
        value_trace(global, file, newcs, newctx->pc, newctx->vars, "          ");
        fprintf(file, "\n");
        fprintf(file, "          ],\n");
    }
    // TODO.  Shouldn't this check if the oldctx is also extended?
    if (newctx->extended && ctx_this(newctx) != ctx_this(oldctx)) {
        char *val = value_json(ctx_this(newctx), global);
        fprintf(file, "          \"this\": %s,\n", val);
        free(val);
    }
    if (newctx->vars != oldctx->vars) {
        fprintf(file, "          \"local\": ");
        print_vars(global, file, newctx->vars);
        fprintf(file, ",\n");
    }
    if (newctx->atomic != oldctx->atomic) {
        fprintf(file, "          \"atomic\": \"%d\",\n", newctx->atomic);
    }
    if (newctx->readonly != oldctx->readonly) {
        fprintf(file, "          \"readonly\": \"%d\",\n", newctx->readonly);
    }
    if (newctx->interruptlevel != oldctx->interruptlevel) {
        fprintf(file, "          \"interruptlevel\": \"%d\",\n", newctx->interruptlevel ? 1 : 0);
    }
    if (newctx->failed) {
        char *val = value_string(ctx_failure(newctx));
        fprintf(file, "          \"failure\": %s,\n", val);
        fprintf(file, "          \"mode\": \"failed\",\n");
        free(val);
    }
    else if (newctx->terminated) {
        fprintf(file, "          \"mode\": \"terminated\",\n");
    }

    unsigned int common;
    for (common = 0; common < newctx->sp && common < oldctx->sp; common++) {
        if (ctx_stack(newctx)[common] != ctx_stack(oldctx)[common]) {
            break;
        }
    }
    if (common < oldctx->sp) {
        fprintf(file, "          \"pop\": \"%d\",\n", oldctx->sp - common);
    }
    fprintf(file, "          \"push\": [");
    for (unsigned int i = common; i < newctx->sp; i++) {
        if (i > common) {
            fprintf(file, ",");
        }
        char *val = value_json(ctx_stack(newctx)[i], global);
        fprintf(file, " %s", val);
        free(val);
    }
    fprintf(file, " ],\n");

    unsigned int bs = oldstate->bagsize * (sizeof(hvalue_t) + 1);
    if (oldstate->bagsize != newstate->bagsize ||
            memcmp(state_contexts(oldstate), state_contexts(newstate), bs) != 0) {
        fprintf(file, "          \"contexts\": \"%d\",\n", newstate->bagsize);
    }

    fprintf(file, "          \"pc\": \"%d\"\n", oldctx->pc);

    fprintf(file, "        }");
}

void diff_dump(
    struct global *global,
    FILE *file,
    struct state *oldstate,
    struct state *newstate,
    struct context *oldctx,
    struct context *newctx,
    struct callstack **oldcs,
    struct callstack *newcs,
    bool interrupt,
    bool choose,
    hvalue_t choice,
    char *print,
    struct step *step
) {
    unsigned int oldsize = ctx_size(oldctx);
    unsigned int newsize = ctx_size(newctx);
    if (memcmp(oldstate, newstate, sizeof(struct state)) == 0 &&
            newsize == oldsize &&
            memcmp(oldctx, newctx, newsize) == 0) {
        return;
    }

    // Keep track of old state and context for taking diffs
    diff_state(global, file, oldstate, newstate, oldctx, newctx, *oldcs, newcs, interrupt, choose, choice, print, step);
    memcpy(oldstate, newstate, state_size(newstate));
    memcpy(oldctx, newctx, newsize);
    *oldcs = newcs;
}

// similar to onestep.
void twostep(
    struct global *global,
    FILE *file,
    struct node *node,
    hvalue_t ctx,
    struct callstack *cs,
    hvalue_t choice,
    bool interrupt,
    struct state *oldstate,
    struct context *oldctx,
    hvalue_t nextvars,
    unsigned int nsteps,
    unsigned int pid
){
    // Make a copy of the state
    struct state *sc = calloc(1, sizeof(struct state) + MAX_CONTEXT_BAG * (sizeof(hvalue_t) + 1));
    memcpy(sc, node->state, state_size(node->state));
    sc->choosing = 0;

    struct step step;
    memset(&step, 0, sizeof(step));
    step.keep_callstack = true;
    step.engine.values = &global->values;
    step.callstack = cs;
    strbuf_init(&step.explain);

    unsigned int size;
    struct context *cc = value_get(ctx, &size);
    step.ctx = calloc(1, sizeof(struct context) +
                            MAX_CONTEXT_STACK * sizeof(hvalue_t));
    memcpy(step.ctx, cc, size);
    if (step.ctx->terminated || step.ctx->failed) {
        panic("twostep: already terminated???");
    }

    struct callstack *oldcs = NULL;

    if (interrupt) {
        assert(step.ctx->extended);
		assert(ctx_trap_pc(step.ctx) != 0);
        interrupt_invoke(&step);
        diff_dump(global, file, oldstate, sc, oldctx, step.ctx, &oldcs, step.callstack, true, false, 0, NULL, &step);
    }

    struct dict *infloop = NULL;        // infinite loop detector
    unsigned int instrcnt = 0;
    for (;;) {
        int pc = step.ctx->pc;

        char *print = NULL;
        struct instr *instrs = global->code.instrs;
        struct op_info *oi = instrs[pc].oi;
        if (instrs[pc].choose) {
            assert(choice != 0);
            char *set = value_string(ctx_stack(step.ctx)[step.ctx->sp - 1]);
            char *sel = value_string(choice);
            strbuf_printf(&step.explain, "replace top of stack (%s) with choice (%s)", set, sel);
            free(set);
            free(sel);
            ctx_stack(step.ctx)[step.ctx->sp - 1] = choice;
            step.ctx->pc++;
        }
        else if (instrs[pc].atomicinc) {
            if (instrcnt == 0) {
                step.ctx->atomicFlag = true;
            }
            (*oi->op)(instrs[pc].env, sc, &step, global);
        }
        else if (instrs[pc].print) {
            print = value_json(ctx_stack(step.ctx)[step.ctx->sp - 1], global);
            (*oi->op)(instrs[pc].env, sc, &step, global);
        }
        else {
            (*oi->op)(instrs[pc].env, sc, &step, global);
        }

        // Infinite loop detection
        if (!step.ctx->terminated && !step.ctx->failed) {
            if (infloop == NULL) {
                infloop = dict_new("infloop2", 0, 0, 0, NULL, NULL);
            }

            int ctxsize = sizeof(struct context) + step.ctx->sp * sizeof(hvalue_t);
            if (step.ctx->extended) {
                ctxsize += ctx_extent * sizeof(hvalue_t);
            }
            int combosize = ctxsize + state_size(sc);
            char *combo = calloc(1, combosize);
            memcpy(combo, step.ctx, ctxsize);
            memcpy(combo + ctxsize, sc, state_size(sc));
            bool new;
            dict_insert(infloop, NULL, combo, combosize, &new);
            free(combo);
            if (!new) {
                value_ctx_failure(step.ctx, &step.engine, "infinite loop");
            }
        }

        assert(!instrs[pc].choose || choice != 0);
        diff_dump(global, file, oldstate, sc, oldctx, step.ctx, &oldcs, step.callstack, false, instrs[pc].choose, choice, print, &step);
        free(print);
        if (step.ctx->terminated || step.ctx->failed || step.ctx->stopped) {
            break;
        }
        instrcnt++;
        if (instrcnt >= nsteps) {
            break;
        }
        if (step.ctx->pc == pc) {
            fprintf(stderr, ">>> %s\n", oi->name);
        }
        assert(step.ctx->pc != pc);

        /* Peek at the next instruction.
         */
        oi = global->code.instrs[step.ctx->pc].oi;
        if (global->code.instrs[step.ctx->pc].choose) {
            assert(step.ctx->sp > 0);
#ifdef TODO
            if (0 && step.ctx->readonly > 0) {    // TODO
                value_ctx_failure(step.ctx, &step.engine, "can't choose in assertion or invariant");
                diff_dump(global, file, oldstate, sc, oldctx, step.ctx, &oldcs, step.callstack, false, global->code.instrs[pc].choose, choice, NULL, &step);
                break;
            }
#endif
            hvalue_t s = ctx_stack(step.ctx)[step.ctx->sp - 1];
            if (VALUE_TYPE(s) != VALUE_SET) {
                value_ctx_failure(step.ctx, &step.engine, "choose operation requires a set");
                diff_dump(global, file, oldstate, sc, oldctx, step.ctx, &oldcs, step.callstack, false, global->code.instrs[pc].choose, choice, NULL, &step);
                break;
            }
            unsigned int size;
            hvalue_t *vals = value_get(s, &size);
            size /= sizeof(hvalue_t);
            if (size == 0) {
                value_ctx_failure(step.ctx, &step.engine, "choose operation requires a non-empty set");
                diff_dump(global, file, oldstate, sc, oldctx, step.ctx, &oldcs, step.callstack, false, global->code.instrs[pc].choose, choice, NULL, &step);
                break;
            }
            if (size == 1) {
                choice = vals[0];
            }
            else {
                break;
            }
        }
    }

    // Remove old context from the bag
    context_remove(sc, ctx);

    hvalue_t after = value_put_context(&step.engine, step.ctx);

    // Add new context to state unless it's terminated or stopped
    if (step.ctx->stopped) {
        sc->stopbag = value_bag_add(&step.engine, sc->stopbag, after, 1);
    }
    else if (!step.ctx->terminated) {
        context_add(sc, after);
    }

    free(sc);
    free(step.ctx);
    strbuf_deinit(&step.explain);
    // TODO free(step.log);

    global->processes[pid] = after;
    global->callstacks[pid] = step.callstack;
}

#endif // PATH_DUMP

// Save the state and the context of a microstep.
static void make_microstep(
    struct state *newstate,
    struct context *newctx,
    struct callstack *newcs,
    bool interrupt,
    bool choose,
    hvalue_t choice,
    hvalue_t print,
    struct step *step,
    struct macrostep *macro
) {
    struct microstep *micro = calloc(1, sizeof(*micro));

    // Save the current context
    unsigned int cs = ctx_size(newctx);
    micro->ctx = malloc(cs);
    memcpy(micro->ctx, newctx, cs);

    // Save the current state
    unsigned int ss = state_size(newstate);
    micro->state = malloc(ss);
    memcpy(micro->state, newstate, ss);

    micro->interrupt = interrupt;
    micro->choose = choose;
    micro->choice = choice;
    micro->print = print;
    micro->cs = newcs;
    micro->explain = json_escape(step->explain.buf, step->explain.len);
    step->explain.len = 0;

    if (macro->nmicrosteps == macro->alloc_microsteps) {
        macro->alloc_microsteps *= 2;
        if (macro->alloc_microsteps < 64) {
            macro->alloc_microsteps = 64;
        }
        macro->microsteps = realloc(macro->microsteps,
            macro->alloc_microsteps * sizeof(*macro->microsteps));
    }
    macro->microsteps[macro->nmicrosteps++] = micro;
}

// similar to onestep.
void twostep2(
    struct global *global,
    struct node *node,
    hvalue_t ctx,
    struct callstack *cs,
    hvalue_t choice,
    bool interrupt,
    hvalue_t nextvars,
    unsigned int nsteps,
    unsigned int pid,
    struct macrostep *macro
){
    // Make a copy of the state
    struct state *sc = calloc(1, sizeof(struct state) + MAX_CONTEXT_BAG * (sizeof(hvalue_t) + 1));
    memcpy(sc, node->state, state_size(node->state));
    sc->choosing = 0;

    struct step step;
    memset(&step, 0, sizeof(step));
    step.keep_callstack = true;
    step.engine.values = &global->values;
    step.callstack = cs;
    strbuf_init(&step.explain);

    unsigned int size;
    struct context *cc = value_get(ctx, &size);
    step.ctx = calloc(1, sizeof(struct context) +
                            MAX_CONTEXT_STACK * sizeof(hvalue_t));
    memcpy(step.ctx, cc, size);
    if (step.ctx->terminated || step.ctx->failed) {
        panic("twostep: already terminated???");
    }

    if (interrupt) {
        assert(step.ctx->extended);
		assert(ctx_trap_pc(step.ctx) != 0);
        interrupt_invoke(&step);
        make_microstep(sc, step.ctx, step.callstack, true, false, 0, 0, &step, macro);
    }

    struct dict *infloop = NULL;        // infinite loop detector
    unsigned int instrcnt = 0;
    for (;;) {
        int pc = step.ctx->pc;

        hvalue_t print = 0;
        struct instr *instrs = global->code.instrs;
        struct op_info *oi = instrs[pc].oi;
        if (instrs[pc].choose) {
            assert(choice != 0);
            char *set = value_string(ctx_stack(step.ctx)[step.ctx->sp - 1]);
            char *sel = value_string(choice);
            strbuf_printf(&step.explain, "replace top of stack (%s) with choice (%s)", set, sel);
            free(set);
            free(sel);
            ctx_stack(step.ctx)[step.ctx->sp - 1] = choice;
            step.ctx->pc++;
        }
        else if (instrs[pc].atomicinc) {
            if (instrcnt == 0) {
                step.ctx->atomicFlag = true;
            }
            (*oi->op)(instrs[pc].env, sc, &step, global);
        }
        else if (instrs[pc].print) {
            print = ctx_stack(step.ctx)[step.ctx->sp - 1];
            (*oi->op)(instrs[pc].env, sc, &step, global);
        }
        else {
            (*oi->op)(instrs[pc].env, sc, &step, global);
        }

        // Infinite loop detection
        if (!step.ctx->terminated && !step.ctx->failed) {
            if (infloop == NULL) {
                infloop = dict_new("infloop2", 0, 0, 0, NULL, NULL);
            }

            int ctxsize = sizeof(struct context) + step.ctx->sp * sizeof(hvalue_t);
            if (step.ctx->extended) {
                ctxsize += ctx_extent * sizeof(hvalue_t);
            }
            int combosize = ctxsize + state_size(sc);
            char *combo = calloc(1, combosize);
            memcpy(combo, step.ctx, ctxsize);
            memcpy(combo + ctxsize, sc, state_size(sc));
            bool new;
            dict_insert(infloop, NULL, combo, combosize, &new);
            free(combo);
            if (!new) {
                value_ctx_failure(step.ctx, &step.engine, "infinite loop");
            }
        }

        assert(!instrs[pc].choose || choice != 0);
        make_microstep(sc, step.ctx, step.callstack, false, instrs[pc].choose, choice, print, &step, macro);
        if (step.ctx->terminated || step.ctx->failed || step.ctx->stopped) {
            break;
        }
        instrcnt++;
        if (instrcnt >= nsteps) {
            break;
        }
        if (step.ctx->pc == pc) {
            fprintf(stderr, ">>> %s\n", oi->name);
        }
        assert(step.ctx->pc != pc);

        /* Peek at the next instruction.
         */
        oi = global->code.instrs[step.ctx->pc].oi;
        if (global->code.instrs[step.ctx->pc].choose) {
            assert(step.ctx->sp > 0);
#ifdef TODO
            if (0 && step.ctx->readonly > 0) {    // TODO
                value_ctx_failure(step.ctx, &step.engine, "can't choose in assertion or invariant");
                make_microstep(sc, step.ctx, step.callstack, false, global->code.instrs[pc].choose, choice, 0, &step, macro);
                break;
            }
#endif
            hvalue_t s = ctx_stack(step.ctx)[step.ctx->sp - 1];
            if (VALUE_TYPE(s) != VALUE_SET) {
                value_ctx_failure(step.ctx, &step.engine, "choose operation requires a set");
                make_microstep(sc, step.ctx, step.callstack, false, global->code.instrs[pc].choose, choice, 0, &step, macro);
                break;
            }
            unsigned int size;
            hvalue_t *vals = value_get(s, &size);
            size /= sizeof(hvalue_t);
            if (size == 0) {
                value_ctx_failure(step.ctx, &step.engine, "choose operation requires a non-empty set");
                make_microstep(sc, step.ctx, step.callstack, false, global->code.instrs[pc].choose, choice, 0, &step, macro);
                break;
            }
            if (size == 1) {
                choice = vals[0];
            }
            else {
                break;
            }
        }
    }

    // Remove old context from the bag
    context_remove(sc, ctx);

    hvalue_t after = value_put_context(&step.engine, step.ctx);

    // Add new context to state unless it's terminated or stopped
    if (step.ctx->stopped) {
        sc->stopbag = value_bag_add(&step.engine, sc->stopbag, after, 1);
    }
    else if (!step.ctx->terminated) {
        context_add(sc, after);
    }

    free(sc);
    free(step.ctx);
    strbuf_deinit(&step.explain);
    // TODO free(step.log);

    global->processes[pid] = after;
    global->callstacks[pid] = step.callstack;
}

#ifdef PATH_DUMP

// Recursively dump the path as a sequence of macrosteps.  The steps are
// recreated using the twostep() function.  Edge e points to the last edge.
void path_dump(
    struct global *global,
    FILE *file,
    struct edge *e,
    struct state *oldstate,
    struct context *oldctx
) {
    struct node *node = e->dst;
    struct node *parent = e->src;

    if (parent->to_parent == NULL) {
        fprintf(file, "\n");
    }
    else {
        path_dump(global, file, parent->to_parent, oldstate, oldctx);
        fprintf(file, ",\n");
    }

    /* Find the starting context in the list of processes.  Prefer
     * sticking with the same pid if possible.
     */
    hvalue_t ctx = e->ctx;
    unsigned int pid;
    if (global->processes[oldpid] == ctx) {
        pid = oldpid;
    }
    else {
        for (pid = 0; pid < global->nprocesses; pid++) {
            if (global->processes[pid] == ctx) {
                break;
            }
        }
        oldpid = pid;
    }
    assert(pid < global->nprocesses);

    fprintf(file, "    {\n");
    fprintf(file, "      \"id\": \"%d\",\n", node->id);
    fprintf(file, "      \"len\": \"%d\",\n", node->len);

    unsigned int ctxsize;
    struct context *context = value_get(ctx, &ctxsize);
    assert(!context->terminated);

    struct callstack *cs = global->callstacks[pid];
    while (cs->parent != NULL) {
        cs = cs->parent;
    }
    assert(strcmp(global->code.instrs[cs->pc].oi->name, "Frame") == 0);
    const struct env_Frame *ef = global->code.instrs[cs->pc].env;
    char *name = value_string(ef->name);
	int len = strlen(name);
    if (oldstate->vars != 0) {
        fprintf(file, "      \"shared\": ");
        print_vars(global, file, oldstate->vars);
        fprintf(file, ",\n");
    }
    fprintf(file, "      \"tid\": \"%d\",\n", pid);
    char *arg = json_escape_value(cs->arg);
    if (*arg == '(') {
        fprintf(file, "      \"name\": \"%.*s%s\",\n", len - 2, name + 1, arg);
    }
    else {
        fprintf(file, "      \"name\": \"%.*s(%s)\",\n", len - 2, name + 1, arg);
    }
    char *c = e->choice == 0 ? NULL : value_json(e->choice, global);
    if (c != NULL) {
        fprintf(file, "      \"choice\": %s,\n", c);
    }
    global->dumpfirst = true;
    fprintf(file, "      \"context\": {\n");
    print_context(global, file, ctx, cs, pid, node, "        ");
    fprintf(file, "      },\n");

    fprintf(file, "      \"microsteps\": [");
    free(name);
    free(arg);
    free(c);
    memset(oldctx, 0, sizeof(struct context) +
                        MAX_CONTEXT_STACK * sizeof(hvalue_t));
    memcpy(oldctx, context, ctxsize);

    // Recreate the steps
    twostep(
        global,
        file,
        parent,
        ctx,
        global->callstacks[pid],
        e->choice,
        e->interrupt,
        oldstate,
        oldctx,
        node->state->vars,
        e->nsteps,
        pid
    );
    fprintf(file, "\n      ],\n");
  
    // Print the resulting state
    print_state(global, file, node);
    fprintf(file, "    }");
}

#endif // PATH_DUMP

static void *copy(void *p, unsigned int size){
    char *c = malloc(size);
    memcpy(c, p, size);
    return c;
}

// Recursively reconstruct the steps to edge e using the twostep() function
void path_recompute(
    struct global *global,
    struct edge *e
) {
    struct node *node = e->dst;
    struct node *parent = e->src;

    // First recurse to the previous step
    if (parent->to_parent != NULL) {
        path_recompute(global, parent->to_parent);
    }

    /* Find the starting context in the list of processes.  Prefer
     * sticking with the same pid if possible.
     */
    hvalue_t ctx = e->ctx;
    unsigned int pid;
    if (global->processes[oldpid] == ctx) {
        pid = oldpid;
    }
    else {
        for (pid = 0; pid < global->nprocesses; pid++) {
            if (global->processes[pid] == ctx) {
                break;
            }
        }
        oldpid = pid;
    }
    assert(pid < global->nprocesses);

    struct macrostep *macro = calloc(sizeof(*macro), 1);
    macro->node = node;
    macro->tid = pid;
    macro->choice = e->choice;
    macro->ctx = ctx;
    macro->cs = global->callstacks[pid];

    // Recreate the steps
    twostep2(
        global,
        parent,
        ctx,
        global->callstacks[pid],
        e->choice,
        e->interrupt,
        node->state->vars,
        e->nsteps,
        pid,
        macro
    );

    // Copy thread state
    macro->nprocesses = global->nprocesses;
    macro->processes = copy(global->processes, global->nprocesses * sizeof(hvalue_t));
    macro->callstacks = copy(global->callstacks, global->nprocesses * sizeof(struct callstack *));

    if (global->nmacrosteps == global->alloc_macrosteps) {
        global->alloc_macrosteps *= 2;
        if (global->alloc_macrosteps < 8) {
            global->alloc_macrosteps = 8;
        }
        global->macrosteps = realloc(global->macrosteps,
            global->alloc_macrosteps * sizeof(*global->macrosteps));
    }
    global->macrosteps[global->nmacrosteps++] = macro;
}

static void path_output_microstep(struct global *global, FILE *file,
    struct microstep *micro,
    struct state *oldstate,
    struct context *oldctx,
    struct callstack *oldcs
){
    fprintf(file, "\n        {\n");
    struct json_value *next = global->pretty->u.list.vals[oldctx->pc];
    assert(next->type == JV_LIST);
    assert(next->u.list.nvals == 2);
    struct json_value *opstr = next->u.list.vals[0];
    assert(opstr->type == JV_ATOM);
    char *op = json_escape(opstr->u.atom.base, opstr->u.atom.len);
    fprintf(file, "          \"code\": \"%s\",\n", op);
    free(op);

    if (strlen(micro->explain) == 0) {
        struct json_value *next = global->pretty->u.list.vals[oldctx->pc];
        assert(next->type == JV_LIST);
        assert(next->u.list.nvals == 2);
        struct json_value *codestr = next->u.list.vals[1];
        assert(codestr->type == JV_ATOM);
		char *v = json_escape(codestr->u.atom.base, codestr->u.atom.len);
        fprintf(file, "          \"explain\": \"%s\",\n", v);
        free(v);
    }
    else {
        fprintf(file, "          \"explain\": \"%s\",\n", micro->explain);
    }

    if (micro->state->vars != oldstate->vars) {
        fprintf(file, "          \"shared\": ");
        print_vars(global, file, micro->state->vars);
        fprintf(file, ",\n");
    }
    if (micro->interrupt) {
        fprintf(file, "          \"interrupt\": \"True\",\n");
    }
    if (micro->choose) {
        char *val = value_json(micro->choice, global);
        fprintf(file, "          \"choose\": %s,\n", val);
        free(val);
    }
    if (micro->print != 0) {
        char *val = value_json(micro->print, global);
        fprintf(file, "          \"print\": %s,\n", val);
        free(val);
    }

    struct context *newctx = micro->ctx;
    struct callstack *newcs = micro->cs;

    fprintf(file, "          \"npc\": \"%d\",\n", newctx->pc);
    if (newcs != NULL && newcs != oldcs) {
        fprintf(file, "          \"fp\": \"%d\",\n", newcs->sp + 1);
#ifdef notdef
        {
            fprintf(stderr, "STACK2 %d:\n", newctx->fp);
            for (int x = 0; x < newctx->sp; x++) {
                fprintf(stderr, "    %d: %s\n", x, value_string(ctx_stack(newctx)[x]));
            }
        }
#endif

        fprintf(file, "          \"trace\": [\n");
        value_trace(global, file, newcs, newctx->pc, newctx->vars, "          ");
        fprintf(file, "\n");
        fprintf(file, "          ],\n");
    }
    // TODO.  Shouldn't this check if the oldctx is also extended?
    if (newctx->extended && ctx_this(newctx) != ctx_this(oldctx)) {
        char *val = value_json(ctx_this(newctx), global);
        fprintf(file, "          \"this\": %s,\n", val);
        free(val);
    }
    if (newctx->vars != oldctx->vars) {
        fprintf(file, "          \"local\": ");
        print_vars(global, file, newctx->vars);
        fprintf(file, ",\n");
    }
    if (newctx->atomic != oldctx->atomic) {
        fprintf(file, "          \"atomic\": \"%d\",\n", newctx->atomic);
    }
    if (newctx->readonly != oldctx->readonly) {
        fprintf(file, "          \"readonly\": \"%d\",\n", newctx->readonly);
    }
    if (newctx->interruptlevel != oldctx->interruptlevel) {
        fprintf(file, "          \"interruptlevel\": \"%d\",\n", newctx->interruptlevel ? 1 : 0);
    }
    if (newctx->failed) {
        char *val = value_string(ctx_failure(newctx));
        fprintf(file, "          \"failure\": %s,\n", val);
        fprintf(file, "          \"mode\": \"failed\",\n");
        free(val);
    }
    else if (newctx->terminated) {
        fprintf(file, "          \"mode\": \"terminated\",\n");
    }

    unsigned int common;
    for (common = 0; common < newctx->sp && common < oldctx->sp; common++) {
        if (ctx_stack(newctx)[common] != ctx_stack(oldctx)[common]) {
            break;
        }
    }
    if (common < oldctx->sp) {
        fprintf(file, "          \"pop\": \"%d\",\n", oldctx->sp - common);
    }
    fprintf(file, "          \"push\": [");
    for (unsigned int i = common; i < newctx->sp; i++) {
        if (i > common) {
            fprintf(file, ",");
        }
        char *val = value_json(ctx_stack(newctx)[i], global);
        fprintf(file, " %s", val);
        free(val);
    }
    fprintf(file, " ],\n");

#ifdef notdef
    unsigned int bs = oldstate->bagsize * (sizeof(hvalue_t) + 1);
    if (oldstate->bagsize != micro->state->bagsize ||
            memcmp(state_contexts(oldstate), state_contexts(micro->state), bs) != 0) {
        fprintf(file, "          \"contexts\": \"%d\",\n", micro->state->bagsize);
    }
#endif

    fprintf(file, "          \"pc\": \"%d\"\n", oldctx->pc);

    fprintf(file, "        }");
}

static void path_output_macrostep(struct global *global, FILE *file, struct macrostep *macro, struct state *oldstate){
    fprintf(file, "    {\n");
    fprintf(file, "      \"id\": \"%d\",\n", macro->node->id);
    fprintf(file, "      \"len\": \"%d\",\n", macro->node->len);
    fprintf(file, "      \"tid\": \"%d\",\n", macro->tid);

    fprintf(file, "      \"shared\": ");
    print_vars(global, file, oldstate->vars);
    fprintf(file, ",\n");

    struct callstack *cs = macro->cs;
    while (cs->parent != NULL) {
        cs = cs->parent;
    }
    assert(strcmp(global->code.instrs[cs->pc].oi->name, "Frame") == 0);
    const struct env_Frame *ef = global->code.instrs[cs->pc].env;
    char *name = value_string(ef->name);
	int len = strlen(name);
    char *arg = json_escape_value(cs->arg);
    if (*arg == '(') {
        fprintf(file, "      \"name\": \"%.*s%s\",\n", len - 2, name + 1, arg);
    }
    else {
        fprintf(file, "      \"name\": \"%.*s(%s)\",\n", len - 2, name + 1, arg);
    }
    free(name);
    free(arg);

    char *c = macro->choice == 0 ? NULL : value_json(macro->choice, global);
    if (c != NULL) {
        fprintf(file, "      \"choice\": %s,\n", c);
    }
    free(c);

    fprintf(file, "      \"context\": {\n");
    print_context(global, file, macro->ctx, macro->cs, macro->tid, macro->node, "        ");
    fprintf(file, "      },\n");

    fprintf(file, "      \"microsteps\": [");
    struct context *oldctx = value_get(macro->ctx, NULL);
    struct callstack *oldcs = NULL;
    for (unsigned int i = 0; i < macro->nmicrosteps; i++) {
        struct microstep *micro = macro->microsteps[i];
        path_output_microstep(global, file, micro, oldstate, oldctx, oldcs);
        if (i == macro->nmicrosteps - 1) {
            fprintf(file, "\n");
        }
        else {
            fprintf(file, ",\n");
        }
        memcpy(oldstate, micro->state, state_size(micro->state));
        oldctx = micro->ctx;
        oldcs = micro->cs;
    }
    fprintf(file, "\n      ],\n");
  
    fprintf(file, "      \"ctxbag\": {\n");
    struct state *state = macro->node->state;
    for (unsigned int i = 0; i < state->bagsize; i++) {
        if (i > 0) {
            fprintf(file, ",\n");
        }
        assert(VALUE_TYPE(state_contexts(state)[i]) == VALUE_CONTEXT);
        fprintf(file, "          \"%"PRIx64"\": \"%u\"", state_contexts(state)[i],
                multiplicities(state)[i]);
    }
    fprintf(file, "\n      },\n");

    fprintf(file, "      \"contexts\": [\n");
    for (unsigned int i = 0; i < macro->nprocesses; i++) {
        fprintf(file, "        {\n");
        print_context(global, file, macro->processes[i], macro->callstacks[i], i, macro->node, "          ");
        fprintf(file, "        }");
        if (i < macro->nprocesses - 1) {
            fprintf(file, ",");
        }
        fprintf(file, "\n");
    }
    fprintf(file, "      ]\n");

    fprintf(file, "    }");
}

// Output the macrosteps
static void path_output(struct global *global, FILE *file){
    fprintf(file, "\n");
    struct state *oldstate = calloc(1, sizeof(struct state) + MAX_CONTEXT_BAG * (sizeof(hvalue_t) + 1));
    oldstate->vars = VALUE_DICT;
    for (unsigned int i = 0; i < global->nmacrosteps; i++) {
        path_output_macrostep(global, file, global->macrosteps[i], oldstate);
        if (i == global->nmacrosteps - 1) {
            fprintf(file, "\n");
        }
        else {
            fprintf(file, ",\n");
        }
    }
}

// Remove unneeded microsteps from error trace
static void path_trim(struct global *global, struct engine *engine){
    // Find the last macrostep for each thread
    unsigned int *last = calloc(global->nprocesses, sizeof(*last));
    for (unsigned int i = 0; i < global->nmacrosteps; i++) {
        last[global->macrosteps[i]->tid] = i;
    }

    struct instr *instrs = global->code.instrs;
    for (unsigned int i = 1; i < global->nprocesses; i++) {
        // Don't trim the very last step
        if (last[i] == global->nmacrosteps - 1) {
            continue;
        }
        struct macrostep *macro = global->macrosteps[last[i]];

        // Look up the last microstep of this thread, which wasn't the
        // last one to take a step overall
        struct context *cc = value_get(macro->ctx, NULL);
        struct microstep *ls = macro->microsteps[macro->nmicrosteps - 1];
        struct instr *fi = &instrs[cc->pc];
        struct instr *li = &instrs[ls->ctx->pc];
        if ((fi->store || fi->load || fi->print) && (li->store || li->load || li->print)) {

            macro->nmicrosteps = 1;
            hvalue_t ictx = value_put_context(engine, macro->microsteps[0]->ctx);
            for (unsigned int j = last[i]; j < global->nmacrosteps; j++) {
                struct macrostep *m = global->macrosteps[j];
                m->processes[macro->tid] = ictx;
                m->callstacks[macro->tid] = macro->microsteps[0]->cs;
            }
        }
    }
}

static char *json_string_encode(char *s, int len){
    char *result = malloc(4 * len + 1), *p = result;

    while (len > 0) {
        switch (*s) {
        case '\r':
            *p++ = '\\'; *p++ = 'r';
            break;
        case '\n':
            *p++ = '\\'; *p++ = 'n';
            break;
        case '\f':
            *p++ = '\\'; *p++ = 'f';
            break;
        case '\t':
            *p++ = '\\'; *p++ = 't';
            break;
        case '"':
            *p++ = '\\'; *p++ = '"';
            break;
        case '\\':
            *p++ = '\\'; *p++ = '\\';
            break;
        default:
            *p++ = *s;
        }
        s++;
        len--;
    }
    *p++ = 0;
    return result;
}

#ifdef notdef

struct enum_loc_env_t {
    FILE *out;
    struct dict *code_map;
};

static void enum_loc(
    void *env,
    const void *key,
    unsigned int key_size,
    void *value
) {
    static bool notfirst = false;
    struct enum_loc_env_t *enum_loc_env = env;
    FILE *out = enum_loc_env->out;
    struct dict *code_map = enum_loc_env->code_map;

    if (notfirst) {
        fprintf(out, ",\n");
    }
    else {
        notfirst = true;
        fprintf(out, "\n");
    }

    // Get program counter
    char *pcc = malloc(key_size + 1);
    memcpy(pcc, key, key_size);
    pcc[key_size] = 0;
    int pc = atoi(pcc);
    free(pcc);

    fprintf(out, "    \"%.*s\": { ", key_size, (char *) key);

    struct json_value **pjv = value;
    struct json_value *jv = *pjv;
    assert(jv->type == JV_MAP);

    struct json_value *file = dict_lookup(jv->u.map, "file", 4);
    assert(file->type == JV_ATOM);
    fprintf(out, "\"file\": \"%s\", ", json_string_encode(file->u.atom.base, file->u.atom.len));

    struct json_value *line = dict_lookup(jv->u.map, "line", 4);
    assert(line->type == JV_ATOM);
    fprintf(out, "\"line\": \"%.*s\", ", line->u.atom.len, line->u.atom.base);

    static char *tocopy[] = { "column", "endline", "endcolumn", NULL };
    for (unsigned int i = 0; tocopy[i] != NULL; i++) {
        char *key2 = tocopy[i];
        struct json_value *jv2 = dict_lookup(jv->u.map, key2, strlen(key2));
        assert(jv2->type == JV_ATOM);
        fprintf(out, "\"%s\": \"%.*s\", ", key2, jv2->u.atom.len, jv2->u.atom.base);
    }

    struct json_value *stmt = dict_lookup(jv->u.map, "stmt", 4);
    assert(stmt->type == JV_LIST);
    assert(stmt->u.list.nvals == 4);
    fprintf(out, "\"stmt\": [");
    for (unsigned int i = 0; i < 4; i++) {
        if (i != 0) {
            fprintf(out, ",");
        }
        struct json_value *jv2 = stmt->u.list.vals[i];
        assert(jv2->type == JV_ATOM);
        fprintf(out, "%.*s", jv2->u.atom.len, jv2->u.atom.base);
    }
    fprintf(out, "], ");

    char **p = dict_insert(code_map, NULL, &pc, sizeof(pc), NULL);
    struct strbuf sb;
    strbuf_init(&sb);
    strbuf_printf(&sb, "%.*s:%.*s", file->u.atom.len, file->u.atom.base, line->u.atom.len, line->u.atom.base);
    *p = strbuf_convert(&sb);

    struct json_value *code = dict_lookup(jv->u.map, "code", 4);
    assert(code->type == JV_ATOM);
    fprintf(out, "\"code\": \"%s\"", json_string_encode(code->u.atom.base, code->u.atom.len));
    fprintf(out, " }");
}

#endif // notdef

enum busywait { BW_ESCAPE, BW_RETURN, BW_VISITED };
static enum busywait is_stuck(
    struct node *start,
    struct node *node,
    hvalue_t ctx,
    bool change
) {
	if (node->component != start->component) {
		return BW_ESCAPE;
	}
	if (node->visited) {
		return BW_VISITED;
	}
    change = change || (node->state->vars != start->state->vars);
	node->visited = true;
	enum busywait result = BW_ESCAPE;
    for (struct edge *edge = node->fwd; edge != NULL; edge = edge->fwdnext) {
        if (edge->ctx == ctx) {
			if (edge->dst == node) {
				node->visited = false;
				return BW_ESCAPE;
			}
			if (edge->dst == start) {
				if (!change) {
					node->visited = false;
					return BW_ESCAPE;
				}
				result = BW_RETURN;
			}
			else {
				enum busywait bw = is_stuck(start, edge->dst, edge->after, change);
				switch (bw) {
				case BW_ESCAPE:
					node->visited = false;
					return BW_ESCAPE;
				case BW_RETURN:
					result = BW_RETURN;
					break;
				case BW_VISITED:
					break;
				default:
					assert(false);
				}
			}
        }
    }
	node->visited = false;
    return result;
}

static void detect_busywait(struct minheap *failures, struct node *node){
	for (unsigned int i = 0; i < node->state->bagsize; i++) {
		if (is_stuck(node, node, state_contexts(node->state)[i], false) == BW_RETURN) {
			struct failure *f = new_alloc(struct failure);
			f->type = FAIL_BUSYWAIT;
			f->edge = node->to_parent;
			minheap_insert(failures, f);
			// break;
		}
	}
}

static int node_cmp(void *n1, void *n2){
    struct node *node1 = n1, *node2 = n2;

    if (node1->len != node2->len) {
        return node1->len - node2->len;
    }
    if (node1->steps != node2->steps) {
        return node1->steps - node2->steps;
    }
    return node1->id - node2->id;
}

static int fail_cmp(void *f1, void *f2){
    struct failure *fail1 = f1, *fail2 = f2;

    return node_cmp(fail1->edge->dst, fail2->edge->dst);
}

static void do_work(struct worker *w){
    struct global *global = w->global;

    for (;;) {
        mutex_acquire(&global->todo_lock);
        assert(global->goal >= global->todo);
        unsigned int start = global->todo;
        unsigned int nleft = global->goal - start;
        if (nleft == 0) {
            mutex_release(&global->todo_lock);
            break;
        }

        unsigned int take = nleft / w->nworkers / 2;
        if (take < 100) {
            take = 100;
        }
        if (take > nleft) {
            take = nleft;
        }
        global->todo = start + take;
        assert(global->todo <= global->graph.size);
        assert(global->goal >= global->todo);
        mutex_release(&global->todo_lock);

        while (take > 0) {
            struct node *node = global->graph.nodes[start++];
            struct state *state = node->state;
            w->dequeued++;

            if (state->choosing != 0) {
                assert(VALUE_TYPE(state->choosing) == VALUE_CONTEXT);

                struct context *cc = value_get(state->choosing, NULL);
                assert(cc != NULL);
                assert(cc->sp > 0);
                hvalue_t s = ctx_stack(cc)[cc->sp - 1];
                assert(VALUE_TYPE(s) == VALUE_SET);
                unsigned int size;
                hvalue_t *vals = value_get(s, &size);
                size /= sizeof(hvalue_t);
                assert(size > 0);
                for (unsigned int i = 0; i < size; i++) {
                    make_step(
                        w,
                        node,
                        state->choosing,
                        vals[i],
                        1
                    );
                }
            }
            else {
                for (unsigned int i = 0; i < state->bagsize; i++) {
                    assert(VALUE_TYPE(state_contexts(state)[i]) == VALUE_CONTEXT);
                    make_step(
                        w,
                        node,
                        state_contexts(state)[i],
                        0,
                        multiplicities(state)[i]
                    );
                }
            }
            take--;
        }
    }
}

static void work_phase2(struct worker *w, struct global *global){
    mutex_acquire(&global->todo_lock);
    for (;;) {
        if (global->scc_todo == NULL) {
            global->scc_nwaiting++;
            if (global->scc_nwaiting == global->nworkers) {
                mutex_release(&global->todo_wait);
                break;
            }
            mutex_release(&global->todo_lock);
            mutex_acquire(&global->todo_wait);
            if (global->scc_nwaiting == global->nworkers) {
                mutex_release(&global->todo_wait);
                break;
            }
            global->scc_nwaiting--;
        }

        // Grab work
        unsigned int component = global->ncomponents++;
        struct scc *scc = global->scc_todo;
        assert(scc != NULL);
        global->scc_todo = scc->next;
        scc->next = NULL;

        // Split binary semaphore release
        if (global->scc_todo != NULL && global->scc_nwaiting > 0) {
            mutex_release(&global->todo_wait);
        }
        else {
            mutex_release(&global->todo_lock);
        }

        for (;;) {
            // Do the work
            assert(scc->next == NULL);
            scc = graph_find_scc_one(&global->graph, scc, component, &w->scc_cache);

            // Put new work on the list except the last (which we'll do ourselves)
            mutex_acquire(&global->todo_lock);
            while (scc != NULL && scc->next != NULL) {
                struct scc *next = scc->next;
                scc->next = global->scc_todo;
                global->scc_todo = scc;
                scc = next;
            }
            if (scc == NULL) {      // get more work
                break;
            }
            component = global->ncomponents++;

            // Split binary semaphore release
            if (global->scc_todo != NULL && global->scc_nwaiting > 0) {
                mutex_release(&global->todo_wait);
            }
            else {
                mutex_release(&global->todo_lock);
            }
        }
    }
}

static void worker(void *arg){
    struct worker *w = arg;
    struct global *global = w->global;

    for (int epoch = 0;; epoch++) {
        barrier_wait(w->start_barrier);

        // (first) parallel phase starts now
		// printf("WORKER %d starting epoch %d\n", w->index, epoch);
		do_work(w);

        // wait for others to finish
		// printf("WORKER %d finished epoch %d %u %u\n", w->index, epoch, w->count, w->node_id);
        barrier_wait(w->middle_barrier);

        if (global->phase2) {
            work_phase2(w, global);
            barrier_wait(w->end_barrier);
            break;
        }

        // Wait for coordinator to have grown the graph table and hash tables
        // In the mean time fix the forward edges
        for (unsigned i = 0; i < w->nworkers; i++) {
            struct edge **pe = &w->workers[i].edges[w->index], *e;
            while ((e = *pe) != NULL) {
                *pe = e->fwdnext;
                struct node *src = e->src;
                e->fwdnext = src->fwd;
                src->fwd = e;
            }
        }

        barrier_wait(w->end_barrier);

		// printf("WORKER %d make stable %d %u %u\n", w->index, epoch, w->count, w->node_id);
        value_make_stable(&global->values, w->index);
        dict_make_stable(w->visited, w->index);

        if (global->layer_done) {
            // Fill the graph table
            for (unsigned int i = 0; w->count != 0; i++) {
                struct node *node = w->results;
                node->id = w->node_id;
                global->graph.nodes[w->node_id++] = node;
                w->results = node->next;
                w->count--;
            }
            assert(w->results == NULL);
        }
    }
}

void process_results(struct global *global, struct worker *w){
    struct failure *f;
    while ((f = w->failures) != NULL) {
        w->failures = f->next;
        minheap_insert(global->failures, f);
    }
}

char *state_string(struct state *state){
    struct strbuf sb;
    strbuf_init(&sb);

    char *v;
    strbuf_printf(&sb, "{");
    v = value_string(state->vars);
    strbuf_printf(&sb, "%s", v); free(v);
    v = value_string(state->choosing);
    strbuf_printf(&sb, ",%s", v); free(v);
    v = value_string(state->stopbag);
    strbuf_printf(&sb, ",%s}", v); free(v);
    return strbuf_convert(&sb);
}

// This routine removes all nodes that have a single incoming edge and it's
// an "epsilon" edge (empty print log).  These are essentially useless nodes.
// Typically about half of the nodes can be removed this way.
static void destutter1(struct graph *graph){
    for (unsigned int i = 0; i < graph->size; i++) {
        struct node *n = graph->nodes[i];

        if (n->bwd != NULL && n->bwd->bwdnext == NULL && n->bwd->nlog == 0) {
            struct node *parent = n->bwd->src;

            if (n->final) {
                parent->final = true;
            }

            // Remove the edge from the parent
            struct edge **pe, *e;
            for (pe = &parent->fwd; (e = *pe) != NULL; pe = &e->fwdnext) {
                if (e->dst == n && e->nlog == 0) {
                    *pe = e->fwdnext;
                    // free(e);
                    break;
                }
            }

            struct edge *next;
            for (struct edge *e = n->fwd; e != NULL; e = next) {
                // Move the outgoing edge to the parent.
                next = e->fwdnext;
                e->fwdnext = parent->fwd;
                parent->fwd = e;

                // Fix the corresponding backwards edge
                for (struct edge *f = e->dst->bwd; f != NULL; f = f->bwdnext) {
                    if (f->src == n && f->nlog == e->nlog &&
                            memcmp(edge_log(f), edge_log(e), f->nlog * sizeof(hvalue_t)) == 0) {
                        f->src = parent;
                        break;
                    }
                }
            }
            n->reachable = false;
        }
        else {
            n->reachable = true;
        }
    }
}

static struct dict *collect_symbols(struct graph *graph){
    struct dict *symbols = dict_new("symbols", sizeof(unsigned int), 0, 0, NULL, NULL);
    unsigned int symbol_id = 0;

    for (unsigned int i = 0; i < graph->size; i++) {
        struct node *n = graph->nodes[i];
        if (!n->reachable) {
            continue;
        }
        for (struct edge *e = n->fwd; e != NULL; e = e->fwdnext) {
            for (unsigned int j = 0; j < e->nlog; j++) {
                bool new;
                unsigned int *p = dict_insert(symbols, NULL, &edge_log(e)[j], sizeof(hvalue_t), &new);
                if (new) {
                    *p = ++symbol_id;
                }
            }
        }
    }
    return symbols;
}

struct symbol_env {
    struct global *global;
    FILE *out;
    bool first;
};

static void print_symbol(void *env, const void *key, unsigned int key_size, void *value){
    struct symbol_env *se = env;
    const hvalue_t *symbol = key;

    assert(key_size == sizeof(*symbol));
    char *p = value_json(*symbol, se->global);
    if (se->first) {
        se->first = false;
    }
    else {
        fprintf(se->out, ",\n");
    }
    fprintf(se->out, "     \"%u\": %s", * (unsigned int *) value, p);
    free(p);
}

struct print_trans_env {
    FILE *out;
    bool first;
    struct dict *symbols;
};

static void print_trans_upcall(void *env, const void *key, unsigned int key_size, void *value){
    struct print_trans_env *pte = env;
    const hvalue_t *log = key;
    unsigned int nkeys = key_size / sizeof(hvalue_t);
    struct strbuf *sb = value;

    if (pte->first) {
        pte->first = false;
    }
    else {
        fprintf(pte->out, ",\n");
    }
    fprintf(pte->out, "        [[");
    for (unsigned int i = 0; i < nkeys; i++) {
        bool new;
        unsigned int *p = dict_insert(pte->symbols, NULL, &log[i], sizeof(log[i]), &new);
        assert(!new);
        if (i != 0) {
            fprintf(pte->out, ",");
        }
        fprintf(pte->out, "%u", *p);
    }
    fprintf(pte->out, "],[%s]]", strbuf_getstr(sb));
    strbuf_deinit(sb);
}

static void print_transitions(FILE *out, struct dict *symbols, struct edge *edges){
    struct dict *d = dict_new("transitions", sizeof(struct strbuf), 0, 0, NULL, NULL);

    fprintf(out, "      \"transitions\": [\n");
    for (struct edge *e = edges; e != NULL; e = e->fwdnext) {
        bool new;
        struct strbuf *sb = dict_insert(d, NULL, edge_log(e), e->nlog * sizeof(hvalue_t), &new);
        if (new) {
            strbuf_init(sb);
            strbuf_printf(sb, "%d", e->dst->id);
        }
        else {
            strbuf_printf(sb, ",%d", e->dst->id);
        }
    }
    struct print_trans_env pte = {
        .out = out, .first = true, .symbols = symbols
    };
    dict_iter(d, print_trans_upcall, &pte);
    fprintf(out, "\n");
    fprintf(out, "      ],\n");
    dict_delete(d);
}

#ifndef _WIN32
static void inthandler(int sig){
    printf("Caught interrupt\n");
    _exit(1);
}
#endif

static void usage(char *prog){
    fprintf(stderr, "Usage: %s [-c] [-t<maxtime>] [-B<dfafile>] -o<outfile> file.json\n", prog);
    exit(1);
}

int main(int argc, char **argv){
    bool cflag = false;
    int i, maxtime = 300000000 /* about 10 years */;
    char *outfile = NULL, *dfafile = NULL;
    for (i = 1; i < argc; i++) {
        if (*argv[i] != '-') {
            break;
        }
        switch (argv[i][1]) {
        case 'c':
            cflag = true;
            break;
        case 't':
            maxtime = atoi(&argv[i][2]);
            if (maxtime <= 0) {
                fprintf(stderr, "%s: negative timeout\n", argv[0]);
                exit(1);
            }
            break;
        case 'B':
            dfafile = &argv[i][2];
            break;
        case 'o':
            outfile = &argv[i][2];
            break;
        case 'x':
            printf("Charm model checker working\n");
            return 0;
        default:
            usage(argv[0]);
        }
    }
    if (argc - i != 1) {
        usage(argv[0]);
    }
    char *fname = argv[i];
    double timeout = gettime() + maxtime;

#ifndef _WIN32
    signal(SIGINT, inthandler);
#endif

    // Determine how many worker threads to use
    struct global *global = new_alloc(struct global);
    global->nworkers = getNumCores();
	printf("nworkers = %d\n", global->nworkers);

    barrier_t start_barrier, middle_barrier, end_barrier;
    barrier_init(&start_barrier, global->nworkers + 1);
    barrier_init(&middle_barrier, global->nworkers + 1);
    barrier_init(&end_barrier, global->nworkers + 1);

    // initialize modules
    mutex_init(&global->inv_lock);
    mutex_init(&global->todo_lock);
    mutex_init(&global->todo_wait);
    mutex_acquire(&global->todo_wait);          // Split Binary Semaphore
    value_init(&global->values, global->nworkers);

    struct engine engine;
    engine.allocator = NULL;
    engine.values = &global->values;
    ops_init(global, &engine);

    graph_init(&global->graph, 1024*1024);
    global->failures = minheap_create(fail_cmp);
    global->seqs = VALUE_SET;

    // First read and parse the DFA if any
    if (dfafile != NULL) {
        global->dfa = dfa_read(&engine, dfafile);
        if (global->dfa == NULL) {
            exit(1);
        }
    }

    // open the HVM file
    FILE *fp = fopen(fname, "r");
    if (fp == NULL) {
        fprintf(stderr, "%s: can't open %s\n", argv[0], fname);
        exit(1);
    }

    // read the file
    json_buf_t buf;
    buf.base = malloc(CHUNKSIZE);
    buf.len = 0;
    int n;
    while ((n = fread(&buf.base[buf.len], 1, CHUNKSIZE, fp)) > 0) {
        buf.len += n;
        buf.base = realloc(buf.base, buf.len + CHUNKSIZE);
    }
    fclose(fp);

    // parse the contents
	char *buf_orig = buf.base;
    struct json_value *jv = json_parse_value(&buf);
    assert(jv->type == JV_MAP);
	free(buf_orig);

    // travel through the json code contents to create the code array
    struct json_value *jc = dict_lookup(jv->u.map, "code", 4);
    assert(jc->type == JV_LIST);
    global->code = code_init_parse(&engine, jc);

    // Create an initial state
    struct context *init_ctx = calloc(1, sizeof(struct context) + MAX_CONTEXT_STACK * sizeof(hvalue_t));
    init_ctx->vars = VALUE_DICT;
    init_ctx->atomic = 1;
    init_ctx->initial = true;
    init_ctx->atomicFlag = true;
    value_ctx_push(init_ctx, VALUE_LIST);

    struct state *state = calloc(1, sizeof(struct state) + sizeof(hvalue_t) + 1);
    state->vars = VALUE_DICT;
    hvalue_t ictx = value_put_context(&engine, init_ctx);
    state->bagsize = 1;
    state_contexts(state)[0] = ictx;
    multiplicities(state)[0] = 1;
    state->stopbag = VALUE_DICT;
    state->dfa_state = global->dfa == NULL ? 0 : dfa_initial(global->dfa);

    // Needed for second phase
    global->processes = new_alloc(hvalue_t);
    global->callstacks = new_alloc(struct callstack *);
    *global->processes = ictx;
    struct callstack *cs = new_alloc(struct callstack);
    cs->arg = VALUE_LIST;
    cs->vars = VALUE_DICT;
    cs->return_address = CALLTYPE_PROCESS;
    *global->callstacks = cs;
    global->nprocesses = 1;

    // Run direct
    if (outfile == NULL) {
        global->run_direct = true;
        mutex_init(&run_mutex);
        mutex_init(&run_waiting);
        mutex_acquire(&run_waiting);
        run_count = 1;

        // Run the initializing thread to completion
        // TODO.  spawned threads should wait...
        run_thread(global, state, init_ctx);

        // Wait for other threads
        mutex_acquire(&run_mutex);
        if (run_count > 0) {
            mutex_release(&run_mutex);
            mutex_acquire(&run_waiting);
        }
        mutex_release(&run_mutex);
        exit(0);
    }

    // Put the initial state in the visited map
    struct dict *visited = dict_new("visited", sizeof(struct node), 0, global->nworkers, NULL, NULL);
    struct node *node = dict_insert(visited, NULL, state, state_size(state), NULL);
    memset(node, 0, sizeof(*node));
    node->state = state;
    graph_add(&global->graph, node);
    global->goal = 1;

    // Allocate space for worker info
    struct worker *workers = calloc(global->nworkers, sizeof(*workers));
    for (unsigned int i = 0; i < global->nworkers; i++) {
        struct worker *w = &workers[i];
        w->visited = visited;
        w->global = global;
        w->timeout = timeout;
        w->start_barrier = &start_barrier;
        w->middle_barrier = &middle_barrier;
        w->end_barrier = &end_barrier;
        w->index = i;
        w->workers = workers;
        w->nworkers = global->nworkers;
        w->edges = calloc(global->nworkers, sizeof(struct edge *));
        w->profile = calloc(global->code.len, sizeof(*w->profile));

        // Create a context for evaluating invariants
        w->inv_step.ctx = calloc(1, sizeof(struct context) +
                                MAX_CONTEXT_STACK * sizeof(hvalue_t));
        // w->inv_step.ctx->name = value_put_atom(&engine, "__invariant__", 13);
        w->inv_step.ctx->vars = VALUE_DICT;
        w->inv_step.ctx->atomic = w->inv_step.ctx->readonly = 1;
        w->inv_step.ctx->atomicFlag = true;
        w->inv_step.ctx->interruptlevel = false;
        w->inv_step.engine.allocator = &w->allocator;
        w->inv_step.engine.values = &global->values;

        w->alloc_buf = malloc(WALLOC_CHUNK);
        w->alloc_ptr = w->alloc_buf;

        w->allocator.alloc = walloc;
        w->allocator.ctx = w;
        w->allocator.worker = i;
    }

    // Start the workers, who'll wait on the start barrier
    for (unsigned int i = 0; i < global->nworkers; i++) {
        thread_create(worker, &workers[i]);
    }

    // Put the state and value dictionaries in concurrent mode
    value_set_concurrent(&global->values);
    dict_set_concurrent(visited);

    double before = gettime(), postproc = 0;
    for (;;) {
        barrier_wait(&start_barrier);

        // Threads are working to create the next layer of nodes.
        // Stay out of their way!

        barrier_wait(&middle_barrier);

        // Back to sequential mode

        // Prepare the grow the hash tables (but the actual work of
        // rehashing is distributed among the threads in the next phase
        double before_postproc = gettime();
        dict_grow_prepare(visited);
        value_grow_prepare(&global->values);
        postproc += gettime() - before_postproc;

        // End of a layer in the Kripke structure?
        global->layer_done = global->todo == global->graph.size;
        if (global->layer_done) {
            global->diameter++;
            // printf("Diameter %d\n", global->diameter);

            // The threads completed producing the next layer of nodes in the graph.
            // Grow the graph table.
            unsigned int total = 0;
            for (unsigned int i = 0; i < global->nworkers; i++) {
                struct worker *w = &workers[i];
                w->node_id = global->todo + total;
                total += w->count;
            }
            graph_add_multiple(&global->graph, total);

            // Collect the failures of all the workers
            for (unsigned int i = 0; i < global->nworkers; i++) {
                process_results(global, &workers[i]);
            }

            if (!minheap_empty(global->failures)) {
                // Pretend we're done
                global->todo = global->goal = global->graph.size;
            }
            if (global->todo == global->graph.size) { // no new nodes added
                break;
            }
        }

        // Determine the new goal
        unsigned int nleft = global->graph.size - global->todo;
        if (nleft > 1024 * global->nworkers) {
            global->goal = global->todo + 1024 * global->nworkers;
        }
        else {
            global->goal = global->graph.size;
        }
        assert(global->goal >= global->todo);

        // printf("Coordinator back to workers (%d)\n", global->diameter);

        barrier_wait(&end_barrier);

        // The threads now update the hash tables and the graph table
    }

    // Get threads going on fixing hash tables
    barrier_wait(&end_barrier);
    // Wait for threads to fix up hash tables
    barrier_wait(&start_barrier);

    printf("#states %d (time %.3lf+%.3lf=%.3lf)\n", global->graph.size, gettime() - before - postproc, postproc, gettime() - before);

    value_set_sequential(&global->values);
    dict_set_sequential(visited);

    // dict_dump(visited);
    // dict_dump(global->values.dicts);
    // dict_dump(global->values.addresses);
    // dict_dump(global->values.atoms);
    // dict_dump(global->values.lists);
    // dict_dump(global->values.sets);
    // dict_dump(global->values.contexts);
 
    printf("Phase 3: analysis\n");
    if (minheap_empty(global->failures)) {
        double now = gettime();
        global->phase2 = true;
        global->scc_todo = scc_alloc(0, global->graph.size, NULL, NULL);
        barrier_wait(&middle_barrier);
        // Workers working on finding SCCs
        barrier_wait(&end_barrier);

        printf("%u components (%.3lf seconds)\n", global->ncomponents, gettime() - now);

#ifdef DUMP_GRAPH
        printf("digraph Harmony {\n");
        for (unsigned int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            printf(" s%u [label=\"%u/%u\"]\n", i, i, node->component);
        }
        for (unsigned int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            for (struct edge *edge = node->fwd; edge != NULL; edge = edge->fwdnext) {
                printf(" s%u -> s%u\n", node->id, edge->dst->id);
            }
        }
        printf("}\n");
#endif

        // mark the components that are "good" because they have a way out
        struct component *components = calloc(global->ncomponents, sizeof(*components));
        for (unsigned int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
			assert(node->component < global->ncomponents);
            struct component *comp = &components[node->component];
            if (comp->size == 0) {
                comp->rep = node;
                comp->all_same = value_state_all_eternal(node->state)
                    && value_ctx_all_eternal(node->state->stopbag);
            }
            else if (node->state->vars != comp->rep->state->vars ||
                        !value_state_all_eternal(node->state) ||
                        !value_ctx_all_eternal(node->state->stopbag)) {
                comp->all_same = false;
            }
            comp->size++;
            if (comp->good) {
                continue;
            }
            // if this component has a way out, it is good
            for (struct edge *edge = node->fwd;
                            edge != NULL && !comp->good; edge = edge->fwdnext) {
                if (edge->dst->component != node->component) {
                    comp->good = true;
                    break;
                }
            }
        }

        // components that have only one shared state and only eternal
        // threads are good because it means all its threads are blocked
        for (unsigned int i = 0; i < global->ncomponents; i++) {
            struct component *comp = &components[i];
            assert(comp->size > 0);
            if (!comp->good && comp->all_same) {
                comp->good = true;
                comp->final = true;
            }
        }

        // Look for states in final components
        for (unsigned int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
			assert(node->component < global->ncomponents);
            struct component *comp = &components[node->component];
            if (comp->final) {
                node->final = true;
                if (global->dfa != NULL &&
						!dfa_is_final(global->dfa, node->state->dfa_state)) {
                    struct failure *f = new_alloc(struct failure);
                    f->type = FAIL_BEHAVIOR;
                    f->edge = node->to_parent;
                    minheap_insert(global->failures, f);
                    // break;
                }
            }
        }

        if (minheap_empty(global->failures)) {
            // now count the nodes that are in bad components
            int nbad = 0;
            for (unsigned int i = 0; i < global->graph.size; i++) {
                struct node *node = global->graph.nodes[i];
                if (!components[node->component].good) {
                    nbad++;
                    struct failure *f = new_alloc(struct failure);
                    f->type = FAIL_TERMINATION;
                    f->edge = node->to_parent;
                    minheap_insert(global->failures, f);
                    // break;
                }
            }

            if (nbad == 0 && !cflag) {
                for (unsigned int i = 0; i < global->graph.size; i++) {
                    global->graph.nodes[i]->visited = false;
                }
                for (unsigned int i = 0; i < global->graph.size; i++) {
                    struct node *node = global->graph.nodes[i];
                    if (components[node->component].size > 1) {
                        detect_busywait(global->failures, node);
                    }
                }
            }
        }
    }

#ifdef DUMP_GRAPH
    if (true) {
        FILE *df = fopen("charm.gv", "w");
        fprintf(df, "digraph Harmony {\n");
        for (unsigned int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            fprintf(df, " s%u [label=\"%u/%u\"]\n", i, i, node->len);
        }
        for (unsigned int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            for (struct edge *edge = node->fwd; edge != NULL; edge = edge->fwdnext) {
                struct state *state = node->state;
                unsigned int j;
                for (j = 0; j < state->bagsize; j++) {
                    if (state_contexts(state)[j] == edge->ctx) {
                        break;
                    }
                }
                assert(j < state->bagsize);
                fprintf(df, " s%u -> s%u [style=%s label=\"%u/%u\"]\n",
                        node->id, edge->dst->id,
                        edge->dst->to_parent == edge ? "solid" : "dashed",
                        multiplicities(state)[j],
                        edge->weight);
            }
        }
        fprintf(df, "}\n");
        fclose(df);
    }
#endif

#ifdef OBSOLETE
    if (true) {
        FILE *df = fopen("charm.dump", "w");
        assert(df != NULL);
        for (unsigned int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            assert(node->id == i);
            fprintf(df, "\nNode %d:\n", node->id);
            fprintf(df, "    component: %d\n", node->component);
            if (node->to_parent != NULL) {
                fprintf(df, "    ancestors:");
                for (struct node *n = node->to_parent->src;; n = n->to_parent->src) {
                    fprintf(df, " %u", n->id);
                    if (n->to_parent == NULL) {
                        break;
                    }
                }
                fprintf(df, "\n");
            }
            fprintf(df, "    vars: %s\n", value_string(node->state->vars));
            fprintf(df, "    len: %u %u\n", node->len, node->steps);
            fprintf(df, "    fwd:\n");
            int eno = 0;
            for (struct edge *edge = node->fwd; edge != NULL; edge = edge->fwdnext, eno++) {
                fprintf(df, "        %d:\n", eno);
                struct context *ctx = value_get(edge->ctx, NULL);
                fprintf(df, "            node: %d (%d)\n", edge->dst->id, edge->dst->component);
                fprintf(df, "            context before: %"PRIx64" %d\n", edge->ctx, ctx->pc);
                ctx = value_get(edge->after, NULL);
                fprintf(df, "            context after:  %"PRIx64" %d\n", edge->after, ctx->pc);
                if (edge->choice != 0) {
                    fprintf(df, "            choice: %s\n", value_string(edge->choice));
                }
                if (edge->nlog > 0) {
                    fprintf(df, "            log:");
                    for (unsigned int j = 0; j < edge->nlog; j++) {
                        char *p = value_string(edge_log(edge)[j]);
                        fprintf(df, " %s", p);
                        free(p);
                    }
                    fprintf(df, "\n");
                }
            }
            fprintf(df, "    bwd:\n");
            eno = 0;
            for (struct edge *edge = node->bwd; edge != NULL; edge = edge->bwdnext, eno++) {
                fprintf(df, "        %d:\n", eno);
                fprintf(df, "            node: %d (%d)\n", edge->src->id, edge->src->component);
                struct context *ctx = value_get(edge->ctx, NULL);
                fprintf(df, "            context before: %"PRIx64" %d\n", edge->ctx, ctx->pc);
                ctx = value_get(edge->after, NULL);
                fprintf(df, "            context after:  %"PRIx64" %d\n", edge->after, ctx->pc);
                if (edge->choice != 0) {
                    fprintf(df, "            choice: %s\n", value_string(edge->choice));
                }
                if (edge->nlog > 0) {
                    fprintf(df, "            log:");
                    for (int j = 0; j < edge->nlog; j++) {
                        char *p = value_string(edge_log(edge)[j]);
                        fprintf(df, " %s", p);
                        free(p);
                    }
                    fprintf(df, "\n");
                }
            }
        }
        fclose(df);
    }

    if (false) {
        FILE *df = fopen("charm.dump", "w");
        assert(df != NULL);
        char **table = malloc(global->graph.size * sizeof(char*));
        for (unsigned int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            table[i] = state_string(node->state);
            fprintf(df, "%s\n", table[i]);
        }
        fclose(df);
    }
#endif // OBSOLETE

    // Look for data races
    // TODO.  Can easily be parallelized
	// TODO.  Don't need failures/warnings distinction any more
    struct minheap *warnings = minheap_create(fail_cmp);
    if (minheap_empty(global->failures)) {
        printf("Check for data races\n");
        for (unsigned int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            graph_check_for_data_race(node, warnings, &engine);
            if (!minheap_empty(warnings)) {
                break;
            }
        }
    }

    bool no_issues = minheap_empty(global->failures) && minheap_empty(warnings);
    if (no_issues) {
        printf("No issues\n");
    }

    FILE *out = fopen(outfile, "w");
    if (out == NULL) {
        fprintf(stderr, "charm: can't create %s\n", outfile);
        exit(1);
    }

    printf("Phase 4: write results to %s\n", outfile);
    fflush(stdout);

    global->pretty = dict_lookup(jv->u.map, "pretty", 6);
    assert(global->pretty->type == JV_LIST);

    fprintf(out, "{\n");

    if (no_issues) {
        fprintf(out, "  \"issue\": \"No issues\",\n");
        fprintf(out, "  \"hvm\": ");
        json_dump(jv, out, 2);
        fprintf(out, ",\n");

        destutter1(&global->graph);

        // Output the symbols;
        struct dict *symbols = collect_symbols(&global->graph);
        fprintf(out, "  \"symbols\": {\n");
        struct symbol_env se = { .global = global, .out = out, .first = true };
        dict_iter(symbols, print_symbol, &se);
        fprintf(out, "\n");
        fprintf(out, "  },\n");

        // Only output nodes if there are symbols
        fprintf(out, "  \"nodes\": [\n");
        bool first = true;
        for (unsigned int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            assert(node->id == i);
            if (node->reachable) {
                if (first) {
                    first = false;
                }
                else {
                    fprintf(out, ",\n");
                }
                fprintf(out, "    {\n");
                fprintf(out, "      \"idx\": %d,\n", node->id);
                fprintf(out, "      \"component\": %d,\n", node->component);
#ifdef notdef
                if (node->parent != NULL) {
                    fprintf(out, "      \"parent\": %d,\n", node->parent->id);
                }
                char *val = json_escape_value(node->state->vars);
                fprintf(out, "      \"value\": \"%s:%d\",\n", val, node->state->choosing != 0);
                free(val);
#endif
                print_transitions(out, symbols, node->fwd);
                if (i == 0) {
                    fprintf(out, "      \"type\": \"initial\"\n");
                }
                else if (node->final) {
                    fprintf(out, "      \"type\": \"terminal\"\n");
                }
                else {
                    fprintf(out, "      \"type\": \"normal\"\n");
                }
                fprintf(out, "    }");
            }
        }
        fprintf(out, "\n");
        fprintf(out, "  ],\n");

        fprintf(out, "  \"profile\": [\n");
        for (unsigned int pc = 0; pc < global->code.len; pc++) {
            unsigned int count = 0;
            for (unsigned int i = 0; i < global->nworkers; i++) {
                struct worker *w = &workers[i];
                count += w->profile[pc];
            }
            if (pc > 0) {
                fprintf(out, ",\n");
            }
            fprintf(out, "    %u", count);
        }
        fprintf(out, "\n");
        fprintf(out, "  ]\n");
    }
    else {
        // Find shortest "bad" path
        struct failure *bad = NULL;
        if (minheap_empty(global->failures)) {
            bad = minheap_getmin(warnings);
        }
        else {
            bad = minheap_getmin(global->failures);
        }

        // printf("BAD: %d %"PRIx64" %"PRIx64"\n", bad->edge->dst->id,
        //                    bad->edge->ctx, bad->edge->after);

        switch (bad->type) {
        case FAIL_SAFETY:
            printf("Safety Violation\n");
            fprintf(out, "  \"issue\": \"Safety violation\",\n");
            break;
        case FAIL_INVARIANT:
            {
                printf("Invariant Violation\n");
                assert(VALUE_TYPE(bad->address) == VALUE_PC);
                fprintf(out, "  \"issue\": \"Invariant violation\",\n");
                fprintf(out, "  \"invpc\": %d,\n", (int) VALUE_FROM_PC(bad->address));
            }
            break;
        case FAIL_BEHAVIOR:
            printf("Behavior Violation: terminal state not final\n");
            fprintf(out, "  \"issue\": \"Behavior violation: terminal state not final\",\n");
            break;
        case FAIL_TERMINATION:
            printf("Non-terminating state\n");
            fprintf(out, "  \"issue\": \"Non-terminating state\",\n");
            break;
        case FAIL_BUSYWAIT:
            printf("Active busy waiting\n");
            fprintf(out, "  \"issue\": \"Active busy waiting\",\n");
            break;
        case FAIL_RACE:
            assert(bad->address != VALUE_ADDRESS);
            char *addr = value_string(bad->address);
            char *json = json_string_encode(addr, strlen(addr));
            printf("Data race (%s)\n", json);
            fprintf(out, "  \"issue\": \"Data race (%s)\",\n", json);
            free(json);
            free(addr);
            break;
        default:
            panic("main: bad fail type");
        }

        fprintf(out, "  \"hvm\": ");
        json_dump(jv, out, 2);
        fprintf(out, ",\n");

        // If it was an invariant failure, add one more macrostep
        // to replay the invariant code.
        struct edge *edge;
        if (bad->type == FAIL_INVARIANT) {
            struct context *inv_ctx = calloc(1, sizeof(struct context) +
                                MAX_CONTEXT_STACK * sizeof(hvalue_t));
            inv_ctx->pc = VALUE_FROM_PC(bad->address);
            inv_ctx->vars = VALUE_DICT;
            inv_ctx->atomic = 1;
            inv_ctx->atomicFlag = true;
            inv_ctx->readonly = 1;

            hvalue_t args[2];
            args[0] = bad->edge->src->state->vars;
            args[1] = bad->edge->dst->state->vars;
            value_ctx_push(inv_ctx, value_put_list(&engine, args, sizeof(args)));

            hvalue_t inv_context = value_put_context(&engine, inv_ctx);

            edge = calloc(1, sizeof(struct edge));
            edge->src = edge->dst = bad->edge->dst;
            edge->ctx = inv_context;
            edge->choice = 0;
            edge->interrupt = false;
            edge->weight = 0;
            edge->after = inv_context;
            edge->ai = NULL;
            edge->nlog = 0;
            edge->nsteps = 10000000;

            global->processes = realloc(global->processes, (global->nprocesses + 1) * sizeof(hvalue_t));
            global->callstacks = realloc(global->callstacks, (global->nprocesses + 1) * sizeof(struct callstack *));
            global->processes[global->nprocesses] = inv_context;
            struct callstack *cs = new_alloc(struct callstack);
            cs->pc = inv_ctx->pc;
            cs->arg = VALUE_LIST;
            cs->vars = VALUE_DICT;
            // TODO.  What's the purpose of the next line exactly?
            cs->return_address = (inv_ctx->pc << CALLTYPE_BITS) | CALLTYPE_PROCESS;
            global->callstacks[global->nprocesses] = cs;
            global->nprocesses++;
        }
        else {
            edge = bad->edge;
        }

        fprintf(out, "  \"macrosteps\": [");
#ifdef PATH_DUMP
        struct state *oldstate = calloc(1, sizeof(struct state) + MAX_CONTEXT_BAG * (sizeof(hvalue_t) + 1));
        struct context *oldctx = calloc(1, sizeof(struct context) +
                        MAX_CONTEXT_STACK * sizeof(hvalue_t));
        global->dumpfirst = true;
        path_dump(global, out, edge, oldstate, oldctx);
        free(oldctx);
#endif
        path_recompute(global, edge);
        if (bad->type == FAIL_INVARIANT || bad->type == FAIL_SAFETY) {
            path_trim(global, &engine);
        }
        path_output(global, out);

        fprintf(out, "\n");
        fprintf(out, "  ]\n");
    }

    fprintf(out, "}\n");
	fclose(out);

    iface_write_spec_graph_to_file(global, "iface.gv");
    iface_write_spec_graph_to_json_file(global, "iface.json");

    free(global);
    return 0;
}
