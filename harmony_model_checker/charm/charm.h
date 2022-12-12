#ifndef SRC_CHARM_H
#define SRC_CHARM_H

#include <stdatomic.h>

#include "minheap.h"
#include "code.h"
#include "graph.h"
#include "json.h"
#include "hashtab.h"

struct scc {        // Strongly Connected Component
    struct scc *next;
    unsigned int start, finish;
};

#define USE_HASHTAB

struct invariant {
    unsigned int pc;                // location of invariant code
    // TODO.  May not need the following since we can get it from env
    bool pre;                       // uses "pre" or not
};

// Info about a microstep
struct microstep {
    struct microstep *next;
    struct state *state;
    struct context *ctx;
    bool interrupt, choose;
    hvalue_t choice, print;
    struct callstack *cs;
    char *explain;
};

// Info about a macrostep (edge in Kripke structure)
struct macrostep {
    struct macrostep *next;
    struct edge *edge;
    unsigned int tid;
    hvalue_t name, arg;
    struct callstack *cs;
    unsigned int nmicrosteps, alloc_microsteps;
    struct microstep **microsteps;
    struct instr *trim;             // first instruction if trimmed
    hvalue_t value;                 // corresponding value

    hvalue_t *processes;            // array of contexts of processes
    struct callstack **callstacks;  // array of callstacks of processes
    unsigned int nprocesses;        // the number of processes in the list
};

struct global {
    struct code code;               // code of the Harmony program
    struct hashtab *values;         // dictionary of values
    hvalue_t seqs;                  // sequential variables

    // invariants
    mutex_t inv_lock;               // lock on list of invariants
    unsigned int ninvs;             // number of invariants
    struct invariant *invs;         // list of invariants
    bool inv_pre;                   // some invariant uses "pre"

    struct graph graph;             // the Kripke structure
    _Atomic(unsigned int) atodo;
    _Atomic(unsigned int) goal;
    // unsigned int todo;           // points into graph->nodes
    bool layer_done;                // all states in a layer completed

#define SPIN        // TODO
#ifdef SPIN
    pthread_spinlock_t todo_lock;
#else
    mutex_t todo_lock;              // to access the todo list
#endif
    mutex_t todo_wait;              // to wait for SCC tasks
    unsigned int nworkers;          // total number of threads
    unsigned int scc_nwaiting;      // # workers waiting for SCC work
    unsigned int ncomponents;       // to generate component identifiers
    struct minheap *failures;       // queue of "struct failure"  (TODO: make part of struct node "issues")
    hvalue_t *processes;            // array of contexts of processes
    struct callstack **callstacks;  // array of callstacks of processes
    unsigned int nprocesses;        // the number of processes in the list
    double lasttime;                // since last report printed
    unsigned int last_nstates;      // to measure #states / second
    struct dfa *dfa;                // for tracking correct behaviors
    unsigned int diameter;          // graph diameter
    bool phase2;                    // when model checking is done and graph analysis starts
    struct scc *scc_todo;           // SCC search
    struct json_value *pretty;      // for output
    bool run_direct;                // non-model-checked mode
    unsigned long allocated;        // allocated table space

    // Reconstructed error trace stored here
    unsigned int nmacrosteps, alloc_macrosteps;
    struct macrostep **macrosteps;
};

#endif //SRC_CHARM_H
