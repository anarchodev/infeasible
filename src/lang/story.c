#include "lang/story.h"
#include "lang/lexer.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MAX_CONJ      64
#define MAX_FLUENTS   512
#define MAX_LABELS    512
#define MAX_LABEL_LEN 64
#define MAX_ATOMS     1024

typedef struct {
    lexer        lx;
    token        cur;
    intern      *syms;
    world       *w;
    story_diags *diags;
    int          nerrors;    /* total errors, tracked even without a sink */
    bool         err_flag;   /* an error hit in the current declaration */

    uint32_t fluents[MAX_FLUENTS];
    int      nfluents;

    struct { char name[MAX_LABEL_LEN]; int handle; } labels[MAX_LABELS];
    int      nlabels;

    /* orphan/typo analysis (§6.1): concludable atoms are declared fluents plus
     * rule heads; a body/guard/requires atom that is neither is silently
     * always-false — the Osiris typo bug. Both dedup to distinct atoms; refs
     * keep the first source location for the diagnostic. Collected across the
     * whole file (heads may be declared after their uses) and checked once at
     * the end. Best-effort: on overflow we stop collecting. */
    uint32_t heads[MAX_ATOMS];
    int      nheads;
    struct { uint32_t atom; int line, col; } refs[MAX_ATOMS];
    int      nrefs;
} parser;

/* ---- diagnostics ---------------------------------------------------- */

static void add_diag(parser *p, story_severity sev, int line, int col,
                     const char *fmt, va_list ap)
{
    if (sev == STORY_ERROR) p->nerrors++;
    story_diags *d = p->diags;
    if (!d) return;
    int idx = d->count++;
    if (sev == STORY_ERROR) d->nerrors++;
    if (d->items && idx < d->cap) {
        story_diag *dg = &d->items[idx];
        dg->sev = sev;
        dg->line = line;
        dg->col = col;
        vsnprintf(dg->msg, sizeof dg->msg, fmt, ap);
    }
}

/* Report one error and enter recovery for the current declaration. At most
 * one error is recorded per declaration (err_flag); the top-level loop
 * synchronises to the next declaration boundary and clears it. */
static void fail(parser *p, int line, int col, const char *fmt, ...)
{
    if (p->err_flag) return;
    p->err_flag = true;
    va_list ap;
    va_start(ap, fmt);
    add_diag(p, STORY_ERROR, line, col, fmt, ap);
    va_end(ap);
}

static void warn(parser *p, int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    add_diag(p, STORY_WARNING, line, col, fmt, ap);
    va_end(ap);
}

/* Human-readable spelling of an actual token: show the text for names and
 * numbers, the canonical spelling otherwise. */
static void tok_desc(token t, char *buf, size_t n)
{
    if (t.kind == TK_IDENT || t.kind == TK_INT)
        snprintf(buf, n, "'%.*s'", t.len, t.start);
    else
        snprintf(buf, n, "%s", tok_kind_name(t.kind));
}

/* ---- token stream --------------------------------------------------- */

static void advance(parser *p) { p->cur = lexer_next(&p->lx); }

static bool expect(parser *p, tok_kind k)
{
    if (p->cur.kind == k) { advance(p); return true; }
    char d[64];
    tok_desc(p->cur, d, sizeof d);
    fail(p, p->cur.line, p->cur.col, "expected %s, found %s",
         tok_kind_name(k), d);
    return false;
}

static uint32_t intern_tok(parser *p, token t)
{
    char buf[256];
    int n = t.len < (int)sizeof buf - 1 ? t.len : (int)sizeof buf - 1;
    memcpy(buf, t.start, (size_t)n);
    buf[n] = '\0';
    return intern_id(p->syms, buf);
}

/* ---- fluent set & rule labels --------------------------------------- */

static bool is_declared_fluent(parser *p, uint32_t atom)
{
    for (int i = 0; i < p->nfluents; i++)
        if (p->fluents[i] == atom) return true;
    return false;
}

static void declare_fluent(parser *p, uint32_t atom, int line, int col)
{
    if (is_declared_fluent(p, atom)) return;      /* idempotent redeclare */
    if (p->nfluents >= MAX_FLUENTS) {
        fail(p, line, col, "too many fluents (max %d)", MAX_FLUENTS);
        return;
    }
    p->fluents[p->nfluents++] = atom;
    world_declare_fluent(p->w, atom);
}

/* ---- orphan/typo analysis ------------------------------------------- */

static void note_head(parser *p, uint32_t atom)
{
    for (int i = 0; i < p->nheads; i++)
        if (p->heads[i] == atom) return;
    if (p->nheads < MAX_ATOMS) p->heads[p->nheads++] = atom;
}

static bool is_head(parser *p, uint32_t atom)
{
    for (int i = 0; i < p->nheads; i++)
        if (p->heads[i] == atom) return true;
    return false;
}

static void note_ref(parser *p, uint32_t atom, int line, int col)
{
    for (int i = 0; i < p->nrefs; i++)
        if (p->refs[i].atom == atom) return;          /* keep first location */
    if (p->nrefs < MAX_ATOMS) {
        p->refs[p->nrefs].atom = atom;
        p->refs[p->nrefs].line = line;
        p->refs[p->nrefs].col = col;
        p->nrefs++;
    }
}

/* Any atom used in a condition that is neither a declared fluent nor concluded
 * by some rule can never be true — flag it as a probable typo or a missing
 * declaration. */
static void check_orphans(parser *p)
{
    for (int i = 0; i < p->nrefs; i++) {
        uint32_t a = p->refs[i].atom;
        if (is_declared_fluent(p, a) || is_head(p, a)) continue;
        warn(p, p->refs[i].line, p->refs[i].col,
             "'%s' is used as a condition but is never a declared fluent or "
             "concluded by any rule — typo, or a missing declaration?",
             intern_name(p->syms, a));
    }
}

static int find_label(parser *p, token t)
{
    for (int i = 0; i < p->nlabels; i++)
        if ((int)strlen(p->labels[i].name) == t.len &&
            memcmp(p->labels[i].name, t.start, (size_t)t.len) == 0)
            return p->labels[i].handle;
    return -1;
}

static bool add_label(parser *p, token t, int handle)
{
    if (t.len >= MAX_LABEL_LEN) {
        fail(p, t.line, t.col, "rule label too long (max %d chars)",
             MAX_LABEL_LEN - 1);
        return false;
    }
    if (find_label(p, t) >= 0) {
        fail(p, t.line, t.col, "duplicate rule label '%.*s'", t.len, t.start);
        return false;
    }
    if (p->nlabels >= MAX_LABELS) {
        fail(p, t.line, t.col, "too many rule labels (max %d)", MAX_LABELS);
        return false;
    }
    memcpy(p->labels[p->nlabels].name, t.start, (size_t)t.len);
    p->labels[p->nlabels].name[t.len] = '\0';
    p->labels[p->nlabels].handle = handle;
    p->nlabels++;
    return true;
}

/* ---- literals and conjunctions -------------------------------------- */

/* lit := [ '~' ] IDENT.  When `id_out` is non-NULL it receives the atom's
 * identifier token, so callers can record its source location. */
static bool parse_lit(parser *p, dl_lit *out, token *id_out)
{
    bool neg = false;
    if (p->cur.kind == TK_TILDE) { neg = true; advance(p); }
    if (p->cur.kind != TK_IDENT) {
        char d[64];
        tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected an atom name, found %s", d);
        return false;
    }
    token id = p->cur;
    if (id_out) *id_out = id;
    advance(p);
    if (p->cur.kind == TK_LPAREN) {
        fail(p, p->cur.line, p->cur.col,
             "predicate arguments are not supported in this slice "
             "(typed variables land with the M1 grounder)");
        return false;
    }
    uint32_t atom = intern_tok(p, id);
    *out = neg ? dl_neg(atom) : dl_pos(atom);
    return true;
}

/* conj := lit ( '&' lit )*  — greedy; a bare name with no leading '&'
 * belongs to the next construct, which is what lets rule/action/superiority
 * parse without newline sensitivity. `note_body` records each literal as a
 * condition reference for the orphan analysis (heads/effects pass false). */
static int parse_conj(parser *p, dl_lit *out, int cap, bool note_body)
{
    token id;
    if (!parse_lit(p, &out[0], &id)) return -1;
    if (note_body) note_ref(p, out[0].atom, id.line, id.col);
    int n = 1;
    while (p->cur.kind == TK_AMP) {
        advance(p);
        if (n >= cap) {
            fail(p, p->cur.line, p->cur.col,
                 "conjunction too long (max %d literals)", cap);
            return -1;
        }
        if (!parse_lit(p, &out[n], &id)) return -1;
        if (note_body) note_ref(p, out[n].atom, id.line, id.col);
        n++;
    }
    return n;
}

/* ---- declarations --------------------------------------------------- */

/* Shared shape for `state`/`init`: a single bare item or a '(' group.
 * `set_init` selects the effect: declare a fluent, or set a declared one. */
static void parse_fluent_list(parser *p, bool set_init)
{
    advance(p);                                    /* 'state' / 'init' */
    bool grouped = false;
    if (p->cur.kind == TK_LPAREN) { grouped = true; advance(p); }

    do {
        if (p->cur.kind != TK_IDENT) {
            char d[64];
            tok_desc(p->cur, d, sizeof d);
            fail(p, p->cur.line, p->cur.col, "expected a fluent name, found %s", d);
            return;
        }
        token id = p->cur;
        advance(p);
        if (p->cur.kind == TK_LPAREN || p->cur.kind == TK_COLON) {
            fail(p, p->cur.line, p->cur.col,
                 "typed and predicate fluents are not supported in this slice "
                 "(booleans only; §5.7/5.8 domains land later in M1)");
            return;
        }
        uint32_t atom = intern_tok(p, id);
        if (set_init) {
            if (!is_declared_fluent(p, atom)) {
                fail(p, id.line, id.col,
                     "init names '%.*s', which is not a declared fluent",
                     id.len, id.start);
                return;
            }
            world_set(p->w, atom, true);
        } else {
            declare_fluent(p, atom, id.line, id.col);
            if (p->err_flag) return;
        }
    } while (grouped && p->cur.kind == TK_IDENT);

    if (grouped && !expect(p, TK_RPAREN)) return;
}

/* rule := 'rule' IDENT ':' conj OP lit [ 'unless' conj ] */
static void parse_rule(parser *p)
{
    advance(p);                                    /* 'rule' */
    if (p->cur.kind != TK_IDENT) {
        char d[64];
        tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected a rule label, found %s", d);
        return;
    }
    token label = p->cur;
    advance(p);
    if (!expect(p, TK_COLON)) return;

    dl_lit body[MAX_CONJ];
    int nb = parse_conj(p, body, MAX_CONJ, true);
    if (nb < 0) return;

    dl_rule_kind kind;
    switch (p->cur.kind) {
    case TK_ARROW:    kind = DL_STRICT;     break;
    case TK_FATARROW: kind = DL_DEFEASIBLE; break;
    case TK_SQARROW:  kind = DL_DEFEATER;   break;
    default: {
        char d[64];
        tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col,
             "expected a rule arrow ('->', '=>', or '~>'), found %s", d);
        return;
    }
    }
    advance(p);

    dl_lit head;
    if (!parse_lit(p, &head, NULL)) return;
    note_head(p, head.atom);

    char name[MAX_LABEL_LEN];
    int hn = label.len < MAX_LABEL_LEN - 1 ? label.len : MAX_LABEL_LEN - 1;
    memcpy(name, label.start, (size_t)hn);
    name[hn] = '\0';
    int handle = world_add_rule(p->w, name, kind, head, body, nb);
    if (!add_label(p, label, handle)) return;

    /* `unless G` sugars to a defeater blocking this rule's head: G ~> ~head
     * (DESIGN.md §6). Value-specific block; the norm is reinstated whenever
     * the guard fails. */
    if (p->cur.kind == TK_UNLESS) {
        advance(p);
        dl_lit guard[MAX_CONJ];
        int ng = parse_conj(p, guard, MAX_CONJ, true);
        if (ng < 0) return;
        char gname[MAX_LABEL_LEN + 8];
        snprintf(gname, sizeof gname, "%s.unless", name);
        world_add_rule(p->w, gname, DL_DEFEATER, dl_complement(head), guard, ng);
    }
}

/* action := 'action' IDENT ':' [ 'requires' conj ] 'causes' conj */
static void parse_action(parser *p)
{
    advance(p);                                    /* 'action' */
    if (p->cur.kind != TK_IDENT) {
        char d[64];
        tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected an action name, found %s", d);
        return;
    }
    token name = p->cur;
    advance(p);
    if (!expect(p, TK_COLON)) return;

    step_cond conds[MAX_CONJ];
    int nc = 0;
    if (p->cur.kind == TK_REQUIRES) {
        advance(p);
        dl_lit body[MAX_CONJ];
        int nb = parse_conj(p, body, MAX_CONJ, true);
        if (nb < 0) return;
        for (int i = 0; i < nb; i++) {
            conds[i].lit = body[i];
            conds[i].primed = false;               /* current-state guards */
        }
        nc = nb;
    }

    if (!expect(p, TK_CAUSES)) return;
    dl_lit effects[MAX_CONJ];
    int ne = parse_conj(p, effects, MAX_CONJ, false);
    if (ne < 0) return;

    char nm[MAX_LABEL_LEN];
    int hn = name.len < MAX_LABEL_LEN - 1 ? name.len : MAX_LABEL_LEN - 1;
    memcpy(nm, name.start, (size_t)hn);
    nm[hn] = '\0';
    uint32_t act = intern_tok(p, name);
    world_add_step_rule(p->w, nm, act, conds, nc, effects, ne);
}

/* sup := IDENT '>' IDENT  (label > label) */
static void parse_sup(parser *p)
{
    token a = p->cur;
    advance(p);
    if (!expect(p, TK_GT)) return;
    if (p->cur.kind != TK_IDENT) {
        char d[64];
        tok_desc(p->cur, d, sizeof d);
        fail(p, p->cur.line, p->cur.col, "expected a rule label, found %s", d);
        return;
    }
    token b = p->cur;
    advance(p);

    int ha = find_label(p, a);
    if (ha < 0) {
        fail(p, a.line, a.col, "unknown rule label '%.*s' in superiority "
             "(rules must be declared before the '>' that references them)",
             a.len, a.start);
        return;
    }
    int hb = find_label(p, b);
    if (hb < 0) {
        fail(p, b.line, b.col, "unknown rule label '%.*s' in superiority "
             "(rules must be declared before the '>' that references them)",
             b.len, b.start);
        return;
    }
    world_add_sup(p->w, ha, hb);
}

/* ---- entry ---------------------------------------------------------- */

/* Panic-mode recovery (§10): after an error, skip to the next declaration
 * boundary so a single bad declaration does not cascade. Only the structural
 * keywords are treated as boundaries — a bare identifier is ambiguous with
 * mid-declaration garbage, so we do not resynchronise on it (a superiority
 * statement stranded after a broken rule is skipped to the next keyword).
 * Every declaration parser consumes at least its leading token before it can
 * fail, so progress is guaranteed even when this returns immediately. */
static void synchronize(parser *p)
{
    while (p->cur.kind != TK_EOF) {
        switch (p->cur.kind) {
        case TK_STATE:
        case TK_INIT:
        case TK_RULE:
        case TK_ACTION:
            return;
        default:
            advance(p);
        }
    }
}

world *story_compile(const char *src, intern *syms, story_diags *diags)
{
    parser p;
    memset(&p, 0, sizeof p);
    lexer_init(&p.lx, src);
    p.syms = syms;
    p.w = world_new(syms);
    p.diags = diags;
    if (diags) { diags->count = 0; diags->nerrors = 0; }

    advance(&p);                                   /* prime lookahead */
    while (p.cur.kind != TK_EOF) {
        p.err_flag = false;                        /* one error per decl */
        switch (p.cur.kind) {
        case TK_STATE:  parse_fluent_list(&p, false); break;
        case TK_INIT:   parse_fluent_list(&p, true);  break;
        case TK_RULE:   parse_rule(&p);               break;
        case TK_ACTION: parse_action(&p);             break;
        case TK_IDENT:  parse_sup(&p);                break;
        default: {
            char d[64];
            tok_desc(p.cur, d, sizeof d);
            fail(&p, p.cur.line, p.cur.col,
                 "expected a declaration (state/init/rule/action) "
                 "or a superiority statement, found %s", d);
            break;
        }
        }
        if (p.err_flag) synchronize(&p);
    }

    if (p.nerrors > 0) {
        world_free(p.w);
        return NULL;
    }

    /* Non-fatal analysis runs only on a well-formed parse. */
    check_orphans(&p);
    return p.w;
}
