#include "logic/dl_trace.h"

/* The shared why-trace format (see dl_trace.h). This reproduces, exactly, the
 * output both backings printed before unification — tests/test_col.c pins the
 * two against each other byte-for-byte, and tests/test_dl.c / test_col.c pin the
 * literal strings. Keep the spacing and punctuation as-is. */

static const char *verdict_str(dl_verdict v)
{
    switch (v) {
    case DL_PROVED:  return "PROVED";
    case DL_REFUTED: return "REFUTED";
    default:         return "UNDECIDED";
    }
}

static void render_rule_line(const dl_trace_vtbl *v, void *ctx, int r,
                             int indent, FILE *out)
{
    static const char *kinds[] = { "strict", "defeasible", "defeater" };
    const char *prov = v->rule_prov ? v->rule_prov(ctx, r) : NULL;
    fprintf(out, "%*s", indent, "");
    v->put_rule(ctx, r, out);
    if (prov)
        fprintf(out, " (%s; %s): ", kinds[v->rule_kind(ctx, r)], prov);
    else
        fprintf(out, " (%s): ", kinds[v->rule_kind(ctx, r)]);

    int nb = v->nbody(ctx, r);
    if (nb == 0)
        fprintf(out, "(no conditions)");
    for (int i = 0; i < nb; i++) {
        if (i != 0)
            fprintf(out, ", ");
        dl_lit bl = v->body_at(ctx, r, i);
        v->put_lit(ctx, bl, out);
        fprintf(out, "[%s]", verdict_str(v->defeasible(ctx, bl)));
    }
    int app = v->applicable(ctx, r);
    fprintf(out, "  -- %s\n",
            app == 1 ? "applicable" : app == -1 ? "inapplicable" : "undecided");
}

void dl_trace_render(const dl_trace_vtbl *v, void *ctx, dl_lit q, FILE *out)
{
    dl_lit nq = dl_complement(q);

    fprintf(out, "why ");
    v->put_lit(ctx, q, out);
    fprintf(out, "?\n  definite: %s   defeasible: %s\n",
            verdict_str(v->definite(ctx, q)),
            verdict_str(v->defeasible(ctx, q)));
    if (v->is_fact(ctx, q))
        fprintf(out, "  it is a base fact\n");
    if (!v->solved(ctx)) {
        fprintf(out, "  (family not solved)\n");
        return;
    }

    int nfor = v->nhead(ctx, q);
    if (nfor > 0) {
        fprintf(out, "  rules for it:\n");
        for (int k = 0; k < nfor; k++)
            render_rule_line(v, ctx, v->head_at(ctx, q, k), 4, out);
    }

    int nagainst = v->nhead(ctx, nq);
    if (nagainst > 0) {
        fprintf(out, "  rules against it:\n");
        for (int k = 0; k < nagainst; k++) {
            int si = v->head_at(ctx, nq, k);
            render_rule_line(v, ctx, si, 4, out);
            for (int j = 0; j < nfor; j++) {
                int ti = v->head_at(ctx, q, j);
                if (v->rule_kind(ctx, ti) != DL_DEFEATER && v->beats(ctx, ti, si)) {
                    fprintf(out, "      (beaten by ");
                    v->put_rule(ctx, ti, out);
                    fprintf(out, " if applicable)\n");
                }
                if (v->beats(ctx, si, ti)) {
                    fprintf(out, "      (superior to ");
                    v->put_rule(ctx, ti, out);
                    fprintf(out, ": ");
                    v->put_rule(ctx, si, out);
                    fprintf(out, " > ");
                    v->put_rule(ctx, ti, out);
                    fprintf(out, ")\n");
                }
            }
        }
    }
}
