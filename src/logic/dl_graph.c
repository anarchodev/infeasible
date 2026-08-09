#include "logic/dl_graph.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

int dl_tarjan(int n, const int32_t *aoff, const int32_t *ato, int32_t *comp)
{
    int32_t *low = malloc((size_t)(n ? n : 1) * sizeof *low);
    int32_t *idx = malloc((size_t)(n ? n : 1) * sizeof *idx);
    int32_t *stk = malloc((size_t)(n ? n : 1) * sizeof *stk);
    bool    *onstk = calloc((size_t)(n ? n : 1), 1);
    /* explicit frames: the recursion depth is the DFS tree's, which on a deep
     * rule chain is the whole literal set */
    int32_t *frame_v = malloc((size_t)(n ? n : 1) * sizeof *frame_v);
    int32_t *frame_e = malloc((size_t)(n ? n : 1) * sizeof *frame_e);
    for (int i = 0; i < n; i++) idx[i] = -1;
    int counter = 0, sp = 0, ncomp = 0;
    for (int root = 0; root < n; root++) {
        if (idx[root] >= 0) continue;
        int fp = 0;
        frame_v[fp] = root; frame_e[fp] = aoff[root];
        idx[root] = low[root] = counter++;
        stk[sp++] = root; onstk[root] = true;
        while (fp >= 0) {
            int v = frame_v[fp];
            if (frame_e[fp] < aoff[v + 1]) {
                int w = ato[frame_e[fp]++];
                if (idx[w] < 0) {
                    idx[w] = low[w] = counter++;
                    stk[sp++] = w; onstk[w] = true;
                    fp++;
                    frame_v[fp] = w; frame_e[fp] = aoff[w];
                } else if (onstk[w] && idx[w] < low[v]) {
                    low[v] = idx[w];
                }
            } else {
                if (low[v] == idx[v]) {
                    int m;
                    do {
                        m = stk[--sp];
                        onstk[m] = false;
                        comp[m] = ncomp;
                    } while (m != v);
                    ncomp++;
                }
                fp--;
                if (fp >= 0 && low[v] < low[frame_v[fp]])
                    low[frame_v[fp]] = low[v];
            }
        }
    }
    free(low); free(idx); free(stk); free(onstk);
    free(frame_v); free(frame_e);
    return ncomp;
}

void dl_group_by_comp(int n, int ncomp, const int32_t *comp,
                      int32_t **off, int32_t **lit)
{
    *off = calloc((size_t)ncomp + 2, sizeof **off);
    *lit = malloc((size_t)(n ? n : 1) * sizeof **lit);
    for (int i = 0; i < n; i++) (*off)[comp[i] + 1]++;
    for (int c = 0; c < ncomp; c++) (*off)[c + 1] += (*off)[c];
    int32_t *fill = malloc((size_t)(ncomp ? ncomp : 1) * sizeof *fill);
    memcpy(fill, *off, (size_t)ncomp * sizeof *fill);
    for (int i = 0; i < n; i++) (*lit)[fill[comp[i]]++] = i;
    free(fill);
}
