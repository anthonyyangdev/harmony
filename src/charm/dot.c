#include <stdlib.h>
#include <assert.h>
#include <string.h>

#ifndef HARMONY_COMBINE
#include "dot.h"
#endif

struct dot_graph_t *dot_graph_init(int alloc_len) {
    struct dot_graph_t *graph = malloc(sizeof(struct  dot_graph_t));
    graph->nodes = malloc(alloc_len * sizeof(struct dot_node_t));
    graph->_alloc_len = alloc_len;
    graph->len = 0;
    return graph;
}

int dot_graph_new_node(struct dot_graph_t *graph, const char *name) {
    struct dot_node_t node;
    node.name = name;
    node.fwd = NULL;
    node.fwd_len = 0;

    int node_idx = graph->len;
    graph->len++;
    assert(graph->len <= graph->_alloc_len);
    if (graph->len == graph->_alloc_len) {
        graph->_alloc_len = 2 * graph->len;
        graph->nodes = realloc(graph->nodes, graph->_alloc_len * sizeof(struct dot_node_t));
    }

    graph->nodes[node_idx] = node;
    return node_idx;
}

void dot_graph_add_edge(struct dot_graph_t *graph, int from_idx, int to_idx) {
    struct dot_node_t from_node = graph->nodes[from_idx];
    for (int i = 0; i < from_node.fwd_len; i++) {
        if (from_node.fwd[i] == to_idx) {
            return;
        }
    }

    from_node.fwd_len++;
    if (from_node.fwd != NULL) {
        from_node.fwd = realloc(from_node.fwd,from_node.fwd_len * sizeof(int));
    } else {
        from_node.fwd = malloc(from_node.fwd_len * sizeof(int));
    }

    from_node.fwd[from_node.fwd_len-1] = to_idx;
    graph->nodes[from_idx] = from_node;
}

void dot_graph_fprint(struct dot_graph_t *graph, FILE *f) {
    fprintf(f, "digraph {\n");
    for (int node_idx = 0; node_idx < graph->len; node_idx++) {
        struct dot_node_t node = graph->nodes[node_idx];
        for (int fwd_idx = 0; fwd_idx < node.fwd_len; fwd_idx++) {
            struct dot_node_t other = graph->nodes[node.fwd[fwd_idx]];
            fprintf(f, "  \"%s\" -> \"%s\"\n", node.name, other.name);
        }
    }
    fprintf(f, "}\n");
}
