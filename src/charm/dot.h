#ifndef SRC_DOT_H
#define SRC_DOT_H

#include <stdio.h>

struct dot_node_t {
    const char *name;     // null-terminated string
    int *fwd;       // forward edges
    int fwd_len;    // number forward edges
};

struct dot_graph_t {
    struct dot_node_t *nodes;
    int len;
    int _alloc_len;
};

struct dot_graph_t *dot_graph_init(int alloc_len);
int dot_graph_new_node(struct dot_graph_t *graph, const char *name);
void dot_graph_add_edge(struct dot_graph_t *graph, int from_idx, int to_idx);
void dot_graph_fprint(struct dot_graph_t *graph, FILE *f);

#endif //SRC_DOT_H
