#include "state/world.h"
#include "state/factindex.h"
#include "core/arena.h"
#include "core/grow.h"
#include "core/intern.h"
#include "logic/dl_col.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *prov;     /* provenance suffix (§6.3), or NULL */
    dl_rule_kind kind;
    dl_lit head;
    dl_lit *body;
    int nbody;
} jrule;

typedef struct { int winner, loser; } jsup;

typedef struct {
    uint32_t num_atom;    /* ground numeric fluent this writes */
    world_numop op;
    const expr_ins *code; /* RHS bytecode (arena-copied) */
    int ncode;
} num_effect;

typedef struct {
    const char *name;
    const char *prov;     /* provenance suffix (§6.3), or NULL */
    uint32_t action;      /* INTERN_NONE = ramification */
    step_cond *body;
    int nbody;
    dl_lit *effects;      /* primed heads */
    int neffects;
    num_effect *neffs;    /* numeric effects (§5.8 write side) */
    int nneff, capneff;
    int stratum;          /* §5.8 strata (#87): 0 = settles in the first solve */
    uint32_t lane_cover;  /* #121 mixed routing: bit v set = a per-value lane
                           * family handles this rule under split value v, so
                           * the residue schema omits it. 0 = never covered. */
} srule;

/* Per-numeric-fluent commit receipt, rebuilt each successful step. */
typedef struct {
    long base;
    world_contrib *items;
    int n, cap;
} num_receipt;

/* A lane family (the DoD thesis): one dl_col schema over `nent` lane entities.
 * `niter` is the join iteration count — 1 for a single-variable rule set (solve
 * once); for a two-variable rule it is the size of the non-lane sort, and the
 * family is re-solved per iteration against a different fact slice (lane one
 * axis, iterate the other). `ground[(a*niter + it)*nent + e]` is the named
 * ground atom for predicate-local `a` at iteration `it`, lane `e`; `is_fluent[a]`
 * flags locals that take base facts. */
typedef struct {
    dlcol   *fam;
    int      natoms, nent, niter;
    uint32_t *ground;             /* [natoms*niter*nent] */
    bool    *is_fluent;           /* [natoms]: local takes closed-world base facts */
    bool    *is_import;           /* [natoms]: local is derived elsewhere, its
                                   * per-cell verdict queried and injected (§5.5) */
    bool     solved;              /* fam currently holds iteration cur_it, and
                                   * that solve reflects the live base facts */
    int      cur_it;              /* which iteration is loaded/solved in fam
                                   * (always 0 when niter==1) */
} lane_family;

/* reverse map: a named ground atom -> its (family, predicate-local, lane,
 * iteration), so world_query can route to the lane family. fam < 0 = not a
 * routable lane atom (unmentioned, or ambiguous within a join family). */
typedef struct { int fam, a, e, it; } lane_ref;

/* A step lane family: the transition theory (generated inertia + causal rules)
 * over one lane sort, bit-parallel across `nent` entities. Each local is a
 * current fluent (loaded closed-world from the fact store), a primed fluent (the
 * next-state readout), or an action (loaded from the step's action list).
 * `ground[a*nent + e]` names the equivalent scalar atom for local `a`, lane `e` —
 * the primed local's is the fluent's `f'` twin, so the differential can look up
 * the N=1 verdict. Prototype-before-adopt: validated, not yet driving world_step. */
typedef struct {
    dlcol   *fam;
    int      nloc, nent;
    uint32_t *ground;             /* [nloc*nent] */
    uint8_t *kind;                /* [nloc]: WORLD_STEP_{CUR,PRIMED,ACTION} */
    int     *fl_of;               /* [nloc*nent]: world fluent index for a CUR cell
                                   * (fact source) or PRIMED cell (commit target),
                                   * -1 otherwise — so the solve reads w->vals and
                                   * the commit writes it directly, no linear scan */
    int     *act_of;              /* [act_of_cap]: ground action atom -> flat cell
                                   * (local*nent + lane), -1 if not an action cell —
                                   * so a step's action list maps to lanes in O(k) */
    uint32_t act_of_cap;

    /* Numeric lane extension (§5.8 write side, bit-parallel). A numeric effect
     * does not defeat (every fired one contributes), so its firing is a synthetic
     * STRICT rule `body -> marker` solved with the boolean transition; the commit
     * then reads each marker column and sums deltas into the numeric column. */
    int      numsc;               /* # numeric fluent schemas over the lane sort */
    int     *num_cell;            /* [numsc*nent]: schema s, lane e -> w->nums index */
    int      nnumeff;             /* # numeric effect specs */
    struct num_lane_eff {
        int        schema;        /* into num_cell rows */
        world_numop op;
        long       konst;         /* S1: constant RHS (RHS constant-folds) */
        uint32_t   marker;        /* family-local: the fired marker (readout) */
        /* #165: an RHS reading k test() verdicts is a table of 2^k constants over
         * k bit-columns — the commit indexes it per lane, no per-entity VM. */
        int        ntest;
        uint32_t   tloc[WORLD_LANE_MAXTEST];    /* family-local column per read */
        uint8_t    tneg[WORLD_LANE_MAXTEST];    /* its polarity */
        long      *table;                       /* [1 << ntest] */
    } *numeff;
    bool     covers_numeric;      /* numeric commit is fully laned -> routable */

    /* broadcast cast triggers (`for each` binders): a ground cast atom maps to the
     * WORLD_STEP_BCAST local it drives; when it occurs, every lane of that local
     * is set (the cast fans out over the target lanes). -1 = not a cast atom. */
    int     *bcast_of;
    uint32_t bcast_of_cap;

    int split_value;              /* #121 mixed routing: the split value index
                                   * this per-value family serves, or -1 (the
                                   * classic whole-transition family). */
} step_lane_family;

struct world {
    arena a;
    intern *syms;
    uint32_t *fluents; bool *vals; uint32_t *primed;
    const char **fl_prov;         /* decl span per fluent (§6.3), or NULL */
    int nfl, capfl;
    /* Structured view per fluent for the tick-time extension index (#28):
     * fl_pred[i]=INTERN_NONE means "no structure" (absent from the index).
     * fl_args is flat, FACTINDEX_MAXARGS per fluent. */
    uint32_t *fl_pred; int *fl_nargs; uint32_t *fl_args;
    factindex *fidx;              /* extension index; built once, maintained live (#73) */
    /* atom -> index maps, so declare/lookup is O(1) not a linear scan (interns
     * are dense uint32, so a direct-indexed array is the natural perfect hash).
     * Grown geometrically; slot value -1 = absent. Fluents/nums are append-only. */
    int *fluent_of; uint32_t fluent_of_cap;
    /* Transient emissions (#11, §12): burst cues. Each declared emit atom keeps
     * a primed twin exactly like a fluent's, because an emission is concluded
     * about the NEXT state (it is an effect head) — but it takes no base fact,
     * no inertia and no commit: the step reads its primed verdict into
     * `emitbuf` and forgets it. */
    uint32_t *emits, *emit_primed; int nemit, capemit;
    int *emit_of; uint32_t emit_of_cap;      /* emit atom -> emits index */
    uint32_t *emitbuf; int nemitbuf, capemitbuf;   /* last step's stream */
    int *num_of;    uint32_t num_of_cap;
    int *guard_of;  uint32_t guard_of_cap;   /* guard atom  -> guards index  */
    int *prov_of;   uint32_t prov_of_cap;    /* provider atom -> provs index */
    int *eguard_of; uint32_t eguard_of_cap;  /* expr-guard atom -> eguards index */
    int *trig_of;   uint32_t trig_of_cap;    /* action atom -> 1 iff any step rule
                                              * triggers on it (loud no-ops, #119) */
    jrule *jrules; int njr, capjr;
    jsup *jsups; int njs, capjs;
    int jr_matched_base;                   /* watermark: static | matched rules (#28) */
    /* Matched rules' arena-copied names/bodies/prov live in their OWN arena so a
     * re-ground can free the previous layer instead of leaking it into w->a
     * (#48). `in_matched` (set at the checkpoint) routes world_add_rule /
     * world_set_rule_prov allocations there for every add past the boundary. */
    arena matched_a; bool in_matched;
    /* auto re-ground hook (#45): fired from ensure_jfam/ensure_fam when
     * matched_stale, to refresh the matched layer against current facts before a
     * solve. `regrounding` guards the callback against re-entering a rebuild. */
    world_reground_fn reground_fn; void *reground_ctx; bool matched_stale, regrounding;
    /* Optional: the world OWNS the re-ground context and disposes it at
     * world_free. Set when the compiler routes an over-cap rule to the matcher
     * on its own (#59) — the caller asked for a plain world and never learns a
     * matcher exists, so it cannot be the one to free it. Left NULL when a
     * caller builds the matcher explicitly and keeps ownership. */
    void (*reground_free)(void *);
    /* Predicates some matchable rule READS (#45). A base-fact edit only stales
     * the matched layer when it touches one of these — editing a fluent no
     * matchable rule mentions cannot change the match set, so re-deriving it
     * would be pure waste. Registered by the lang layer at rule capture.
     * Default-open: with nothing registered (`watch_set` false) every edit
     * stales, so a caller that never registers keeps the conservative
     * behaviour, and forgetting to register can only cost time, never
     * correctness. */
    int *watch_of; uint32_t watch_cap; bool watch_set;

    /* Matched views (#80): island judgments as sets instead of matched rules.
     * Rows are the per-tick match set (cleared by world_views_reset, caps
     * kept); vseen_of is the append-only ever-seen registry (atom -> view, -1
     * never) and vmark_of stamps the reset sequence an atom was last added
     * under, so "present" is vmark == vseq without any per-tick clearing. */
    struct { uint32_t head_pred; bool head_neg; dl_rule_kind kind; } *views;
    int nviews, capviews;
    struct vrow { uint32_t atom; int view; int nvars;
                  uint32_t bind[WORLD_VIEW_MAXBIND]; } *vrows;
    int nvrow, capvrow;
    uint32_t vseq;                 /* bumped by world_views_reset; 0 = pre-first */
    int *vseen_of; uint32_t vseen_cap;
    int *vmark_of; uint32_t vmark_cap;
    int *vmat_of;  uint32_t vmat_cap;   /* atom -> vseq it was last why-materialized
                                         * under (rules live until that re-ground) */
    world_materialize_fn materialize_fn; void *materialize_ctx;

    /* Fluent schema hook (#92): recognizes ground instances of declared
     * boolean state predicates by atom id, so touched fluents can be declared
     * lazily and never-touched ones answered closed-world. */
    world_schema_fn schema_fn; void *schema_ctx;

    srule *srules; int nsr, capsr;

    /* numeric value store + comparison guards (§5.8, read side). A clamp bound
     * may be dynamic (`int in 0 .. hp_max(X)`): lo_code/hi_code hold effect-VM
     * bytecode evaluated per-commit against the value store, NULL = use the
     * constant min/max. */
    struct { uint32_t atom; long value, min, max; bool has_range;
             const expr_ins *lo_code; int n_lo;
             const expr_ins *hi_code; int n_hi;
             world_merge merge; } *nums;   /* ASSIGN-class algebra (#85) */
    int nnum, capnum;
    struct { uint32_t guard, num; world_cmp op; long threshold; } *guards;
    int ng, capg;
    /* Primed guards (§5.8 #87): over the NEXT value, minted as strict facts by
     * the stratum loop once the owning fluent's next value settles. `pg_cur`
     * holds the facts minted so far this tick — kept after commit so a
     * why-replay of the final solve sees them; reset at the next step. */
    struct { uint32_t guard, num; world_cmp op; long threshold; } *pguards;
    int npg, cappg;
    struct { uint32_t atom; bool val; } *pg_cur;
    int npg_cur, cappg_cur;
    /* Structure-derived step scaffolding, cached per struct_ver so a step
     * never re-sweeps the O(nsr) rule table (#121 made the sweep the hot
     * spot once everything else was narrowed): the stratum count, the
     * per-numeric writer strata, and the srules that carry numeric effects. */
    uint64_t scaffold_ver;
    int scaffold_nsr, scaffold_nnum;    /* nums/rules can grow without a
                                         * struct_ver bump (world_declare_num) */
    int nstrata_cache;
    int *neff_rules; int n_neff_rules, cap_neff_rules;
    int *num_stratum;                   /* per nums[i]: max writer stratum;
                                         * recomputed per stratified step */
    int num_stratum_cap;
    const long *nn_cur;                 /* EXPR_LOADN (#84): the in-progress
                                         * nextnum during the stratum loop */
    int cur_stratum;
    /* Expression guards (§5.8/§5.10): `expr <op> expr` — e.g. roll(20)+atk >= ac.
     * Two RHS-bytecode programs compared at solve time; loads as a fact like a
     * numeric guard. Bytecode is arena-copied. `has_test` marks a guard whose
     * bytecode contains EXPR_TEST (#86 guard half): it cannot evaluate at fact
     * load (the tested verdict is the fixpoint's OUTPUT), so each solve runs
     * two-phase — solve without those guards, evaluate them against the
     * settled result into `tval`, reload with them, solve again. Sound because
     * the compiler rejects a tested atom whose cone contains a test-bearing
     * guard, so pass-A verdicts of tested atoms are final. `tval` is
     * tri-valued (#116): 1 holds, 0 fails, -1 UNDEFINED — the guard read a
     * partial value with no applicable definition, and asserts NEITHER fact
     * (the guard atom stays UNDECIDED in the solve). */
    struct { uint32_t guard; const expr_ins *lhs; int nlhs;
             const expr_ins *rhs; int nrhs; world_cmp op;
             bool has_test; int8_t tval; } *eguards;
    int neguard, capeguard;
    /* #159 exclusivity groups: per ground action atom, a chain of (group,
     * key) memberships; groups carry a label + declaration prov for the
     * rejection message. */
    struct { int grp; uint32_t key, atom; int32_t next; } *excl_ents;
    int nexcl, capexcl;
    int *excl_of; uint32_t excl_of_cap;        /* action atom -> first entry */
    struct { char *label, *prov; } *excl_grps;
    int negrp, capegrp;
    int n_teg;                    /* # test-bearing eguards (two-phase iff > 0) */
    bool teg_ready;               /* pass C: load has_test guards from tval */
    dlcol *tctx_fam;              /* EXPR_TEST eval context override for pass-B
                                   * evals against a non-step family (jfam);
                                   * NULL = the step family (fam/loc_of) */
    const uint32_t *tctx_of;
    uint32_t tctx_cap;

    /* Providers (§5.2/§5.6/§5.10): computed relations answered host-side, never
     * stored. Each ground provider atom records its predicate + entity args; at
     * solve time it loads as a fact from the registered callback (closed-world),
     * exactly like a numeric guard. Read-only: providers never appear as heads. */
    struct { uint32_t atom, pred; int nargs; uint32_t args[4]; } *provs;
    int nprov, capprov;
    world_provider_fn provider_fn; void *provider_ctx;
    /* value-returning function providers (§5.6): host functions consulted from
     * the effect-VM (EXPR_CALL), returning a cell handle / int. No ground-atom
     * table — a call carries its function pred + args in the bytecode. */
    world_fn_provider_fn fn_provider_fn; void *fn_provider_ctx;

    /* Seeded randomness (§5.10): a roll is a keyed lookup hash(seed, tick, site),
     * idempotent under re-read and independent across sites — so it can sit inside
     * the fixpoint. `tick` is a monotone step counter (deterministic, not
     * wall-clock). Each roll site records its die size + precomputed site key. */
    uint64_t seed, tick;
    struct { int sides; uint64_t site; } *rollsites;
    int nrollsite, caprollsite;

    /* commit receipts, one per numeric fluent (parallel to nums), valid only
     * immediately after a successful world_step (§5.8 write side). Sized lazily
     * and grown when new numeric fluents are declared after a step. */
    num_receipt *rcpt;
    int caprcpt;

    /* Cached columnar step schema (an N=1 family — DESIGN.md 5.8: single
     * derive is multiderive at N=1). The step theory's structure — judgment
     * rules, generated inertia, causal rules, superiority — is fixed
     * between steps; only the fact bits change. Rebuilt lazily when rules
     * or fluents are added; each world_step then just rewrites fact
     * columns and re-solves, paying no theory rebuild or compile. */
    dlcol *fam;                   /* the ACTIVE step family for this state —
                                   * borrows fam0 or a split cache slot below   */
    dlcol *fam0;                  /* the full (unsplit) step family; owned      */
    uint64_t fam0_ver;
    dlcol *jfam;                  /* judgment family: judgments only (the query
                                   * layer — DESIGN.md §6.3, one columnar engine
                                   * for both "what's true" and "what happens") */

    /* `split` (#121): per-value step-schema specialization on ONE designated
     * arity-0 finite-domain fluent (its MV erasure — the value atoms). Zero
     * semantic content: each cached schema omits rules statically dead under
     * that value and the inertia of fluents no live rule can touch; excluded
     * fluents commit by copy. Selection is by the PRE-step value; no unique
     * value true -> the full schema (fam0). nvals == 0 -> split inactive. */
    struct {
        uint32_t *vatoms; int nvals;  /* the value atoms, domain order (owned) */
        dlcol   **fams;               /* per-value schema cache; NULL = unbuilt */
        uint64_t *vers;               /* struct_ver each slot was built at      */
        uint8_t **flw;                /* [v][nfl]  write-set at build time      */
        uint8_t **numw;               /* [v][nnum] numeric write-set            */
        uint8_t *mixed;               /* [v] 1 = this value's schema omits its
                                       * lane-covered rules, so the step MUST
                                       * run the mixed lane/N=1 route          */
        /* The SPARSE residue space (#121 mixed, the #77 trick on the step
         * side): one compact loc space shared by every mixed value's residue
         * schema, holding only the atoms the residue can touch — judgment
         * rules, uncovered srules, their fluents' cur/primed twins. A mixed
         * step then pays O(residue), not O(all ground atoms). */
        uint32_t *smap; uint32_t smap_cap;    /* atom -> sparse loc (LOC_NONE) */
        uint32_t *satoms; int nsloc, capsloc; /* reverse: sparse loc -> atom   */
        uint32_t *sfl_loc, *spr_loc;          /* [nfl] fluent cur/primed locs  */
        uint32_t *sfl_list; int nsfl, capsfl; /* fluent indices present        */
        uint64_t space_ver;
        int *map_of; uint32_t map_cap;      /* value atom -> value index        */
        int *trig_of; uint32_t trig_cap;    /* action -> live-value bitmask     */
        uint64_t trig_ver;
    } sp;
    const uint8_t *flw_cur, *numw_cur;  /* active narrowing; NULL = all live   */
    /* Mixed lane/N=1 step in flight (#121): the lane half solved first; its
     * per-fluent next-state is injected into the residue solve as STRICT
     * facts on the primed locs (strict beats defeasible inertia, so residue
     * rules reading a lane fluent primed see the true next value), and the
     * commit takes lane-managed fluents from here instead of the family. */
    const uint8_t *mix_managed;         /* [nfl] 1 = fluent owned by the lane half */
    const bool    *mix_next;            /* [nfl] its lane-computed next value      */
    /* The ACTIVE step-family maps: how the solve/commit/why path finds the
     * current w->fam's locations. The dense path points these at loc_of /
     * fl_loc / pr_loc; a mixed split value points them at the sparse residue
     * space above. afl_list non-NULL = load facts for just those fluents. */
    const uint32_t *aloc_of; uint32_t aloc_cap;
    const uint32_t *afl_loc, *apr_loc;
    const uint32_t *afl_list; int nafl;
    /* Structure version (#63): bumped on any structural edit (rule/fluent add,
     * a matcher re-ground). Each family caches the version it was built at, so a
     * query rebuilds ONLY the judgment family and a step ONLY the step family —
     * a re-ground no longer forces the O(fluents) step-family (inertia) rebuild
     * onto the query path. fam_ver/jfam_ver == struct_ver means "up to date". */
    uint64_t struct_ver, fam_ver, jfam_ver;
    /* static_ver bumps only on a NON-matcher structural edit (a host rule/sup);
     * a matcher re-ground bumps struct_ver but not static_ver, so the jfam cache
     * can be reused incrementally when only the matched suffix changed (#68). */
    uint64_t static_ver, jfam_static_ver;
    bool jfam_solved;             /* jfam holds the current state's judgments     */
    uint32_t *loc_of;             /* intern atom -> schema atom, ~0u = absent    */
    uint32_t loc_cap, nloc;       /* loc_of size; # assigned schema locations    */
    uint32_t *loc_atom;           /* reverse: schema loc -> intern atom (#68 names)*/
    uint32_t loc_atom_cap;
    uint32_t *fl_loc, *pr_loc;    /* per fluent: schema ids of f and f'          */
    /* The jfam-only SPARSE location map (#77): only atoms the judgment rules
     * actually reference (heads/bodies, static + matched) plus lazily-
     * materialized query atoms — never the fluent sweep, primed twins, or step
     * atoms the dense map carries for the step family. Append-only like the
     * dense map (#67), so the cached jfam's columns keep their meaning across
     * re-grounds. Everything outside it answers through the exact lazy path in
     * query_jfam. */
    uint32_t *jloc_of;            /* intern atom -> jfam schema atom, ~0u absent */
    uint32_t jloc_cap, njloc;
    uint32_t *jloc_atom;          /* reverse: jfam schema atom -> intern atom    */
    uint32_t jloc_atom_cap;
    /* jfam incremental reuse (#68): the static-rule watermark in the cached jfam
     * dlcol, and how far its atom names have been emitted. */
    int jfam_wm_rules, jfam_wm_body, jfam_wm_sups; uint32_t jfam_named;

    lane_family *lanes;           /* per-sort N-lane families (DoD thesis)       */
    int nlanes, caplanes;
    lane_ref *lane_map;           /* ground atom -> (family, local, lane)        */
    uint32_t lane_map_cap;
    bool lanes_ok;                /* lane families reflect the current structure */

    step_lane_family *steplanes;  /* transition layer, bit-parallel (DoD thesis) */
    int nsteplanes, capsteplanes;

    /* When world_step is answered on the step lanes, w->fam does NOT hold the
     * transition — so world_step_why lazily re-solves it from this snapshot of
     * the pre-step state + actions (the analog of world_why staying on jfam). */
    bool last_routed;
    bool *step_snap; int step_snap_cap;
    uint32_t *last_actions; int last_nactions, last_actions_cap;
};

world *world_new(intern *syms)
{
    world *w = calloc(1, sizeof *w);
    arena_init(&w->a);
    arena_init(&w->matched_a);
    w->syms = syms;
    return w;
}

/* Does any matchable rule read `pred`? Conservative before registration. */
static bool matcher_watches(const world *w, uint32_t pred)
{
    if (!w->watch_set || pred == INTERN_NONE)
        return true;
    return pred < w->watch_cap && w->watch_of[pred] >= 0;
}

/* A base-fact edit: the judgment family and every lane family must re-solve. */
static void invalidate_state_solved(world *w)
{
    w->jfam_solved = false;
    w->matched_stale = true;           /* the matched layer must be re-ground (#45) */
    for (int i = 0; i < w->nlanes; i++)
        w->lanes[i].solved = false;
}

/* As above, for an edit whose predicate is known: the matched layer is staled
 * only if some matchable rule reads that predicate. The judgment family and the
 * lanes still re-solve unconditionally — they hold every rule, not just the
 * matchable ones. */
static void invalidate_state_solved_of(world *w, uint32_t pred)
{
    w->jfam_solved = false;
    if (matcher_watches(w, pred))
        w->matched_stale = true;
    for (int i = 0; i < w->nlanes; i++)
        w->lanes[i].solved = false;
}

void world_free(world *w)
{
    if (w->reground_free && w->reground_ctx) {     /* #59: compiler-owned matcher */
        w->reground_free(w->reground_ctx);
        w->reground_ctx = NULL;
        w->reground_fn = NULL;
    }
    if (w->fam0)
        dlcol_free(w->fam0);           /* w->fam only borrows (fam0 or a slot) */
    for (int v = 0; v < w->sp.nvals; v++) {
        if (w->sp.fams[v]) dlcol_free(w->sp.fams[v]);
        free(w->sp.flw[v]);
        free(w->sp.numw[v]);
    }
    free(w->sp.vatoms); free(w->sp.fams); free(w->sp.vers);
    free(w->sp.flw); free(w->sp.numw); free(w->sp.mixed);
    free(w->sp.smap); free(w->sp.satoms);
    free(w->sp.sfl_loc); free(w->sp.spr_loc); free(w->sp.sfl_list);
    free(w->neff_rules);
    free(w->sp.map_of); free(w->sp.trig_of);
    if (w->jfam)
        dlcol_free(w->jfam);
    for (int i = 0; i < w->nlanes; i++) {
        dlcol_free(w->lanes[i].fam);
        free(w->lanes[i].ground);
        free(w->lanes[i].is_fluent);
        free(w->lanes[i].is_import);
    }
    free(w->lanes);
    for (int i = 0; i < w->nsteplanes; i++) {
        dlcol_free(w->steplanes[i].fam);
        free(w->steplanes[i].ground);
        free(w->steplanes[i].kind);
        free(w->steplanes[i].fl_of);
        free(w->steplanes[i].act_of);
        free(w->steplanes[i].num_cell);
        free(w->steplanes[i].numeff);
        free(w->steplanes[i].bcast_of);
    }
    free(w->steplanes);
    free(w->step_snap);
    free(w->last_actions);
    free(w->lane_map);
    free(w->loc_of);
    free(w->loc_atom);
    free(w->jloc_of);
    free(w->jloc_atom);
    free(w->fl_loc);
    free(w->pr_loc);
    arena_release(&w->a);
    arena_release(&w->matched_a);
    if (w->fidx)
        factindex_free(w->fidx);
    free(w->fluents);
    free(w->vals);
    free(w->primed);
    free(w->fl_prov);
    free(w->fl_pred);
    free(w->fl_nargs);
    free(w->fl_args);
    free(w->fluent_of);
    free(w->emits);
    free(w->emit_primed);
    free(w->emit_of);
    free(w->emitbuf);
    free(w->watch_of);
    free(w->num_of);
    free(w->guard_of);
    free(w->prov_of);
    free(w->eguard_of);
    free(w->trig_of);
    free(w->views);
    free(w->vrows);
    free(w->vseen_of);
    free(w->vmark_of);
    free(w->vmat_of);
    free(w->jrules);
    free(w->excl_ents);
    free(w->excl_of);
    free(w->excl_grps);
    free(w->jsups);
    free(w->srules);
    free(w->nums);
    free(w->guards);
    free(w->pguards);
    free(w->pg_cur);
    free(w->num_stratum);
    free(w->eguards);
    free(w->provs);
    free(w->rollsites);
    if (w->rcpt) {
        for (int i = 0; i < w->caprcpt; i++)
            free(w->rcpt[i].items);
        free(w->rcpt);
    }
    free(w);
}

/* atom -> index map: geometric growth keeps declare amortized O(1) even when
 * atoms arrive in increasing id order (a per-call grow-to-atom+1 would be O(n^2)
 * of reallocs — the very trap this replaces). New slots init to -1. */
static void atom_map_set(int **map, uint32_t *cap, uint32_t key, int val)
{
    if (key >= *cap) {
        uint32_t nc = *cap ? *cap : 16;
        while (nc <= key) nc *= 2;
        *map = realloc(*map, (size_t)nc * sizeof **map);
        for (uint32_t k = *cap; k < nc; k++) (*map)[k] = -1;
        *cap = nc;
    }
    (*map)[key] = val;
}

static int fluent_index(const world *w, uint32_t atom)
{
    return atom < w->fluent_of_cap ? w->fluent_of[atom] : -1;
}

/* Register a predicate some matchable rule reads (#45). The first call flips
 * the world from watch-everything to watch-this-set, so the lang layer must
 * register EVERY predicate its matchable rules read — body atoms of any kind,
 * including negated ones and guards — before the first edit. */
void world_matcher_watch(world *w, uint32_t pred)
{
    if (pred == INTERN_NONE) return;
    atom_map_set(&w->watch_of, &w->watch_cap, pred, 1);
    w->watch_set = true;
}

void world_declare_fluent(world *w, uint32_t atom)
{
    if (fluent_index(w, atom) >= 0)
        return;
    int oldcap = w->capfl;
    GROW(w->fluents, w->nfl, w->capfl);
    if (w->capfl != oldcap) {
        w->vals = realloc(w->vals, (size_t)w->capfl * sizeof *w->vals);
        w->primed = realloc(w->primed, (size_t)w->capfl * sizeof *w->primed);
        w->fl_prov = realloc(w->fl_prov, (size_t)w->capfl * sizeof *w->fl_prov);
        w->fl_pred = realloc(w->fl_pred, (size_t)w->capfl * sizeof *w->fl_pred);
        w->fl_nargs = realloc(w->fl_nargs, (size_t)w->capfl * sizeof *w->fl_nargs);
        w->fl_args = realloc(w->fl_args,
                             (size_t)w->capfl * FACTINDEX_MAXARGS * sizeof *w->fl_args);
    }

    char buf[256];
    snprintf(buf, sizeof buf, "%s'", intern_name(w->syms, atom));
    w->fluents[w->nfl] = atom;
    w->vals[w->nfl] = false;
    w->primed[w->nfl] = intern_id(w->syms, buf);
    w->fl_prov[w->nfl] = NULL;
    w->fl_pred[w->nfl] = INTERN_NONE;          /* no structure until set (#28) */
    w->fl_nargs[w->nfl] = 0;
    atom_map_set(&w->fluent_of, &w->fluent_of_cap, atom, w->nfl);
    w->nfl++;
    w->struct_ver++;             /* structural edit (#63) */
    w->lanes_ok = false;          /* a structural edit stales the lane families */
}

static int emit_index(const world *w, uint32_t atom)
{
    return atom < w->emit_of_cap ? w->emit_of[atom] : -1;
}

/* Declare a burst cue (#11, §12). Idempotent — the grounder declares each
 * ground emit atom where it is USED (an effect head), so a cue over a sort
 * costs one atom per firing rule instance, never a cross product. */
void world_declare_emit(world *w, uint32_t atom)
{
    if (emit_index(w, atom) >= 0)
        return;
    int oldcap = w->capemit;
    GROW(w->emits, w->nemit, w->capemit);
    if (w->capemit != oldcap)
        w->emit_primed = realloc(w->emit_primed,
                                 (size_t)w->capemit * sizeof *w->emit_primed);
    char buf[256];
    snprintf(buf, sizeof buf, "%s'", intern_name(w->syms, atom));
    w->emits[w->nemit] = atom;
    w->emit_primed[w->nemit] = intern_id(w->syms, buf);
    atom_map_set(&w->emit_of, &w->emit_of_cap, atom, w->nemit);
    w->nemit++;
    w->struct_ver++;             /* structural edit (#63) */
    w->lanes_ok = false;
}

bool world_has_emit(const world *w, uint32_t atom)
{
    return emit_index(w, atom) >= 0;
}

const uint32_t *world_emits(const world *w, int *count)
{
    if (count) *count = w->nemitbuf;
    return w->emitbuf;
}

/* Where a fluent was declared (§6.3), for its generated inertia rules'
 * provenance. `at` is a "srcname:line" span; copied, NULL clears it. */
void world_set_fluent_prov(world *w, uint32_t atom, const char *at)
{
    int i = fluent_index(w, atom);
    if (i >= 0)
        w->fl_prov[i] = at ? arena_strdup(&w->a, at) : NULL;
}

/* Maintain the extension index for fluent `i` flipping to `now` (#73), so the
 * index is updated per fact change instead of rebuilt from all fluents per
 * re-ground. No-op until the index exists (built lazily from the initial state)
 * and only for structured base boolean fluents. */
static void fidx_update(world *w, int i, bool now)
{
    if (w->fidx && w->fl_pred[i] != INTERN_NONE) {
        const uint32_t *args = &w->fl_args[(size_t)i * FACTINDEX_MAXARGS];
        if (now) factindex_add(w->fidx, w->fl_pred[i], args, w->fl_nargs[i]);
        else     factindex_remove(w->fidx, w->fl_pred[i], args, w->fl_nargs[i]);
    }
}

/* Apply a step's fluent changes to the extension index (#73). Called with the
 * NEXT state, while w->vals still holds the current one, so it diffs and updates
 * only the changed fluents. O(fluents), but a step is O(fluents) anyway; the win
 * is that a query after the step finds the index current, no full rebuild. */
static void reindex_commit(world *w, const bool *next)
{
    if (!w->fidx) return;              /* not built yet — the first query builds it */
    for (int i = 0; i < w->nfl; i++)
        if (next[i] != w->vals[i])
            fidx_update(w, i, next[i]);
}

/* Lazily declare a schema-recognized fluent atom (#92): declare + attach its
 * (pred, args) structure in the same breath — a structureless declare would be
 * invisible to the fact index, and thus to the matcher, forever. Returns the
 * fluent index, or -1 when no hook is set / the atom isn't recognized. */
static int schema_declare(world *w, uint32_t atom)
{
    uint32_t pred, args[FACTINDEX_MAXARGS];
    int nargs;
    if (!w->schema_fn ||
        !w->schema_fn(w->schema_ctx, atom, &pred, args, &nargs))
        return -1;
    world_declare_fluent(w, atom);
    world_set_fluent_struct(w, atom, pred, args, nargs);
    return fluent_index(w, atom);
}

void world_set(world *w, uint32_t atom, bool value)
{
    int i = fluent_index(w, atom);
    if (i < 0)
        i = schema_declare(w, atom);   /* #92: first touch — true OR false (a
                                        * false assertion still stales judgments) */
    if (i >= 0) {
        if (w->vals[i] != value) fidx_update(w, i, value);   /* index tracks the change */
        w->vals[i] = value;
        /* pred-scoped: an edit to a fluent no matchable rule reads leaves the
         * match set provably unchanged, so it must not force a re-ground */
        invalidate_state_solved_of(w, w->fl_pred[i]);
    }
}

bool world_get(const world *w, uint32_t atom)
{
    int i = fluent_index(w, atom);
    return i >= 0 && w->vals[i];
}

/* Record a boolean fluent's (pred, arg-entities) so the extension index can be
 * rebuilt from w->vals (#28). No-op for an unknown atom or arity over the index
 * bound (such a fluent simply stays out of the index). */
void world_set_fluent_struct(world *w, uint32_t atom, uint32_t pred,
                             const uint32_t *args, int nargs)
{
    int i = fluent_index(w, atom);
    if (i < 0 || nargs < 0 || nargs > FACTINDEX_MAXARGS)
        return;
    w->fl_pred[i] = pred;
    w->fl_nargs[i] = nargs;
    for (int k = 0; k < nargs; k++)
        w->fl_args[(size_t)i * FACTINDEX_MAXARGS + k] = args[k];
    /* structure recorded at compile; the index is built lazily from it (#73) */
}

const struct factindex *world_fact_index(world *w)
{
    if (!w->fidx) {                        /* build once from the current state; */
        w->fidx = factindex_new();         /* thereafter maintained incrementally */
        for (int i = 0; i < w->nfl; i++)   /* (world_set / the step commit, #73)  */
            if (w->vals[i] && w->fl_pred[i] != INTERN_NONE)
                factindex_add(w->fidx, w->fl_pred[i],
                              &w->fl_args[(size_t)i * FACTINDEX_MAXARGS],
                              w->fl_nargs[i]);
    }
    return w->fidx;
}

/* ---- numeric value store & guards (§5.8, read side) ---------------- */

static int num_index(const world *w, uint32_t atom)
{
    return atom < w->num_of_cap ? w->num_of[atom] : -1;
}

void world_declare_num(world *w, uint32_t atom, long min, long max, bool has_range)
{
    if (num_index(w, atom) >= 0) return;
    GROW(w->nums, w->nnum, w->capnum);
    w->nums[w->nnum].atom = atom;
    w->nums[w->nnum].value = 0;
    w->nums[w->nnum].min = min;
    w->nums[w->nnum].max = max;
    w->nums[w->nnum].has_range = has_range;
    w->nums[w->nnum].lo_code = NULL; w->nums[w->nnum].n_lo = 0;
    w->nums[w->nnum].hi_code = NULL; w->nums[w->nnum].n_hi = 0;
    w->nums[w->nnum].merge = WORLD_MERGE_REGISTER;
    atom_map_set(&w->num_of, &w->num_of_cap, atom, w->nnum);
    w->nnum++;
}

void world_set_num_merge(world *w, uint32_t atom, world_merge m)
{
    int i = num_index(w, atom);
    if (i >= 0) w->nums[i].merge = m;
}

/* Attach dynamic clamp bounds to an already-declared numeric fluent (§5.8):
 * `int in 0 .. hp_max(X)` compiles the bound to effect-VM bytecode, evaluated
 * per-commit against the value store. NULL/0 leaves that side on its constant
 * min/max. Bytecode is arena-copied. Requires has_range (a declared range). */
void world_set_num_clamp(world *w, uint32_t atom,
                         const expr_ins *lo, int nlo,
                         const expr_ins *hi, int nhi)
{
    int i = num_index(w, atom);
    if (i < 0) return;
    if (lo && nlo > 0) {
        expr_ins *c = arena_alloc(&w->a, (size_t)nlo * sizeof *c);
        memcpy(c, lo, (size_t)nlo * sizeof *c);
        w->nums[i].lo_code = c; w->nums[i].n_lo = nlo;
    }
    if (hi && nhi > 0) {
        expr_ins *c = arena_alloc(&w->a, (size_t)nhi * sizeof *c);
        memcpy(c, hi, (size_t)nhi * sizeof *c);
        w->nums[i].hi_code = c; w->nums[i].n_hi = nhi;
    }
}

void world_set_num(world *w, uint32_t atom, long value)
{
    int i = num_index(w, atom);
    if (i >= 0) {
        w->nums[i].value = value;
        invalidate_state_solved(w);                /* guard truth may change */
    }
}

long world_get_num(const world *w, uint32_t atom)
{
    int i = num_index(w, atom);
    return i >= 0 ? w->nums[i].value : 0;
}

static int guard_index(const world *w, uint32_t atom)
{
    return atom < w->guard_of_cap ? w->guard_of[atom] : -1;
}

void world_add_guard(world *w, uint32_t guard, uint32_t num,
                     world_cmp op, long threshold)
{
    if (guard_index(w, guard) >= 0) return;  /* dedup: one atom per (fluent,op,thr) */
    atom_map_set(&w->guard_of, &w->guard_of_cap, guard, w->ng);
    GROW(w->guards, w->ng, w->capg);
    w->guards[w->ng].guard = guard;
    w->guards[w->ng].num = num;
    w->guards[w->ng].op = op;
    w->guards[w->ng].threshold = threshold;
    w->ng++;
}

void world_add_primed_guard(world *w, uint32_t guard, uint32_t num,
                            world_cmp op, long threshold)
{
    for (int i = 0; i < w->npg; i++)
        if (w->pguards[i].guard == guard) return;   /* dedup, like world_add_guard */
    GROW(w->pguards, w->npg, w->cappg);
    w->pguards[w->npg].guard = guard;
    w->pguards[w->npg].num = num;
    w->pguards[w->npg].op = op;
    w->pguards[w->npg].threshold = threshold;
    w->npg++;
    w->lanes_ok = false;              /* stratified worlds step N=1 (#87) */
}

void world_set_step_stratum(world *w, int rule, int stratum)
{
    if (rule >= 0 && rule < w->nsr && stratum >= 0)
        w->srules[rule].stratum = stratum;
}

#define LOC_NONE (~0u)   /* schema location for an atom absent from the family */

void world_set_provider_fn(world *w, world_provider_fn fn, void *ctx)
{
    w->provider_fn = fn;
    w->provider_ctx = ctx;
}

void world_set_fn_provider_fn(world *w, world_fn_provider_fn fn, void *ctx)
{
    w->fn_provider_fn = fn;
    w->fn_provider_ctx = ctx;
}

static int prov_index(const world *w, uint32_t atom)
{
    return atom < w->prov_of_cap ? w->prov_of[atom] : -1;
}

void world_declare_provider_atom(world *w, uint32_t atom, uint32_t pred,
                                 const uint32_t *args, int nargs)
{
    if (nargs > 4) nargs = 4;
    if (prov_index(w, atom) >= 0) return;    /* dedup: one entry per ground atom */
    atom_map_set(&w->prov_of, &w->prov_of_cap, atom, w->nprov);
    GROW(w->provs, w->nprov, w->capprov);
    w->provs[w->nprov].atom = atom;
    w->provs[w->nprov].pred = pred;
    w->provs[w->nprov].nargs = nargs;
    for (int k = 0; k < nargs; k++) w->provs[w->nprov].args[k] = args[k];
    w->nprov++;
}

/* Does provider atom i hold now? No callback -> closed-world false. */
static bool provider_holds(const world *w, int i)
{
    if (!w->provider_fn) return false;
    return w->provider_fn(w->provider_ctx, w->provs[i].pred,
                          w->provs[i].args, w->provs[i].nargs);
}

bool world_provider_holds_at(const world *w, uint32_t pred,
                             const uint32_t *args, int nargs)
{
    if (!w->provider_fn) return false;
    return w->provider_fn(w->provider_ctx, pred, args, nargs);
}

/* Load every ground provider atom as a closed-world fact from the callback —
 * mirrors the numeric-guard load; consulted fresh each solve (positions/state may
 * have changed), constant within the solve so the fixpoint's re-reads agree.
 * `of`/`cap` name the target family's location map (the shared dense map today;
 * the jfam-only sparse map once the judgment family stops sweeping fluents). */
static void load_providers(world *w, dlcol *f, const uint32_t *of, uint32_t cap)
{
    for (int i = 0; i < w->nprov; i++) {
        uint32_t pa = w->provs[i].atom;
        if (pa < cap && of[pa] != LOC_NONE)
            dlcol_add_fact(f, (dl_lit){ of[pa], !provider_holds(w, i) }, 0);
    }
}

void world_set_seed(world *w, uint64_t seed) { w->seed = seed; }
uint64_t world_tick(const world *w) { return w->tick; }

int world_add_roll_site(world *w, int sides, uint64_t site)
{
    GROW(w->rollsites, w->nrollsite, w->caprollsite);
    w->rollsites[w->nrollsite].sides = sides < 1 ? 1 : sides;
    w->rollsites[w->nrollsite].site = site;
    return w->nrollsite++;
}

/* splitmix64-style mix of (seed, tick, site) -> a die face 1..sides. Pure: the
 * same (seed, tick, site) always yields the same face (idempotent re-read). */
static long roll_value(const world *w, int idx)
{
    uint64_t x = w->seed ^ (w->rollsites[idx].site * 0x9E3779B97F4A7C15ull)
                         ^ (w->tick + 0x2545F4914F6CDD1Dull);
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    int sides = w->rollsites[idx].sides;
    return (long)(x % (uint64_t)sides) + 1;
}

static bool cmp_ok(long v, long t, world_cmp op)
{
    switch (op) {
    case WORLD_CMP_LE: return v <= t;
    case WORLD_CMP_LT: return v <  t;
    case WORLD_CMP_GE: return v >= t;
    case WORLD_CMP_GT: return v >  t;
    case WORLD_CMP_EQ: return v == t;
    }
    return false;
}

/* Does guard g hold for the current value of its numeric fluent? */
static bool guard_holds(const world *w, int g)
{
    return cmp_ok(world_get_num(w, w->guards[g].num), w->guards[g].threshold,
                  w->guards[g].op);
}

bool world_num_cmp_holds(const world *w, uint32_t num_atom,
                         world_cmp op, long threshold)
{
    return cmp_ok(world_get_num(w, num_atom), threshold, op);
}

static long eval_expr(const world *w, const expr_ins *code, int n, bool *undef);

static int eguard_index(const world *w, uint32_t atom)
{
    return atom < w->eguard_of_cap ? w->eguard_of[atom] : -1;
}

void world_add_expr_guard(world *w, uint32_t guard,
                          const expr_ins *lhs, int nlhs,
                          const expr_ins *rhs, int nrhs, world_cmp op)
{
    if (eguard_index(w, guard) >= 0) return; /* dedup: one entry per ground atom */
    atom_map_set(&w->eguard_of, &w->eguard_of_cap, guard, w->neguard);
    GROW(w->eguards, w->neguard, w->capeguard);
    expr_ins *l = arena_alloc(&w->a, (size_t)(nlhs ? nlhs : 1) * sizeof *l);
    expr_ins *r = arena_alloc(&w->a, (size_t)(nrhs ? nrhs : 1) * sizeof *r);
    if (nlhs) memcpy(l, lhs, (size_t)nlhs * sizeof *l);
    if (nrhs) memcpy(r, rhs, (size_t)nrhs * sizeof *r);
    w->eguards[w->neguard].guard = guard;
    w->eguards[w->neguard].lhs = l; w->eguards[w->neguard].nlhs = nlhs;
    w->eguards[w->neguard].rhs = r; w->eguards[w->neguard].nrhs = nrhs;
    w->eguards[w->neguard].op = op;
    bool ht = false;
    for (int k = 0; k < nlhs && !ht; k++) ht = l[k].op == EXPR_TEST;
    for (int k = 0; k < nrhs && !ht; k++) ht = r[k].op == EXPR_TEST;
    w->eguards[w->neguard].has_test = ht;
    w->eguards[w->neguard].tval = 0;
    if (ht) w->n_teg++;
    w->neguard++;
}

int world_new_excl_group(world *w, const char *label, const char *prov)
{
    GROW(w->excl_grps, w->negrp, w->capegrp);
    w->excl_grps[w->negrp].label = arena_strdup(&w->a, label ? label : "?");
    w->excl_grps[w->negrp].prov = prov ? arena_strdup(&w->a, prov) : NULL;
    return w->negrp++;
}

void world_add_excl_member(world *w, int group, uint32_t action_atom,
                           uint32_t key)
{
    GROW(w->excl_ents, w->nexcl, w->capexcl);
    w->excl_ents[w->nexcl].grp = group;
    w->excl_ents[w->nexcl].key = key;
    w->excl_ents[w->nexcl].atom = action_atom;
    int prev = action_atom < w->excl_of_cap ? w->excl_of[action_atom] : -1;
    w->excl_ents[w->nexcl].next = prev;
    atom_map_set(&w->excl_of, &w->excl_of_cap, action_atom, w->nexcl);
    w->nexcl++;
}

/* Does expression guard i hold? Evaluates both bytecode sides (roll/fluent/const)
 * and compares. Rolls read the current tick — idempotent within a solve.
 * Tri-valued (#116): 1 holds, 0 fails, -1 UNDEFINED (a side read a partial
 * value with no applicable definition — the comparison does not apply). */
static int guard_holds_expr(const world *w, int i)
{
    bool und = false;
    long l = eval_expr(w, w->eguards[i].lhs, w->eguards[i].nlhs, &und);
    long r = eval_expr(w, w->eguards[i].rhs, w->eguards[i].nrhs, &und);
    if (und) return -1;
    return cmp_ok(l, r, w->eguards[i].op) ? 1 : 0;
}

/* Load every ground expression guard as a closed-world fact (like numeric
 * guards). A test-bearing guard (#86) has no value in pass A — its atom stays
 * unasserted, so nothing gated on it fires yet; in pass C (`teg_ready`) it
 * loads from `tval`, evaluated between the solves against the settled pass-A
 * result. Roll-bearing guards re-evaluate identically in both passes (§5.10
 * keyed lookup — same tick, same draw). */
static void load_eguards(world *w, dlcol *f, const uint32_t *of, uint32_t cap)
{
    for (int i = 0; i < w->neguard; i++) {
        uint32_t ga = w->eguards[i].guard;
        if (ga >= cap || of[ga] == LOC_NONE) continue;
        int v;
        if (w->eguards[i].has_test) {
            if (!w->teg_ready) continue;           /* pass A: undecided */
            v = w->eguards[i].tval;
        } else {
            v = guard_holds_expr(w, i);
        }
        if (v < 0) {           /* #116 partial value undefined: NEITHER fact —
                                * marked OPEN so the guard atom (a located,
                                * rule-less literal that would otherwise close
                                * to REFUTED) stays honestly UNDECIDED */
            dlcol_set_open(f, (dl_lit){ of[ga], false }, 0);
            dlcol_set_open(f, (dl_lit){ of[ga], true }, 0);
            continue;
        }
        dlcol_add_fact(f, (dl_lit){ of[ga], !v }, 0);
    }
}

/* Pass B (#86): evaluate the test-bearing guards against the just-settled
 * family. `tf`/`of`/`cap` become the EXPR_TEST evaluation context, so a
 * tested judgment reads THIS solve's verdicts, whichever family it is. */
static void eval_test_guards(world *w, dlcol *tf, const uint32_t *of, uint32_t cap)
{
    w->tctx_fam = tf; w->tctx_of = of; w->tctx_cap = cap;
    for (int i = 0; i < w->neguard; i++)
        if (w->eguards[i].has_test)
            w->eguards[i].tval = (int8_t)guard_holds_expr(w, i);
    w->tctx_fam = NULL;
    w->teg_ready = true;
}

/* Load every ground numeric-comparison guard the same way. */
static void load_guards(world *w, dlcol *f, const uint32_t *of, uint32_t cap)
{
    for (int g = 0; g < w->ng; g++) {
        uint32_t ga = w->guards[g].guard;
        if (ga < cap && of[ga] != LOC_NONE)
            dlcol_add_fact(f, (dl_lit){ of[ga], !guard_holds(w, g) }, 0);
    }
}

int world_add_rule(world *w, const char *name, dl_rule_kind kind,
                   dl_lit head, const dl_lit *body, int nbody)
{
    arena *ra = w->in_matched ? &w->matched_a : &w->a;   /* matched rules: own arena (#48) */
    GROW(w->jrules, w->njr, w->capjr);
    jrule *r = &w->jrules[w->njr];
    r->name = arena_strdup(ra, name);
    r->prov = NULL;
    r->kind = kind;
    r->head = head;
    r->nbody = nbody;
    r->body = arena_alloc(ra, (size_t)(nbody ? nbody : 1) * sizeof(dl_lit));
    if (nbody)
        memcpy(r->body, body, (size_t)nbody * sizeof(dl_lit));
    w->struct_ver++;             /* structural edit (#63) */
    if (!w->in_matched) w->static_ver++;   /* a STATIC judgment rule changed (#68) */
    w->lanes_ok = false;          /* a structural edit stales the lane families */
    return w->njr++;
}

/* #109 cycle rule (§5.2): see world.h. Nodes are literal indices
 * (atom*2 + neg); support edges body -> head over strict/defeasible jrules;
 * iterative Tarjan; the first cyclic SCC any rule (defeaters included)
 * concludes a complement into is reported with its loop and attacker. */
bool world_attacked_cycle(world *w, char *err, size_t errsz, const char **prov)
{
    if (prov) *prov = NULL;
    if (w->njr == 0) return false;
    uint32_t maxa = 0;
    for (int r = 0; r < w->njr; r++) {
        const jrule *jr = &w->jrules[r];
        if (jr->head.atom > maxa) maxa = jr->head.atom;
        for (int b = 0; b < jr->nbody; b++)
            if (jr->body[b].atom > maxa) maxa = jr->body[b].atom;
    }
    int n = ((int)maxa + 1) * 2;
#define LIDX(l) ((int)(l).atom * 2 + ((l).neg ? 1 : 0))
    int nedge = 0;
    for (int r = 0; r < w->njr; r++)
        if (w->jrules[r].kind != DL_DEFEATER)
            nedge += w->jrules[r].nbody;
    int32_t *aoff = calloc((size_t)n + 2, sizeof *aoff);
    int32_t *ato = malloc((size_t)(nedge ? nedge : 1) * sizeof *ato);
    for (int r = 0; r < w->njr; r++) {
        if (w->jrules[r].kind == DL_DEFEATER) continue;
        for (int b = 0; b < w->jrules[r].nbody; b++)
            aoff[LIDX(w->jrules[r].body[b]) + 1]++;
    }
    for (int i = 0; i < n; i++) aoff[i + 1] += aoff[i];
    int32_t *fill = malloc((size_t)n * sizeof *fill);
    memcpy(fill, aoff, (size_t)n * sizeof *fill);
    for (int r = 0; r < w->njr; r++) {
        if (w->jrules[r].kind == DL_DEFEATER) continue;
        for (int b = 0; b < w->jrules[r].nbody; b++)
            ato[fill[LIDX(w->jrules[r].body[b])]++] = LIDX(w->jrules[r].head);
    }
    free(fill);

    int32_t *low = malloc((size_t)n * sizeof *low);
    int32_t *idx = malloc((size_t)n * sizeof *idx);
    int32_t *stk = malloc((size_t)n * sizeof *stk);
    bool    *onstk = calloc((size_t)n, 1);
    int32_t *fv = malloc((size_t)n * sizeof *fv);
    int32_t *fe = malloc((size_t)n * sizeof *fe);
    int32_t *scc = malloc((size_t)n * sizeof *scc);
    for (int i = 0; i < n; i++) idx[i] = -1;
    int counter = 0, sp = 0, nscc = 0;
    for (int root = 0; root < n; root++) {
        if (idx[root] >= 0) continue;
        int fp = 0;
        fv[fp] = root; fe[fp] = aoff[root];
        idx[root] = low[root] = counter++;
        stk[sp++] = root; onstk[root] = true;
        while (fp >= 0) {
            int v = fv[fp];
            if (fe[fp] < aoff[v + 1]) {
                int x = ato[fe[fp]++];
                if (idx[x] < 0) {
                    idx[x] = low[x] = counter++;
                    stk[sp++] = x; onstk[x] = true;
                    fp++;
                    fv[fp] = x; fe[fp] = aoff[x];
                } else if (onstk[x] && idx[x] < low[v]) {
                    low[v] = idx[x];
                }
            } else {
                if (low[v] == idx[v]) {
                    int m;
                    do { m = stk[--sp]; onstk[m] = false; scc[m] = nscc; }
                    while (m != v);
                    nscc++;
                }
                fp--;
                if (fp >= 0 && low[v] < low[fv[fp]]) low[fv[fp]] = low[v];
            }
        }
    }
    free(low); free(idx); free(stk); free(onstk); free(fv); free(fe);

    int *sz = calloc((size_t)(nscc ? nscc : 1), sizeof *sz);
    bool *cyc = calloc((size_t)(nscc ? nscc : 1), 1);
    for (int i = 0; i < n; i++) sz[scc[i]]++;
    for (int c = 0; c < nscc; c++) if (sz[c] > 1) cyc[c] = true;
    for (int i = 0; i < n; i++)
        for (int k = aoff[i]; k < aoff[i + 1]; k++)
            if (ato[k] == i) cyc[scc[i]] = true;

    bool found = false;
    for (int r = 0; r < w->njr && !found; r++) {
        int t = LIDX(w->jrules[r].head) ^ 1;   /* the literal r attacks */
        if (t >= n || !cyc[scc[t]]) continue;
        found = true;
        char loop[192];
        int off = 0, shown = 0;
        for (int i = 0; i < n && shown < 4; i++) {
            if (scc[i] != scc[t]) continue;
            off += snprintf(loop + off, sizeof loop - (size_t)off, "%s%s%s",
                            shown ? " <- " : "", (i & 1) ? "~" : "",
                            intern_name(w->syms, (uint32_t)(i >> 1)));
            if (off >= (int)sizeof loop) { off = (int)sizeof loop - 1; break; }
            shown++;
        }
        if (err)
            snprintf(err, errsz,
                     "the rules concluding '%s%s' form a support cycle (%s%s) "
                     "and rule '%s' attacks it — defeat cannot reach through "
                     "a cycle (§5.2); break the loop or remove the attack",
                     (t & 1) ? "~" : "", intern_name(w->syms, (uint32_t)(t >> 1)),
                     loop, sz[scc[t]] > 4 ? " <- ..." : "",
                     w->jrules[r].name);
        if (prov) *prov = w->jrules[r].prov;
    }
    free(sz); free(cyc); free(scc); free(aoff); free(ato);
#undef LIDX
    return found;
}

void world_add_sup(world *w, int winner, int loser)
{
    GROW(w->jsups, w->njs, w->capjs);
    w->jsups[w->njs].winner = winner;
    w->jsups[w->njs].loser = loser;
    w->njs++;
    w->struct_ver++;             /* structural edit (#63) */
    if (!w->in_matched) w->static_ver++;   /* matched rules carry no sup (#68) */
    w->lanes_ok = false;          /* a structural edit stales the lane families */
}

/* Tick-time matcher watermark (#28): the judgment rules added after a checkpoint
 * are re-materialized matched rules, dropped wholesale before the next re-ground.
 * Only the RULE array is watermarked, never the superiority array: matched-kernel
 * rules carry no `>` (the rule_in_sup gate in rule_matchable), so njs is invariant
 * across a re-ground and the static superiority relation must stay intact — the
 * checkpoint can therefore precede ground_sup safely. The checkpoint also flips
 * `in_matched`, routing every subsequent rule's arena-copied name/body/prov into
 * matched_a, which world_matched_reset frees — so a re-ground per tick is bounded
 * in memory rather than leaking into w->a (#48). */
void world_matched_checkpoint(world *w)
{
    w->jr_matched_base = w->njr;
    w->in_matched = true;
}

void world_matched_reset(world *w)
{
    if (w->njr == w->jr_matched_base)
        return;                  /* empty suffix: dropping nothing is not an edit —
                                  * an all-view re-ground (#80) must not force a
                                  * jfam re-emit every tick via struct_ver */
    w->njr = w->jr_matched_base;
    arena_release(&w->matched_a);   /* free the previous matched layer's strings/bodies */
    w->struct_ver++;             /* structural edit (#63) */
    w->lanes_ok = false;
}

void world_set_reground_fn(world *w, world_reground_fn fn, void *ctx)
{
    w->reground_fn = fn;
    w->reground_ctx = ctx;
    w->matched_stale = true;        /* the first solve re-grounds the initial layer */
}

void world_own_reground_ctx(world *w, void (*free_fn)(void *))
{
    w->reground_free = free_fn;
}

/* ---- matched views (#80): island judgments as sets, not rules ---- */

int world_view_new(world *w, uint32_t head_pred, bool head_neg, dl_rule_kind kind)
{
    GROW(w->views, w->nviews, w->capviews);
    w->views[w->nviews].head_pred = head_pred;
    w->views[w->nviews].head_neg = head_neg;
    w->views[w->nviews].kind = kind;
    return w->nviews++;
}

void world_views_reset(world *w)
{
    w->nvrow = 0;                   /* drop rows, keep allocations (#48) */
    w->vseq++;                      /* everything previously present is now absent */
}

void world_view_add(world *w, int view, uint32_t atom,
                    const uint32_t *bind, int nvars)
{
    if (nvars > WORLD_VIEW_MAXBIND) {          /* loud, never silently truncated */
        fprintf(stderr, "world_view_add: %d binds exceeds WORLD_VIEW_MAXBIND\n",
                nvars);
        abort();
    }
    GROW(w->vrows, w->nvrow, w->capvrow);
    struct vrow *r = &w->vrows[w->nvrow++];
    r->atom = atom;
    r->view = view;
    r->nvars = nvars;
    for (int i = 0; i < nvars; i++) r->bind[i] = bind[i];
    atom_map_set(&w->vseen_of, &w->vseen_cap, atom, view);   /* append-only */
    atom_map_set(&w->vmark_of, &w->vmark_cap, atom, (int)w->vseq);
}

void world_set_materialize_fn(world *w, world_materialize_fn fn, void *ctx)
{
    w->materialize_fn = fn;
    w->materialize_ctx = ctx;
}

void world_set_schema_fn(world *w, world_schema_fn fn, void *ctx)
{
    w->schema_fn = fn;
    w->schema_ctx = ctx;
}

int world_fluent_count(const world *w) { return w->nfl; }

bool world_has_fluent(const world *w, uint32_t atom)
{
    return fluent_index(w, atom) >= 0;
}

/* Pure schema probe (#92): recognized-but-undeclared fluent atom? No declare,
 * no mutation — the query path's closed-world fallback. */
static bool schema_knows(const world *w, uint32_t atom)
{
    uint32_t pred, args[FACTINDEX_MAXARGS];
    int nargs;
    return w->schema_fn &&
           w->schema_fn(w->schema_ctx, atom, &pred, args, &nargs);
}

int world_view_row_count(const world *w) { return w->nvrow; }

size_t world_view_bytes(const world *w)
{
    return (size_t)w->capviews * sizeof *w->views
         + (size_t)w->capvrow * sizeof *w->vrows
         + (size_t)w->vseen_cap * sizeof *w->vseen_of
         + (size_t)w->vmark_cap * sizeof *w->vmark_of
         + (size_t)w->vmat_cap * sizeof *w->vmat_of;
}

/* Is `atom` view-owned, and is it present this tick? -1 = not view-owned. */
static int view_of_atom(const world *w, uint32_t atom, bool *present)
{
    if (atom >= w->vseen_cap || w->vseen_of[atom] < 0)
        return -1;
    *present = atom < w->vmark_cap && w->vmark_of[atom] == (int)w->vseq;
    return w->vseen_of[atom];
}

int world_add_step_rule(world *w, const char *name, uint32_t action,
                        const step_cond *body, int nbody,
                        const dl_lit *effects, int neffects)
{
    GROW(w->srules, w->nsr, w->capsr);
    srule *r = &w->srules[w->nsr];
    r->name = arena_strdup(&w->a, name);
    r->prov = NULL;
    r->action = action;
    if (action != INTERN_NONE)     /* the loud-no-op trigger set (#119) */
        atom_map_set(&w->trig_of, &w->trig_of_cap, action, 1);
    r->nbody = nbody;
    r->body = arena_alloc(&w->a, (size_t)(nbody ? nbody : 1) * sizeof(step_cond));
    if (nbody)
        memcpy(r->body, body, (size_t)nbody * sizeof(step_cond));
    r->neffects = neffects;
    r->effects = arena_alloc(&w->a, (size_t)(neffects ? neffects : 1) * sizeof(dl_lit));
    if (neffects)
        memcpy(r->effects, effects, (size_t)neffects * sizeof(dl_lit));
    r->neffs = NULL;
    r->nneff = r->capneff = 0;
    r->stratum = 0;
    r->lane_cover = 0;
    w->struct_ver++;             /* structural edit (#63) */
    w->lanes_ok = false;          /* a structural edit stales the lane families */
    return w->nsr++;
}

int world_set_split(world *w, const uint32_t *value_atoms, int nvals)
{
    if (w->sp.nvals)
        return -1;                     /* one split fluent per world (#121) */
    if (nvals < 2 || nvals > 31)
        return -1;                     /* finite, and the liveness mask is 31 bits
                                        * — generous, and loud rather than silent */
    for (int v = 0; v < nvals; v++)
        if (fluent_index(w, value_atoms[v]) < 0)
            return -1;                 /* every value atom must be a declared fluent */
    w->sp.vatoms = malloc((size_t)nvals * sizeof *w->sp.vatoms);
    memcpy(w->sp.vatoms, value_atoms, (size_t)nvals * sizeof *w->sp.vatoms);
    w->sp.nvals = nvals;
    w->sp.fams = calloc((size_t)nvals, sizeof *w->sp.fams);
    w->sp.vers = calloc((size_t)nvals, sizeof *w->sp.vers);
    w->sp.flw  = calloc((size_t)nvals, sizeof *w->sp.flw);
    w->sp.numw = calloc((size_t)nvals, sizeof *w->sp.numw);
    w->sp.mixed = calloc((size_t)nvals, 1);
    for (int v = 0; v < nvals; v++)
        atom_map_set(&w->sp.map_of, &w->sp.map_cap, value_atoms[v], v);
    w->sp.trig_ver = w->struct_ver - 1;    /* masks not built yet */
    return 0;
}

static int srule_split_value(const world *w, const srule *r);

/* Per-action live-value bitmasks for the split loudness check: bit v set iff
 * some step rule with this trigger is live under value v. Rebuilt lazily on a
 * structural edit; absent actions read -1 (the #119 check already screens
 * triggers that match nothing anywhere). */
static void ensure_split_trig_masks(world *w)
{
    if (w->sp.trig_ver == w->struct_ver)
        return;
    for (uint32_t k = 0; k < w->sp.trig_cap; k++)
        w->sp.trig_of[k] = -1;
    int all = (int)((1u << w->sp.nvals) - 1);
    for (int s = 0; s < w->nsr; s++) {
        const srule *r = &w->srules[s];
        if (r->action == INTERN_NONE)
            continue;
        int g = srule_split_value(w, r);
        int mask = g == -2 ? all : g >= 0 ? (1 << g) : 0;
        int prev = r->action < w->sp.trig_cap ? w->sp.trig_of[r->action] : -1;
        atom_map_set(&w->sp.trig_of, &w->sp.trig_cap, r->action,
                     (prev < 0 ? 0 : prev) | mask);
    }
    w->sp.trig_ver = w->struct_ver;
}

void world_set_rule_prov(world *w, int rule, const char *prov)
{
    arena *ra = w->in_matched ? &w->matched_a : &w->a;   /* match world_add_rule (#48) */
    if (rule >= 0 && rule < w->njr)
        w->jrules[rule].prov = prov ? arena_strdup(ra, prov) : NULL;
}

void world_set_step_prov(world *w, int rule, const char *prov)
{
    if (rule >= 0 && rule < w->nsr) {
        w->srules[rule].prov = prov ? arena_strdup(&w->a, prov) : NULL;
        w->struct_ver++;             /* structural edit (#63) */
    w->lanes_ok = false;          /* a structural edit stales the lane families */
    }
}

void world_add_num_effect(world *w, int rule, uint32_t num_atom,
                          world_numop op, const expr_ins *code, int ncode)
{
    srule *r = &w->srules[rule];
    /* grow the effect list; it lives in the arena, so double by re-copying */
    if (r->nneff == r->capneff) {
        int nc = r->capneff ? r->capneff * 2 : 4;
        num_effect *ne = arena_alloc(&w->a, (size_t)nc * sizeof *ne);
        if (r->nneff)
            memcpy(ne, r->neffs, (size_t)r->nneff * sizeof *ne);
        r->neffs = ne;
        r->capneff = nc;
    }
    expr_ins *owncode = arena_alloc(&w->a, (size_t)(ncode ? ncode : 1) * sizeof *owncode);
    if (ncode)
        memcpy(owncode, code, (size_t)ncode * sizeof *owncode);
    num_effect *e = &r->neffs[r->nneff++];
    e->num_atom = num_atom;
    e->op = op;
    e->code = owncode;
    e->ncode = ncode;
    /* numeric effects run in the commit phase, not the fixpoint — the cached
     * boolean family is unaffected, so no fam_dirty here. */
}

/* Both families (step + judgment) are built from one location map; the query
 * layer runs on the columnar engine too (DESIGN.md §6.3 — one production
 * engine, the scalar dl kept only as test_col's differential oracle). */
static void ensure_jfam(world *w);
static void ensure_fam(world *w);
static void solve_judgment_family(world *w);
static void solve_lane_iter(world *w, lane_family *lf, int it);
static void solve_step_family(world *w, const uint32_t *actions, int nactions);
static void solve_step_family_vals(world *w, const bool *vals,
                                   const uint32_t *actions, int nactions);
static uint32_t jassign_loc(world *w, uint32_t atom);
static void emit_atom_names_range(dlcol *f, const world *w, const uint32_t *atomv,
                                  uint32_t from, uint32_t to);

/* Exact verdict for an atom OUTSIDE the sparse judgment family (#77): no
 * judgment rule concludes or attacks it, so its verdict is a pure function of
 * its own fact — precisely what the dense family's fact load concluded for it.
 * Row order matters and mirrors the dense behavior class by class:
 *   - a declared fluent answers closed-world from the value store (the world.h
 *     contract; the dense family loaded f or ~f for every declared fluent);
 *   - otherwise, an atom the dense map never located is not in the theory at
 *     all (never-declared, or a registered-but-unreferenced guard/provider —
 *     the landmark carve-out) -> UNDECIDED, exactly as before;
 *   - a guard/provider/expr-guard atom the dense map located (e.g. referenced
 *     only by a step-rule body) was loaded as a closed-world fact -> its
 *     holds() verdict;
 *   - anything else the dense map located (primed, action, step-only atoms)
 *     had a location but no rules and no fact: refuted, both polarities.
 * Pure: mutates nothing (world_why materializes instead, so traces render). */
static dl_verdict lazy_judgment_verdict(const world *w, dl_lit q)
{
    int i = fluent_index(w, q.atom);
    if (i >= 0)
        return q.neg != w->vals[i] ? DL_PROVED : DL_REFUTED;
    if (schema_knows(w, q.atom))                   /* #92: never-touched fluent —
                                                    * closed-world false, and pure
                                                    * (world_why declares instead) */
        return q.neg ? DL_PROVED : DL_REFUTED;
    if (q.atom >= w->loc_cap || w->loc_of[q.atom] == LOC_NONE)
        return DL_UNDECIDED;                       /* absent: unmentioned atom */
    int g = guard_index(w, q.atom);
    if (g >= 0)
        return q.neg != guard_holds(w, g) ? DL_PROVED : DL_REFUTED;
    int p = prov_index(w, q.atom);
    if (p >= 0)
        return q.neg != provider_holds(w, p) ? DL_PROVED : DL_REFUTED;
    int e = eguard_index(w, q.atom);
    if (e >= 0) {
        int v = guard_holds_expr(w, e);
        if (v < 0) return DL_UNDECIDED;    /* #116: partial value undefined —
                                            * the comparison does not apply */
        return q.neg != (v != 0) ? DL_PROVED : DL_REFUTED;
    }
    return DL_REFUTED;         /* located, rule-less, fact-less: -d both ways */
}

/* The N=1 judgment path: the proven route, and the oracle world_lanes_check
 * measures the lane families against. */
static dl_verdict query_jfam(world *w, dl_lit q)
{
    ensure_jfam(w);                /* re-ground fills the views before we look */
    /* Matched views (#80), checked BEFORE the solve and the jloc branch: a
     * view atom's verdict is pure membership — an all-island query touches no
     * dlcol at all — and a why-materialized view atom (which then has a jloc
     * and transient rules) must keep answering from the view, not the family. */
    bool present;
    int vi = view_of_atom(w, q.atom, &present);
    if (vi >= 0) {
        if (present)
            return q.neg == w->views[vi].head_neg ? DL_PROVED : DL_REFUTED;
        return DL_REFUTED;         /* dropped match: located-rule-less analog */
    }
    if (!w->jfam_solved)
        solve_judgment_family(w);
    if (q.atom < w->jloc_cap && w->jloc_of[q.atom] != LOC_NONE) {
        dl_lit loc = { w->jloc_of[q.atom], q.neg };
        return dlcol_defeasible(w->jfam, loc, 0);
    }
    return lazy_judgment_verdict(w, q);
}

dl_verdict world_query(world *w, dl_lit q)
{
    ensure_jfam(w);
    /* the hot path: if this atom is a lane cell, answer from the bit-parallel
     * family (all lanes solved at once) instead of the N=1 judgment family. For
     * a join family the cell names an iteration too; solve that iteration's fact
     * slice on demand and cache it (cur_it), so a run of queries into the same
     * slice pays one solve — the per-iteration adopt of the join matcher. */
    if (w->lanes_ok && q.atom < w->lane_map_cap && w->lane_map[q.atom].fam >= 0) {
        lane_ref r = w->lane_map[q.atom];
        lane_family *lf = &w->lanes[r.fam];
        if (!lf->solved || lf->cur_it != r.it) {
            solve_lane_iter(w, lf, r.it);
            lf->cur_it = r.it;
            lf->solved = true;
        }
        dl_lit la = { (uint32_t)r.a, q.neg };
        return dlcol_defeasible(lf->fam, la, r.e);
    }
    return query_jfam(w, q);
}

void world_why(world *w, dl_lit q, FILE *out)
{
    /* Never-touched schema fluent (#92): DECLARE it — a jloc alone is not
     * enough, because the fact loader keys on fluent_index, and the dense
     * world's trace for the negative polarity includes the base-fact line.
     * Declaring first makes every gate below see an ordinary declared-false
     * fluent, so the trace is byte-identical by construction. The struct_ver
     * bump is fine on this slow path: matched_stale is untouched (no
     * re-ground) and the jfam re-emit takes the #68 incremental branch. */
    if (fluent_index(w, q.atom) < 0)
        (void)schema_declare(w, q.atom);
    ensure_jfam(w);
    /* Matched view atoms (#80) have no rules in the family; re-emit just this
     * atom's instances as ordinary matched rules through the registered hook,
     * so the one shared renderer produces the same trace full emission would
     * (world_add_rule bumps struct_ver, so the ensure below re-emits the
     * suffix; the next re-ground truncates these transients). NOT gated on the
     * jloc — a view atom keeps its (append-only) jloc from an earlier
     * materialization long after those rules were truncated; the vmat stamp is
     * what says "this generation's rules are in the family", making repeated
     * whys idempotent. A dropped view atom emits nothing and falls through to
     * the lazy jloc materialization — the located-rule-less two-line REFUTED
     * trace, exactly as a dropped matched rule rendered before views. */
    {
        bool present;
        if (view_of_atom(w, q.atom, &present) >= 0
            && present && w->materialize_fn
            && !(q.atom < w->vmat_cap && w->vmat_of[q.atom] == (int)w->vseq)) {
            for (int i = 0; i < w->nvrow; i++)
                if (w->vrows[i].atom == q.atom)
                    w->materialize_fn(w->materialize_ctx, w, q.atom,
                                      w->vrows[i].view, w->vrows[i].bind,
                                      w->vrows[i].nvars);
            atom_map_set(&w->vmat_of, &w->vmat_cap, q.atom, (int)w->vseq);
            ensure_jfam(w);
        }
    }
    if (q.atom >= w->jloc_cap || w->jloc_of[q.atom] == LOC_NONE) {
        /* outside the sparse family. If the dense map never saw it either (and
         * it is no fluent and no ever-seen view atom), it is not in the
         * theory — same message as always. An ever-seen view atom instead
         * takes the materialization below: a location with no rules renders
         * the two-line REFUTED trace a dropped matched rule always had. */
        bool vpresent;
        if ((q.atom >= w->loc_cap || w->loc_of[q.atom] == LOC_NONE)
            && fluent_index(w, q.atom) < 0
            && view_of_atom(w, q.atom, &vpresent) < 0) {
            fprintf(out, "why %s%s?\n  (not in the theory — no rule or fact)\n",
                    q.neg ? "~" : "", intern_name(w->syms, q.atom));
            return;
        }
        /* Lazily materialize it into the jfam so the trace renders: append a
         * location (append-only, so the cached family stays valid), grow the
         * columns, name the tail, re-solve. The next solve's fact load picks
         * the atom up (fluent / guard / provider) via the jloc gate, so the
         * trace is byte-identical to the dense family's. NOT a structural
         * edit: struct_ver stays put (bumping it would force a re-ground). */
        jassign_loc(w, q.atom);
        dlcol_ensure_atoms(w->jfam, (int)w->njloc);
        emit_atom_names_range(w->jfam, w, w->jloc_atom, w->jfam_named, w->njloc);
        w->jfam_named = w->njloc;
        w->jfam_solved = false;
    }
    if (!w->jfam_solved)
        solve_judgment_family(w);
    dl_lit loc = { w->jloc_of[q.atom], q.neg };
    dlcol_why(w->jfam, loc, 0, out);
}

/* ---- lane families (DoD thesis) ---- */

void world_add_lane_family(world *w, dlcol *fam, int natoms, int nent, int niter,
                           const uint32_t *ground, const bool *is_fluent,
                           const bool *is_import)
{
    GROW(w->lanes, w->nlanes, w->caplanes);
    int fi = w->nlanes;
    lane_family *lf = &w->lanes[w->nlanes++];
    lf->fam = fam;
    lf->natoms = natoms;
    lf->nent = nent;
    lf->niter = niter;
    lf->solved = false;
    lf->cur_it = 0;
    size_t g = (size_t)natoms * (size_t)niter * (size_t)nent;
    lf->ground = malloc((g ? g : 1) * sizeof *lf->ground);
    memcpy(lf->ground, ground, (g ? g : 1) * sizeof *lf->ground);
    lf->is_fluent = malloc((size_t)(natoms ? natoms : 1) * sizeof *lf->is_fluent);
    memcpy(lf->is_fluent, is_fluent, (size_t)(natoms ? natoms : 1) * sizeof *lf->is_fluent);
    lf->is_import = calloc((size_t)(natoms ? natoms : 1), sizeof *lf->is_import);
    if (is_import)
        memcpy(lf->is_import, is_import, (size_t)(natoms ? natoms : 1) * sizeof *lf->is_import);

    /* Index each ground atom back to its lane cell so world_query can route —
     * for join families (niter>1) as well as single-variable ones. A join
     * family repeats role-0-only atoms across iterations and role-1-only atoms
     * across lanes, so a ground atom can name more than one cell; such an atom
     * has no single verdict to route to and stays on the N=1 path. Only atoms
     * UNIQUE within the family are routable — which, for a join, are exactly the
     * ones mentioning both variables (the relational head and binary bodies).
     * Single-variable families (niter==1) are unique by construction. First
     * family to claim an atom keeps it; a later family's cell for the same atom
     * is redundant (both validated against the same N=1 verdict). */
    size_t ncells = (size_t)natoms * (size_t)niter * (size_t)nent;
    uint32_t maxat = 0;
    for (size_t k = 0; k < ncells; k++)
        if (ground[k] > maxat) maxat = ground[k];
    uint8_t *mult = calloc((size_t)maxat + 1, 1);   /* 0 unseen, 1 once, 2 = dup */
    for (size_t k = 0; k < ncells; k++)
        if (mult[ground[k]] < 2) mult[ground[k]]++;

    for (int a = 0; a < natoms; a++) {
        if (is_import && is_import[a])
            continue;                          /* not concluded here: never route */
        for (int it = 0; it < niter; it++)
            for (int e = 0; e < nent; e++) {
                uint32_t at = ground[((size_t)a * niter + it) * nent + e];
                if (mult[at] != 1) continue;   /* ambiguous within family -> jfam */
                if (at >= w->lane_map_cap) {
                    uint32_t nc = at + 1;
                    w->lane_map = realloc(w->lane_map, (size_t)nc * sizeof *w->lane_map);
                    for (uint32_t k = w->lane_map_cap; k < nc; k++) w->lane_map[k].fam = -1;
                    w->lane_map_cap = nc;
                }
                if (w->lane_map[at].fam < 0)         /* unclaimed: first wins */
                    w->lane_map[at] = (lane_ref){ fi, a, e, it };
            }
    }
    free(mult);
    w->lanes_ok = true;
}

int world_lane_family_count(const world *w) { return w->nlanes; }

int world_judgment_rule_count(const world *w) { return w->njr; }
int world_step_rule_count(const world *w) { return w->nsr; }

size_t world_arena_bytes(const world *w)
{
    return arena_bytes(&w->a) + arena_bytes(&w->matched_a);
}

uint32_t world_atom_loc(const world *w, uint32_t atom)
{
    return atom < w->loc_cap ? w->loc_of[atom] : LOC_NONE;
}

/* Load one iteration's fact slice into a lane family and solve it (all lanes at
 * once). For niter==1 (single-variable) `it` is 0 and the solve is the whole
 * family; for a join it is the current non-lane entity. */
static void solve_lane_iter(world *w, lane_family *lf, int it)
{
    dlcol_clear_facts(lf->fam);
    for (int a = 0; a < lf->natoms; a++) {
        if (lf->is_fluent[a]) {
            for (int e = 0; e < lf->nent; e++) {
                uint32_t g = lf->ground[((size_t)a * lf->niter + it) * lf->nent + e];
                dl_lit l = { (uint32_t)a, !world_get(w, g) };  /* a if true, ~a else */
                dlcol_add_fact(lf->fam, l, e);
            }
        } else if (lf->is_import[a]) {
            /* a derived body atom, concluded elsewhere: query it and inject the
             * conclusion for each cell. Inject a literal ONLY when it is genuinely
             * proved (+∂): +a if a is PROVED, ~a if the complement is PROVED. Do
             * NOT map REFUTED to a ~a fact — REFUTED means -∂a (a finitely failed),
             * which does not make ~a provable; injecting it would force the wrong
             * verdict on the complement. With nothing injected the atom finitely
             * fails here too (no local rule concludes it), matching the N=1 path.
             * Equivalent to a defeasible import (§5.5), since nothing local
             * concludes `a`. (A cyclic UNDECIDED import can't be reproduced this
             * way — the differential oracle would flag it; the compiler rejects
             * cycles.) */
            for (int e = 0; e < lf->nent; e++) {
                uint32_t g = lf->ground[((size_t)a * lf->niter + it) * lf->nent + e];
                if (world_query(w, (dl_lit){ g, false }) == DL_PROVED)
                    dlcol_add_fact(lf->fam, (dl_lit){ (uint32_t)a, false }, e);
                else if (world_query(w, (dl_lit){ g, true }) == DL_PROVED)
                    dlcol_add_fact(lf->fam, (dl_lit){ (uint32_t)a, true }, e);
            }
        }
    }
    dlcol_solve(lf->fam);
}

int world_lanes_check(world *w, bool *ok)
{
    int checks = 0;
    if (ok) *ok = true;
    for (int i = 0; i < w->nlanes; i++) {
        lane_family *lf = &w->lanes[i];
        for (int it = 0; it < lf->niter; it++) {
            solve_lane_iter(w, lf, it);
            /* every (predicate, lane) verdict at this iteration must match the
             * N=1 query path on the equivalent named ground atom */
            for (int a = 0; a < lf->natoms; a++)
                for (int e = 0; e < lf->nent; e++) {
                    uint32_t g = lf->ground[((size_t)a * lf->niter + it) * lf->nent + e];
                    for (int neg = 0; neg < 2; neg++) {
                        dl_lit la = { (uint32_t)a, neg != 0 };
                        dl_lit gq = { g, neg != 0 };
                        /* compare to the N=1 path directly, not world_query —
                         * which routes back to lanes and would be circular */
                        if (dlcol_defeasible(lf->fam, la, e) != query_jfam(w, gq))
                            if (ok) *ok = false;
                        checks++;
                    }
                }
        }
        lf->solved = false;   /* left dirty for the router (it re-solves iter 0) */
    }
    return checks;
}

/* ---- step lane families (DoD thesis: the transition layer, bit-parallel) ---- */

void world_add_step_lane_family(world *w, dlcol *fam, int nloc, int nent,
                                const uint32_t *ground, const uint8_t *kind)
{
    GROW(w->steplanes, w->nsteplanes, w->capsteplanes);
    step_lane_family *sf = &w->steplanes[w->nsteplanes++];
    sf->fam = fam;
    sf->nloc = nloc;
    sf->nent = nent;
    sf->numsc = 0; sf->num_cell = NULL;           /* numeric extension: off unless */
    sf->nnumeff = 0; sf->numeff = NULL;            /* world_step_lane_set_numeric */
    sf->covers_numeric = false;
    sf->bcast_of = NULL; sf->bcast_of_cap = 0;     /* broadcast triggers: off unless set */
    sf->split_value = -1;                          /* whole-transition family (#121) */
    size_t g = (size_t)nloc * (size_t)nent;
    sf->ground = malloc((g ? g : 1) * sizeof *sf->ground);
    memcpy(sf->ground, ground, (g ? g : 1) * sizeof *sf->ground);
    sf->kind = malloc((size_t)(nloc ? nloc : 1) * sizeof *sf->kind);
    memcpy(sf->kind, kind, (size_t)(nloc ? nloc : 1) * sizeof *sf->kind);

    /* Map each cell to a world fluent index (CUR = fact source, PRIMED = commit
     * target) and each ground action atom to its cell, so a step reads w->vals
     * and its action list in O(cells)/O(k) — never the linear fluent_index scan.
     * A reverse index over the fluent + primed atoms makes the build O(cells)
     * too (a scan per cell would be O(nfl^2)). */
    sf->fl_of = malloc((g ? g : 1) * sizeof *sf->fl_of);
    for (size_t k = 0; k < g; k++) sf->fl_of[k] = -1;

    uint32_t maxf = 0;
    for (int i = 0; i < w->nfl; i++) {
        if (w->fluents[i] > maxf) maxf = w->fluents[i];
        if (w->primed[i]  > maxf) maxf = w->primed[i];
    }
    int *revf = malloc(((size_t)maxf + 1) * sizeof *revf);
    for (uint32_t k = 0; k <= maxf; k++) revf[k] = -1;
    for (int i = 0; i < w->nfl; i++) {
        revf[w->fluents[i]] = i;      /* the CUR ground atom is the fluent itself */
        revf[w->primed[i]]  = i;      /* the PRIMED ground atom is its f' twin    */
    }

    uint32_t maxa = 0;
    for (int a = 0; a < nloc; a++)
        if (kind[a] == WORLD_STEP_ACTION)
            for (int e = 0; e < nent; e++) {
                uint32_t ga = ground[(size_t)a * nent + e];
                if (ga > maxa) maxa = ga;
            }
    sf->act_of_cap = maxa + 1;
    sf->act_of = malloc((size_t)sf->act_of_cap * sizeof *sf->act_of);
    for (uint32_t k = 0; k < sf->act_of_cap; k++) sf->act_of[k] = -1;

    for (int a = 0; a < nloc; a++)
        for (int e = 0; e < nent; e++) {
            uint32_t ga = ground[(size_t)a * nent + e];
            size_t cell = (size_t)a * nent + e;
            if (kind[a] == WORLD_STEP_ACTION)
                sf->act_of[ga] = (int)cell;
            else
                sf->fl_of[cell] = ga <= maxf ? revf[ga] : -1;
        }
    free(revf);
    w->lanes_ok = true;           /* step lanes now reflect the current structure */
}

/* Attach the numeric lane extension to the last-added step lane family: `num_cell`
 * [numsc*nent] maps each (schema, lane) to a w->nums index; the effect arrays give
 * each fired numeric effect its schema, op, constant RHS, and marker local. Called
 * by emit_step_lanes after world_add_step_lane_family, only for the fully-covered
 * numeric case (covers_numeric = true → world_step may route here). */
void world_step_lane_set_numeric(world *w, int numsc, const uint32_t *num_atom_cell,
                                 int nnumeff, const int *eff_schema,
                                 const int *eff_op, const long *eff_konst,
                                 const uint32_t *eff_marker,
                                 const int *eff_ntest, const uint32_t *eff_tloc,
                                 const uint8_t *eff_tneg,
                                 const long *const *eff_table, int tstride)
{
    if (w->nsteplanes == 0) return;
    step_lane_family *sf = &w->steplanes[w->nsteplanes - 1];
    sf->numsc = numsc;
    size_t nc = (size_t)numsc * (size_t)sf->nent;
    sf->num_cell = malloc((nc ? nc : 1) * sizeof *sf->num_cell);
    for (size_t i = 0; i < nc; i++)              /* ground atom -> w->nums index */
        sf->num_cell[i] = num_index(w, num_atom_cell[i]);
    sf->nnumeff = nnumeff;
    sf->numeff = malloc((size_t)(nnumeff ? nnumeff : 1) * sizeof *sf->numeff);
    for (int i = 0; i < nnumeff; i++) {
        sf->numeff[i].schema = eff_schema[i];
        sf->numeff[i].op     = (world_numop)eff_op[i];
        sf->numeff[i].konst  = eff_konst[i];
        sf->numeff[i].marker = eff_marker[i];
        int k = eff_ntest ? eff_ntest[i] : 0;
        if (k > WORLD_LANE_MAXTEST) k = WORLD_LANE_MAXTEST;
        sf->numeff[i].ntest = k;
        for (int j = 0; j < k; j++) {
            sf->numeff[i].tloc[j] = eff_tloc[(size_t)i * tstride + j];
            sf->numeff[i].tneg[j] = eff_tneg[(size_t)i * tstride + j];
        }
        size_t ne = (size_t)1 << k;
        sf->numeff[i].table = malloc(ne * sizeof *sf->numeff[i].table);
        for (size_t m = 0; m < ne; m++)
            sf->numeff[i].table[m] = (eff_table && eff_table[i]) ? eff_table[i][m]
                                                                 : eff_konst[i];
    }
    sf->covers_numeric = true;
}

/* Register broadcast cast triggers on the last-added step lane family: each of the
 * `ncast` ground cast atoms drives the WORLD_STEP_BCAST local `cast_local[i]` — a
 * `for each` binder's cast fans out over every target lane of that local. */
void world_step_lane_set_bcast(world *w, int ncast, const uint32_t *cast_atom,
                               const int *cast_local)
{
    if (w->nsteplanes == 0 || ncast == 0) return;
    step_lane_family *sf = &w->steplanes[w->nsteplanes - 1];
    uint32_t maxa = 0;
    for (int i = 0; i < ncast; i++) if (cast_atom[i] > maxa) maxa = cast_atom[i];
    sf->bcast_of_cap = maxa + 1;
    sf->bcast_of = malloc((size_t)sf->bcast_of_cap * sizeof *sf->bcast_of);
    for (uint32_t k = 0; k < sf->bcast_of_cap; k++) sf->bcast_of[k] = -1;
    for (int i = 0; i < ncast; i++) sf->bcast_of[cast_atom[i]] = cast_local[i];
}

void world_step_lane_bind_value(world *w, int value_index)
{
    if (w->nsteplanes == 0) return;
    w->steplanes[w->nsteplanes - 1].split_value = value_index;
}

void world_step_rule_set_lane_cover(world *w, int rule, uint32_t value_mask)
{
    if (rule >= 0 && rule < w->nsr)
        w->srules[rule].lane_cover = value_mask;
}

int world_step_lane_family_count(const world *w) { return w->nsteplanes; }

/* True iff world_step will route the numeric transition through the lane family
 * (a single family that covers the numerics bit-parallel) — for tests/telemetry. */
bool world_routes_numeric(const world *w)
{
    return w->lanes_ok && w->nsteplanes == 1 && w->steplanes[0].covers_numeric;
}

/* Load a step lane family's fact columns from the current state and the given
 * action list, and solve it bit-parallel across lanes. Current fluents are
 * closed-world; an action local's lane bit is set iff its ground action atom is
 * in `actions`; primed locals carry no facts (they are the readout). */
static void solve_step_lane_family(world *w, step_lane_family *sf,
                                   const uint32_t *actions, int nactions)
{
    int W = (sf->nent + 63) / 64;
    dlcol_clear_facts(sf->fam);
    for (int a = 0; a < sf->nloc; a++) {
        if (sf->kind[a] == WORLD_STEP_CUR) {
            /* closed-world current state, read straight from w->vals via fl_of */
            for (int e = 0; e < sf->nent; e++) {
                int i = sf->fl_of[(size_t)a * sf->nent + e];
                dl_lit l = { (uint32_t)a, !(i >= 0 && w->vals[i]) };
                dlcol_add_fact(sf->fam, l, e);
            }
        } else if (sf->kind[a] == WORLD_STEP_ACTION || sf->kind[a] == WORLD_STEP_BCAST) {
            /* default every lane to "did not occur" (~action / ~cast); the
             * occurring ones are flipped below in O(#actions), not O(lanes) */
            uint64_t *pos = dlcol_fact_row(sf->fam, (dl_lit){ (uint32_t)a, false });
            uint64_t *neg = dlcol_fact_row(sf->fam, (dl_lit){ (uint32_t)a, true });
            memset(pos, 0x00, (size_t)W * sizeof *pos);
            memset(neg, 0xFF, (size_t)W * sizeof *neg);
        }
    }
    /* flip the occurring actions to true at their lanes (reverse map, O(k)) */
    for (int i = 0; i < nactions; i++) {
        uint32_t at = actions[i];
        if (at >= sf->act_of_cap) continue;
        int cell = sf->act_of[at];
        if (cell < 0) continue;
        int a = cell / sf->nent, e = cell % sf->nent;
        uint64_t *pos = dlcol_fact_row(sf->fam, (dl_lit){ (uint32_t)a, false });
        uint64_t *neg = dlcol_fact_row(sf->fam, (dl_lit){ (uint32_t)a, true });
        pos[e / 64] |=  (1ull << (e % 64));
        neg[e / 64] &= ~(1ull << (e % 64));
    }
    /* a broadcast cast (a `for each` binder) sets its whole column — the discrete
     * cast fans out over every target lane; per-lane `where`/`when` guards then
     * decide which lanes actually take the effect in the solve. */
    for (int i = 0; i < nactions; i++) {
        uint32_t at = actions[i];
        if (at >= sf->bcast_of_cap || sf->bcast_of[at] < 0) continue;
        int a = sf->bcast_of[at];
        uint64_t *pos = dlcol_fact_row(sf->fam, (dl_lit){ (uint32_t)a, false });
        uint64_t *neg = dlcol_fact_row(sf->fam, (dl_lit){ (uint32_t)a, true });
        memset(pos, 0xFF, (size_t)W * sizeof *pos);
        memset(neg, 0x00, (size_t)W * sizeof *neg);
    }
    dlcol_solve(sf->fam);
}

int world_step_lanes_check(world *w, const uint32_t *actions, int nactions,
                           bool *ok)
{
    int checks = 0;
    if (ok) *ok = true;
    ensure_fam(w);
    solve_step_family(w, actions, nactions);       /* the N=1 oracle */

    for (int i = 0; i < w->nsteplanes; i++) {
        step_lane_family *sf = &w->steplanes[i];
        solve_step_lane_family(w, sf, actions, nactions);
        /* every fluent's next-state verdict per lane must match the N=1 step
         * family on that fluent's primed twin */
        for (int a = 0; a < sf->nloc; a++) {
            if (sf->kind[a] != WORLD_STEP_PRIMED)
                continue;
            for (int e = 0; e < sf->nent; e++) {
                /* Skip lane-only synthetic readouts the same way the commit loop
                 * does (fl_of < 0): a numeric effect's fired marker is a PRIMED
                 * local whose ground atom exists in no N=1 family, so comparing
                 * it against N=1 asks a question with no answer — it would report
                 * a difference on every world that has a numeric effect at all. */
                if (sf->fl_of[(size_t)a * sf->nent + e] < 0)
                    continue;
                uint32_t pa = sf->ground[(size_t)a * sf->nent + e];   /* the f' atom */
                for (int neg = 0; neg < 2; neg++) {
                    dl_lit la = { (uint32_t)a, neg != 0 };
                    dl_verdict n1 = DL_UNDECIDED;
                    if (pa < w->loc_cap && w->loc_of[pa] != LOC_NONE) {
                        dl_lit gq = { w->loc_of[pa], neg != 0 };
                        n1 = dlcol_defeasible(w->fam, gq, 0);
                    }
                    if (dlcol_defeasible(sf->fam, la, e) != n1)
                        if (ok) *ok = false;
                    checks++;
                }
            }
        }
    }
    return checks;
}

/* Trace how a fluent got its value in the last step: the step theory (causal
 * rules, ramifications, generated inertia) as solved by the most recent
 * world_step. With `next` true, `q`'s atom is read in the next state (its primed
 * form) — the usual question, "why did `door` end up open?"; with `next` false,
 * the current-state value the step saw. Valid only after a world_step (the
 * family holds that transition's solution until the next step or edit). */
void world_step_why(world *w, dl_lit q, bool next, FILE *out)
{
    uint32_t atom = q.atom;
    if (next) {
        int i = fluent_index(w, atom);
        if (i >= 0)
            atom = w->primed[i];
        else if ((i = emit_index(w, atom)) >= 0)
            atom = w->emit_primed[i];  /* a burst cue is only ever about the
                                        * transition — "why did it fire?" (#11) */
    }
    bool fam_stale = w->fam_ver != w->struct_ver;
    if (!w->fam || fam_stale ||
        atom >= w->aloc_cap || w->aloc_of[atom] == LOC_NONE) {
        /* a lane-managed fluent in a mixed step (#121) has no residue rules —
         * it was solved bit-parallel on the per-value lane family */
        if (!fam_stale && w->fam && w->sp.nvals) {
            int i = fluent_index(w, q.atom);
            if (i >= 0) {
                fprintf(out, "why %s%s%s? solved on the split value's lane "
                             "family this step (bit-parallel); value = %s\n",
                        q.neg ? "~" : "", intern_name(w->syms, q.atom),
                        next ? "'" : "", w->vals[i] ? "true" : "false");
                return;
            }
        }
        fprintf(out, "why %s%s? not in the step theory%s\n",
                q.neg ? "~" : "", intern_name(w->syms, atom),
                fam_stale ? " (no step taken since the last edit)" : "");
        return;
    }
    /* Split narrowing (#121): a fluent outside the active value's write-set
     * has no rules in the narrowed family — not even inertia — because the
     * commit copied it through. Say so, instead of rendering an empty trace. */
    if (next && w->flw_cur) {
        int i = fluent_index(w, q.atom);
        if (i >= 0 && !w->flw_cur[i]) {
            fprintf(out, "why %s%s'? outside the split write-set this step — "
                         "no live rule can affect it; committed by copy "
                         "(inertia), value = %s\n",
                    q.neg ? "~" : "", intern_name(w->syms, q.atom),
                    w->vals[i] ? "true" : "false");
            return;
        }
    }
    /* if the last step was answered on the lanes, w->fam does not hold that
     * transition — replay it from the snapshot so the trace is the real one. */
    if (w->last_routed)
        solve_step_family_vals(w, w->step_snap, w->last_actions, w->last_nactions);
    dl_lit loc = { w->aloc_of[atom], q.neg };
    dlcol_why(w->fam, loc, 0, out);
}

static dl_lit primed_lit(const world *w, dl_lit l)
{
    int i = fluent_index(w, l.atom);
    if (i < 0) {                   /* tripwire, not a path: every primed read /
                                    * effect head must be a DECLARED fluent —
                                    * the grounder guarantees it (#92); reading
                                    * primed[-1] would be silent OOB */
        fprintf(stderr, "world: primed reference to undeclared fluent '%s'\n",
                intern_name(w->syms, l.atom));
        abort();
    }
    dl_lit p = { w->primed[i], l.neg };
    return p;
}

/* ---- the columnar schemas ----
 *
 * Both "what's true" (judgments, the query layer) and "what happens next"
 * (inertia + causal, the step) run on the columnar engine — one production
 * engine, the scalar dl kept only as test_col's differential oracle (§6.3).
 * Two N=1 dlcol families share one location map: `jfam` is the judgment rules
 * over current-state facts; `fam` adds generated inertia (f => f', ~f => ~f')
 * and causal rules, each superior to the inertia rule it conflicts with. The
 * structure is fixed between edits, so a step or query just rewrites fact
 * columns and re-solves. The scalar tests pin that the semantics are identical;
 * both families are N=1 today (entities are baked into atom names), so this is
 * columnar-in-structure — the per-entity *lanes* that make it fast are the M3
 * join matcher, a later change on this same family API. */

static uint32_t assign_loc(world *w, uint32_t atom, uint32_t *n)
{
    if (w->loc_of[atom] == LOC_NONE) {
        uint32_t loc = (*n)++;
        w->loc_of[atom] = loc;
        if (loc >= w->loc_atom_cap) {   /* grow the reverse loc->atom map (#68) */
            uint32_t nc = w->loc_atom_cap ? w->loc_atom_cap : 64;
            while (nc <= loc) nc *= 2;
            w->loc_atom = realloc(w->loc_atom, (size_t)nc * sizeof *w->loc_atom);
            w->loc_atom_cap = nc;
        }
        w->loc_atom[loc] = atom;
    }
    return w->loc_of[atom];
}

/* Rewrite a literal into a family's location space (`of` = its atom->loc map). */
static dl_lit map_lit(const uint32_t *of, dl_lit l)
{
    dl_lit m = { of[l.atom], l.neg };
    return m;
}

/* pass 1, shared: assign dense schema ids to every atom either family touches
 * (both are sized to the same location space; the judgment family simply leaves
 * the primed/action columns unused). Sets w->nloc.
 *
 * APPEND-ONLY (#67): an atom that already has a location KEEPS it — never memset
 * + reassigned — so a re-ground that changes the matched rule set does not shift
 * the locations of surviving atoms. This is the correctness prerequisite for
 * reusing a cached columnar family across re-grounds (#68): the cached dlcol's
 * columns are indexed by location, so a shifted location would misread every
 * verdict. Tradeoff: a location freed by a dropped match is not reclaimed until a
 * compaction pass, so nloc is bounded by the distinct atoms ever seen (#63). */
static void grow_loc_of(world *w)
{
    uint32_t na = intern_count(w->syms);
    if (na > w->loc_cap) {                          /* new interned atoms -> unassigned */
        w->loc_of = realloc(w->loc_of, (size_t)na * sizeof *w->loc_of);
        for (uint32_t k = w->loc_cap; k < na; k++) w->loc_of[k] = LOC_NONE;
        w->loc_cap = na;
    }
}

/* ---- the jfam-only sparse map (#77): same append-only discipline (#67),
 * separate space. The dense map interleaves f, f' from location 0 for the step
 * family's sake; the judgment family never reads a primed atom, so sizing it to
 * the dense space made every solve sweep ~2x all declared fluents. The sparse
 * map holds only judgment-rule atoms + lazily-materialized query atoms. */

static uint32_t jassign_loc(world *w, uint32_t atom)
{
    if (atom >= w->jloc_cap) {                     /* grow atom -> loc to intern size */
        uint32_t na = intern_count(w->syms);
        w->jloc_of = realloc(w->jloc_of, (size_t)na * sizeof *w->jloc_of);
        for (uint32_t k = w->jloc_cap; k < na; k++) w->jloc_of[k] = LOC_NONE;
        w->jloc_cap = na;
    }
    if (w->jloc_of[atom] == LOC_NONE) {
        uint32_t loc = w->njloc++;
        w->jloc_of[atom] = loc;
        if (loc >= w->jloc_atom_cap) {             /* grow the reverse loc -> atom map */
            uint32_t nc = w->jloc_atom_cap ? w->jloc_atom_cap : 64;
            while (nc <= loc) nc *= 2;
            w->jloc_atom = realloc(w->jloc_atom, (size_t)nc * sizeof *w->jloc_atom);
            w->jloc_atom_cap = nc;
        }
        w->jloc_atom[loc] = atom;
    }
    return w->jloc_of[atom];
}

/* jfam locations for jrules[from, to) heads/bodies, in rule order (I4:
 * deterministic given the same add/re-ground sequence). No fluent sweep, no
 * primed twins, no step rules — that is the whole point. */
static void assign_jlocs_range(world *w, int from, int to)
{
    for (int j = from; j < to; j++) {
        jassign_loc(w, w->jrules[j].head.atom);
        for (int i = 0; i < w->jrules[j].nbody; i++)
            jassign_loc(w, w->jrules[j].body[i].atom);
    }
}

/* Assign the locations of only the matched jrules[jr_matched_base, njr) (#68).
 * Fluent + static-rule + step-rule locations are already assigned (append-only,
 * #67) and never re-scanned per re-ground — that O(fluents) rescan was the fixed
 * per-update cost. */
static void assign_locs_matched(world *w)
{
    grow_loc_of(w);
    uint32_t n = w->nloc;
    for (int j = w->jr_matched_base; j < w->njr; j++) {
        assign_loc(w, w->jrules[j].head.atom, &n);
        for (int i = 0; i < w->jrules[j].nbody; i++)
            assign_loc(w, w->jrules[j].body[i].atom, &n);
    }
    w->nloc = n;
}

static void assign_locs(world *w)
{
    grow_loc_of(w);
    uint32_t n = w->nloc;                           /* resume from the high-water mark */
    w->fl_loc = realloc(w->fl_loc, (size_t)(w->nfl ? w->nfl : 1) * sizeof *w->fl_loc);
    w->pr_loc = realloc(w->pr_loc, (size_t)(w->nfl ? w->nfl : 1) * sizeof *w->pr_loc);
    for (int i = 0; i < w->nfl; i++) {
        w->fl_loc[i] = assign_loc(w, w->fluents[i], &n);
        w->pr_loc[i] = assign_loc(w, w->primed[i], &n);
    }
    /* burst cues (#11): only the primed twin gets a column — an emission has no
     * current-state reading to assign one for */
    for (int i = 0; i < w->nemit; i++)
        assign_loc(w, w->emit_primed[i], &n);
    for (int j = 0; j < w->njr; j++) {
        assign_loc(w, w->jrules[j].head.atom, &n);
        for (int i = 0; i < w->jrules[j].nbody; i++)
            assign_loc(w, w->jrules[j].body[i].atom, &n);
    }
    for (int s = 0; s < w->nsr; s++) {
        const srule *r = &w->srules[s];
        if (r->action != INTERN_NONE)
            assign_loc(w, r->action, &n);
        for (int i = 0; i < r->nbody; i++)
            assign_loc(w, r->body[i].primed
                              ? primed_lit(w, r->body[i].lit).atom
                              : r->body[i].lit.atom, &n);
        /* effect heads are primed fluents — already assigned */
    }
    w->nloc = n;
}

static void emit_atom_names(dlcol *f, const world *w)
{
    for (uint32_t a = 0; a < w->loc_cap; a++)
        if (w->loc_of[a] != LOC_NONE)
            dlcol_set_atom_name(f, w->loc_of[a], intern_name(w->syms, a));
}

/* Name only the schema locations [from,to) — the atoms a re-ground newly added
 * (#68). Append-only locations (#67) guarantee an atom's location is fixed, so a
 * name set once is never wrong; only the new tail needs naming. */
static void emit_atom_names_range(dlcol *f, const world *w, const uint32_t *atomv,
                                  uint32_t from, uint32_t to)
{
    for (uint32_t loc = from; loc < to; loc++)
        dlcol_set_atom_name(f, loc, intern_name(w->syms, atomv[loc]));
}

/* Add jrules[from,to) as columnar rules (no superiority — that is added once),
 * rewritten through the target family's location map `of`. */
static void emit_judgment_rule_range(dlcol *f, const world *w, const uint32_t *of,
                                     int from, int to)
{
    dl_lit lbuf[64];
    dl_lit *body = lbuf;
    int bodycap = 64;
    for (int j = from; j < to; j++) {
        const jrule *r = &w->jrules[j];
        if (r->nbody > bodycap) {
            if (body != lbuf) free(body);
            body = malloc((size_t)r->nbody * sizeof *body);
            bodycap = r->nbody;
        }
        for (int i = 0; i < r->nbody; i++)
            body[i] = map_lit(of, r->body[i]);
        int h = dlcol_add_rule(f, r->name, r->kind, map_lit(of, r->head),
                               body, r->nbody);
        if (r->prov)
            dlcol_set_prov(f, h, r->prov);
    }
    if (body != lbuf)
        free(body);
}

/* Judgment rules + their superiority — the whole judgment family, and the
 * leading slice of the step family (primed bodies may read these conclusions). */
static void emit_judgment_rules(dlcol *f, const world *w)
{
    emit_judgment_rule_range(f, w, w->loc_of, 0, w->njr);
    for (int j = 0; j < w->njs; j++)
        dlcol_add_sup(f, w->jsups[j].winner, w->jsups[j].loser);
}

/* The judgment (query-layer) family: judgment rules only, over current-state
 * facts. Same location space as the step family, just without inertia/causal.
 *
 * Incremental (#68): when the STATIC judgment rules are unchanged (a matcher
 * re-ground only churned the matched suffix, jfam_static_ver == static_ver),
 * REUSE the cached dlcol — truncate to the static-rule watermark, grow the atom
 * columns for any new atoms, name only those, and re-add the matched rules —
 * instead of a from-scratch `dlcol_new` + O(atoms) name emit + all rules. This is
 * sound because atom locations are append-only (#67), so the cached columns still
 * mean what they meant. Falls back to a full build when the static rules change. */
static void emit_judgment_family(world *w)
{
    if (w->jfam && w->jfam_static_ver == w->static_ver) {
        assign_locs_matched(w);        /* dense map: world_atom_loc's schedule (#67) */
        assign_jlocs_range(w, w->jr_matched_base, w->njr);
        dlcol_truncate_rules(w->jfam, w->jfam_wm_rules, w->jfam_wm_body, w->jfam_wm_sups);
        dlcol_ensure_atoms(w->jfam, (int)w->njloc);
        emit_atom_names_range(w->jfam, w, w->jloc_atom, w->jfam_named, w->njloc);
        w->jfam_named = w->njloc;
    } else {
        assign_locs(w);                /* dense map keeps its schedule: fluents +
                                        * rules + step rules (the step family and
                                        * the lazy path's located/absent line) */
        assign_jlocs_range(w, 0, w->njr);          /* sparse: judgment atoms only */
        if (w->jfam) dlcol_free(w->jfam);
        w->jfam = dlcol_new((int)w->njloc, 1);
        emit_atom_names_range(w->jfam, w, w->jloc_atom, 0, w->njloc);
        emit_judgment_rule_range(w->jfam, w, w->jloc_of, 0, w->jr_matched_base); /* static */
        for (int j = 0; j < w->njs; j++)                              /* static sups */
            dlcol_add_sup(w->jfam, w->jsups[j].winner, w->jsups[j].loser);
        w->jfam_wm_rules = dlcol_rule_count(w->jfam);
        w->jfam_wm_body  = dlcol_body_count(w->jfam);
        w->jfam_wm_sups  = dlcol_sup_count(w->jfam);
        w->jfam_named    = w->njloc;
        w->jfam_static_ver = w->static_ver;
    }
    emit_judgment_rule_range(w->jfam, w, w->jloc_of, w->jr_matched_base, w->njr); /* matched suffix */
    w->jfam_solved = false;
}

/* The value index of `atom` in the split domain, or -1. */
static int split_value_index(const world *w, uint32_t atom)
{
    return atom < w->sp.map_cap ? w->sp.map_of[atom] : -1;
}

/* The split guard of a step rule: the value index its body demands via an
 * unprimed positive cond on a split value atom; -2 = unguarded (live in every
 * value), -3 = two distinct values demanded (never fires — dead everywhere).
 * Detection is syntactic and dumb by design (#121): negated or primed reads
 * of the split fluent do not filter (the rule stays in every schema). */
static int srule_split_value(const world *w, const srule *r)
{
    int found = -2;
    for (int i = 0; i < r->nbody; i++) {
        if (r->body[i].primed || r->body[i].lit.neg)
            continue;
        int v = split_value_index(w, r->body[i].lit.atom);
        if (v < 0)
            continue;
        if (found == -2)      found = v;
        else if (found != v)  found = -3;
    }
    return found;
}

/* The step family: judgments + generated inertia + causal rules/ramifications.
 * `sv` < 0 builds the full schema; `sv` >= 0 builds the split value's narrowed
 * schema (#121): rules guarded on another value are omitted, and inertia is
 * emitted only for the value's write-set — the fluents a live rule writes or
 * reads primed (a primed read needs the inertia rule so its next-state verdict
 * settles). The write-set bitmaps are stored into the split cache slot so the
 * commit can copy excluded fluents through. The location space is shared with
 * the full schema (append-only, #67) — narrowing removes rules, never atoms. */
static dlcol *build_step_family(world *w, int sv, bool mix)
{
    uint8_t *live = NULL, *flw = NULL, *numw = NULL;
    if (sv >= 0) {
        live = calloc((size_t)(w->nsr ? w->nsr : 1), 1);
        flw  = calloc((size_t)(w->nfl ? w->nfl : 1), 1);
        numw = calloc((size_t)(w->nnum ? w->nnum : 1), 1);
        for (int s = 0; s < w->nsr; s++) {
            const srule *r = &w->srules[s];
            int g = srule_split_value(w, r);
            if (!(g == -2 || g == sv))
                continue;                              /* dead under sv */
            if (mix && ((r->lane_cover >> sv) & 1))
                continue;                              /* the per-value lane family
                                                        * owns it — residue omits it */
            live[s] = 1;
            for (int e = 0; e < r->neffects; e++) {
                int fi = fluent_index(w, r->effects[e].atom);
                if (fi >= 0) flw[fi] = 1;
            }
            for (int e = 0; e < r->nneff; e++) {
                int ni = num_index(w, r->neffs[e].num_atom);
                if (ni >= 0) numw[ni] = 1;
            }
            for (int i = 0; i < r->nbody; i++)
                if (r->body[i].primed) {
                    int fi = fluent_index(w, r->body[i].lit.atom);
                    if (fi >= 0) flw[fi] = 1;
                }
        }
        free(w->sp.flw[sv]);
        free(w->sp.numw[sv]);
        w->sp.flw[sv] = flw;
        w->sp.numw[sv] = numw;
    }

    dlcol *f = dlcol_new((int)w->nloc, 1);
    emit_atom_names(f, w);
    emit_judgment_rules(f, w);

    dl_lit lbuf[64];
    dl_lit *body = lbuf;
    int bodycap = 64;
    char buf[300];

    /* generated inertia, ids recorded for causal superiority */
    int *inertia_pos = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *inertia_pos);
    int *inertia_neg = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *inertia_neg);
    for (int i = 0; i < w->nfl; i++) {
        if (flw && !flw[i])
            continue;                  /* outside the write-set: commit by copy */
        const char *fname = intern_name(w->syms, w->fluents[i]);
        dl_lit now = { w->fl_loc[i], false }, nxt = { w->pr_loc[i], false };
        /* generated inertia reads in source terms (§6.3): a name the author
         * recognizes, and a provenance pointing at the fluent's declaration. */
        char prov[320];
        if (w->fl_prov[i])
            snprintf(prov, sizeof prov, "generated; declared %s", w->fl_prov[i]);
        else
            snprintf(prov, sizeof prov, "generated");
        snprintf(buf, sizeof buf, "inertia on %s", fname);
        inertia_pos[i] = dlcol_add_rule(f, buf, DL_DEFEASIBLE, nxt, &now, 1);
        dlcol_set_prov(f, inertia_pos[i], prov);
        dl_lit nnow = dl_complement(now), nnxt = dl_complement(nxt);
        inertia_neg[i] = dlcol_add_rule(f, buf, DL_DEFEASIBLE, nnxt, &nnow, 1);
        dlcol_set_prov(f, inertia_neg[i], prov);
    }

    /* causal rules and ramifications, one rule per effect, each superior to
     * the inertia rule it conflicts with */
    for (int s = 0; s < w->nsr; s++) {
        const srule *r = &w->srules[s];
        if (live && !live[s])
            continue;                  /* statically dead under this value */
        int nbody = r->nbody + (r->action != INTERN_NONE ? 1 : 0);
        if (nbody > bodycap) {
            if (body != lbuf)
                free(body);
            body = malloc((size_t)nbody * sizeof *body);
            bodycap = nbody;
        }
        int bi = 0;
        for (int i = 0; i < r->nbody; i++)
            body[bi++] = map_lit(w->loc_of, r->body[i].primed
                                        ? primed_lit(w, r->body[i].lit)
                                        : r->body[i].lit);
        if (r->action != INTERN_NONE) {
            dl_lit act = { r->action, false };
            body[bi++] = map_lit(w->loc_of, act);
        }
        for (int e = 0; e < r->neffects; e++) {
            dl_lit eff = r->effects[e];
            int em = emit_index(w, eff.atom);
            if (em >= 0) {         /* a burst cue (#11): concluded about this
                                    * transition, competing with no inertia
                                    * (there is none) and committed nowhere */
                dl_lit ehead = { w->loc_of[w->emit_primed[em]], eff.neg };
                snprintf(buf, sizeof buf, "%s/%s%s", r->name,
                         eff.neg ? "~" : "", intern_name(w->syms, eff.atom));
                int eid = dlcol_add_rule(f, buf, DL_DEFEASIBLE, ehead, body, nbody);
                if (r->prov)
                    dlcol_set_prov(f, eid, r->prov);
                continue;
            }
            int fi = fluent_index(w, eff.atom);
            if (fi < 0) {          /* tripwire (#92): effect heads must be
                                    * declared fluents; pr_loc[-1] is silent OOB */
                fprintf(stderr, "world: step effect on undeclared fluent '%s'\n",
                        intern_name(w->syms, eff.atom));
                abort();
            }
            dl_lit head = { w->pr_loc[fi], eff.neg };
            snprintf(buf, sizeof buf, "%s/%s%s", r->name,
                     eff.neg ? "~" : "", intern_name(w->syms, eff.atom));
            int rid = dlcol_add_rule(f, buf, DL_DEFEASIBLE, head, body, nbody);
            /* the causal rule is this authored step rule's effect — carry its
             * source span (generated inertia's own provenance is a later slice) */
            if (r->prov)
                dlcol_set_prov(f, rid, r->prov);
            /* effect f' conflicts with inertia-f ; effect ~f' with inertia+f */
            dlcol_add_sup(f, rid, eff.neg ? inertia_pos[fi] : inertia_neg[fi]);
        }
    }
    if (body != lbuf)
        free(body);
    free(inertia_pos);
    free(inertia_neg);
    free(live);
    return f;
}

/* Refresh the matched judgment layer against current facts before (re)building a
 * solve family (#45). The callback (world_matched_reset + re-match) bumps
 * struct_ver, so the family rebuilds below pick up the fresh layer. `regrounding`
 * blocks re-entry — the callback touches only the index and jrules, never a
 * solve, but the guard makes that contract explicit. */
static void refresh_matched(world *w)
{
    if (w->reground_fn && w->matched_stale && !w->regrounding) {
        w->regrounding = true;
        w->reground_fn(w->reground_ctx, w);
        w->matched_stale = false;
        w->regrounding = false;
    }
}

/* Ensure the judgment family reflects the current structure — the QUERY path.
 * Rebuilt only when its cached version lags struct_ver, so a re-ground that only
 * changed judgment rules does NOT drag the O(fluents) step-family rebuild onto a
 * query (#63). assign_locs is deterministic over the atom set, so jfam and fam
 * built at the same struct_ver share one location map. */
static void ensure_jfam(world *w)
{
    refresh_matched(w);
    if (w->jfam && w->jfam_ver == w->struct_ver)
        return;
    emit_judgment_family(w);       /* assigns locs (full or matched-only) + clears jfam_solved */
    w->jfam_ver = w->struct_ver;
}

/* Sparse loc for `atom` in the split residue space, assigning one if new. */
static uint32_t sadd(world *w, uint32_t atom)
{
    if (atom < w->sp.smap_cap && w->sp.smap[atom] != LOC_NONE)
        return w->sp.smap[atom];
    if (atom >= w->sp.smap_cap) {
        uint32_t nc = w->sp.smap_cap ? w->sp.smap_cap : 64;
        while (nc <= atom) nc *= 2;
        w->sp.smap = realloc(w->sp.smap, (size_t)nc * sizeof *w->sp.smap);
        for (uint32_t k = w->sp.smap_cap; k < nc; k++) w->sp.smap[k] = LOC_NONE;
        w->sp.smap_cap = nc;
    }
    if (w->sp.nsloc == w->sp.capsloc) {
        w->sp.capsloc = w->sp.capsloc ? w->sp.capsloc * 2 : 64;
        w->sp.satoms = realloc(w->sp.satoms,
                               (size_t)w->sp.capsloc * sizeof *w->sp.satoms);
    }
    w->sp.smap[atom] = (uint32_t)w->sp.nsloc;
    w->sp.satoms[w->sp.nsloc] = atom;
    return (uint32_t)w->sp.nsloc++;
}

/* Pull fluent `i` into the space: cur + primed locs, and the fact list. */
static void sadd_fluent(world *w, int i)
{
    if (i < 0 || w->sp.sfl_loc[i] != LOC_NONE)
        return;
    w->sp.sfl_loc[i] = sadd(w, w->fluents[i]);
    w->sp.spr_loc[i] = sadd(w, w->primed[i]);
    if (w->sp.nsfl == w->sp.capsfl) {
        w->sp.capsfl = w->sp.capsfl ? w->sp.capsfl * 2 : 64;
        w->sp.sfl_list = realloc(w->sp.sfl_list,
                                 (size_t)w->sp.capsfl * sizeof *w->sp.sfl_list);
    }
    w->sp.sfl_list[w->sp.nsfl++] = (uint32_t)i;
}

/* (Re)build the shared sparse residue space: every atom a mixed value's
 * residue schema can touch — all judgment rules, plus every srule that is
 * live-and-uncovered under at least one value. Anything else (the lane
 * half's per-actor churn) never enters, which is the whole point. */
static void ensure_split_space(world *w)
{
    if (w->sp.space_ver == w->struct_ver && w->sp.satoms)
        return;
    for (uint32_t k = 0; k < w->sp.smap_cap; k++) w->sp.smap[k] = LOC_NONE;
    w->sp.nsloc = 0;
    w->sp.nsfl = 0;
    w->sp.sfl_loc = realloc(w->sp.sfl_loc,
                            (size_t)(w->nfl ? w->nfl : 1) * sizeof *w->sp.sfl_loc);
    w->sp.spr_loc = realloc(w->sp.spr_loc,
                            (size_t)(w->nfl ? w->nfl : 1) * sizeof *w->sp.spr_loc);
    for (int i = 0; i < w->nfl; i++)
        w->sp.sfl_loc[i] = w->sp.spr_loc[i] = LOC_NONE;

    for (int j = 0; j < w->njr; j++) {
        sadd_fluent(w, fluent_index(w, w->jrules[j].head.atom));
        sadd(w, w->jrules[j].head.atom);
        for (int b = 0; b < w->jrules[j].nbody; b++) {
            sadd_fluent(w, fluent_index(w, w->jrules[j].body[b].atom));
            sadd(w, w->jrules[j].body[b].atom);
        }
    }
    uint32_t all = (1u << w->sp.nvals) - 1;
    for (int s = 0; s < w->nsr; s++) {
        const srule *r = &w->srules[s];
        int g = srule_split_value(w, r);
        uint32_t lv = g == -3 ? 0 : g == -2 ? all : (1u << g);
        if (!(lv & ~r->lane_cover))
            continue;                  /* lane-covered wherever it is live */
        if (r->action != INTERN_NONE)
            sadd(w, r->action);
        for (int b = 0; b < r->nbody; b++) {
            int fi = fluent_index(w, r->body[b].lit.atom);
            if (fi >= 0)
                sadd_fluent(w, fi);    /* cur + primed (a primed read needs both) */
            else
                sadd(w, r->body[b].lit.atom);   /* guard / judgment / marker */
        }
        for (int e = 0; e < r->neffects; e++) {
            int em = emit_index(w, r->effects[e].atom);
            if (em >= 0)                       /* burst cue: the primed twin only */
                sadd(w, w->emit_primed[em]);
            else
                sadd_fluent(w, fluent_index(w, r->effects[e].atom));
        }
    }
    w->sp.space_ver = w->struct_ver;
}

/* The residue schema for one MIXED split value, over the sparse space:
 * judgments + inertia for the value's write-set + the live uncovered rules.
 * Same semantics as build_step_family(v, mix=true), a fraction of the size. */
static dlcol *build_step_family_sparse(world *w, int sv)
{
    uint8_t *live = calloc((size_t)(w->nsr ? w->nsr : 1), 1);
    uint8_t *flw  = calloc((size_t)(w->nfl ? w->nfl : 1), 1);
    uint8_t *numw = calloc((size_t)(w->nnum ? w->nnum : 1), 1);
    for (int s = 0; s < w->nsr; s++) {
        const srule *r = &w->srules[s];
        int g = srule_split_value(w, r);
        if (!(g == -2 || g == sv) || ((r->lane_cover >> sv) & 1))
            continue;
        live[s] = 1;
        for (int e = 0; e < r->neffects; e++) {
            int fi = fluent_index(w, r->effects[e].atom);
            if (fi >= 0) flw[fi] = 1;
        }
        for (int e = 0; e < r->nneff; e++) {
            int ni = num_index(w, r->neffs[e].num_atom);
            if (ni >= 0) numw[ni] = 1;
        }
        for (int i = 0; i < r->nbody; i++)
            if (r->body[i].primed) {
                int fi = fluent_index(w, r->body[i].lit.atom);
                if (fi >= 0) flw[fi] = 1;
            }
    }
    free(w->sp.flw[sv]);
    free(w->sp.numw[sv]);
    w->sp.flw[sv] = flw;
    w->sp.numw[sv] = numw;

    dlcol *f = dlcol_new(w->sp.nsloc, 1);
    emit_atom_names_range(f, w, w->sp.satoms, 0, w->sp.nsloc);
    emit_judgment_rule_range(f, w, w->sp.smap, 0, w->njr);
    for (int j = 0; j < w->njs; j++)
        dlcol_add_sup(f, w->jsups[j].winner, w->jsups[j].loser);

    char buf[300];
    int *inertia_pos = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *inertia_pos);
    int *inertia_neg = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *inertia_neg);
    for (int i = 0; i < w->nfl; i++) {
        if (!flw[i])
            continue;
        const char *fname = intern_name(w->syms, w->fluents[i]);
        dl_lit now = { w->sp.sfl_loc[i], false }, nxt = { w->sp.spr_loc[i], false };
        char prov[320];
        if (w->fl_prov[i])
            snprintf(prov, sizeof prov, "generated; declared %s", w->fl_prov[i]);
        else
            snprintf(prov, sizeof prov, "generated");
        snprintf(buf, sizeof buf, "inertia on %s", fname);
        inertia_pos[i] = dlcol_add_rule(f, buf, DL_DEFEASIBLE, nxt, &now, 1);
        dlcol_set_prov(f, inertia_pos[i], prov);
        dl_lit nnow = dl_complement(now), nnxt = dl_complement(nxt);
        inertia_neg[i] = dlcol_add_rule(f, buf, DL_DEFEASIBLE, nnxt, &nnow, 1);
        dlcol_set_prov(f, inertia_neg[i], prov);
    }

    dl_lit lbuf[64];
    dl_lit *body = lbuf;
    int bodycap = 64;
    for (int s = 0; s < w->nsr; s++) {
        const srule *r = &w->srules[s];
        if (!live[s])
            continue;
        int nbody = r->nbody + (r->action != INTERN_NONE ? 1 : 0);
        if (nbody > bodycap) {
            if (body != lbuf) free(body);
            body = malloc((size_t)nbody * sizeof *body);
            bodycap = nbody;
        }
        int bi = 0;
        for (int i = 0; i < r->nbody; i++)
            body[bi++] = map_lit(w->sp.smap, r->body[i].primed
                                        ? primed_lit(w, r->body[i].lit)
                                        : r->body[i].lit);
        if (r->action != INTERN_NONE) {
            dl_lit act = { r->action, false };
            body[bi++] = map_lit(w->sp.smap, act);
        }
        for (int e = 0; e < r->neffects; e++) {
            dl_lit eff = r->effects[e];
            int em = emit_index(w, eff.atom);
            if (em >= 0) {                     /* a burst cue (#11): no inertia
                                                * to outrank, nothing to commit.
                                                * Kept identical to the dense
                                                * builder so the two cannot
                                                * disagree if the mixed route
                                                * ever admits a cue — today
                                                * ensure_fam declines to mix */
                dl_lit ehead = { w->sp.smap[w->emit_primed[em]], eff.neg };
                snprintf(buf, sizeof buf, "%s/%s%s", r->name,
                         eff.neg ? "~" : "", intern_name(w->syms, eff.atom));
                int eid = dlcol_add_rule(f, buf, DL_DEFEASIBLE, ehead, body, nbody);
                if (r->prov)
                    dlcol_set_prov(f, eid, r->prov);
                continue;
            }
            int fi = fluent_index(w, eff.atom);
            dl_lit head = { w->sp.spr_loc[fi], eff.neg };
            snprintf(buf, sizeof buf, "%s/%s%s", r->name,
                     eff.neg ? "~" : "", intern_name(w->syms, eff.atom));
            int rid = dlcol_add_rule(f, buf, DL_DEFEASIBLE, head, body, nbody);
            if (r->prov)
                dlcol_set_prov(f, rid, r->prov);
            dlcol_add_sup(f, rid, eff.neg ? inertia_pos[fi] : inertia_neg[fi]);
        }
    }
    if (body != lbuf)
        free(body);
    free(inertia_pos);
    free(inertia_neg);
    free(live);
    return f;
}

/* The current value index of the split fluent (pre-step state), or -1 when
 * split is inactive or no unique value atom is true (fall back to the full
 * schema — always safe, the narrowing is pure optimization). */
static int split_cur_value(const world *w)
{
    int found = -1;
    for (int v = 0; v < w->sp.nvals; v++) {
        int i = fluent_index(w, w->sp.vatoms[v]);
        if (i >= 0 && w->vals[i]) {
            if (found >= 0) return -1;             /* not unique */
            found = v;
        }
    }
    return found;
}

/* Ensure the step family reflects the current structure — the STEP path.
 * With split active (#121), select the cached per-value schema by the
 * PRE-step value of the split fluent, building it lazily; w->fam borrows the
 * selected slot and flw_cur/numw_cur carry the value's write-set narrowing
 * into the commit (NULL = full schema, everything live). */
static void ensure_fam(world *w)
{
    refresh_matched(w);
    int v = split_cur_value(w);
    if (v < 0) {
        if (!w->fam0 || w->fam0_ver != w->struct_ver) {
            assign_locs(w);
            if (w->fam0)
                dlcol_free(w->fam0);
            w->fam0 = build_step_family(w, -1, false);
            w->fam0_ver = w->struct_ver;
        }
        w->fam = w->fam0;
        w->flw_cur = NULL;
        w->numw_cur = NULL;
    } else {
        /* mixed routing (#121): the residue schema omits lane-covered rules
         * ONLY when the step will actually run the lane half — decided here,
         * stored with the cache slot, so the two can never disagree. A mixed
         * residue builds on the SPARSE space (O(residue) per step, the #77
         * trick); a value with no lane family stays on the dense schema. */
        bool mix = false;
        if (w->npg == 0 && w->lanes_ok && w->nemit == 0)   /* the lane half has no
                                                            * emit columns (#11) */
            for (int k = 0; k < w->nsteplanes; k++)
                if (w->steplanes[k].split_value == v) { mix = true; break; }
        if (!w->sp.fams[v] || w->sp.vers[v] != w->struct_ver ||
            (w->sp.mixed[v] != 0) != mix) {
            if (w->sp.fams[v])
                dlcol_free(w->sp.fams[v]);
            if (mix) {
                ensure_split_space(w);
                w->sp.fams[v] = build_step_family_sparse(w, v);
            } else {
                assign_locs(w);
                w->sp.fams[v] = build_step_family(w, v, false);
            }
            w->sp.vers[v] = w->struct_ver;
            w->sp.mixed[v] = mix;
        }
        w->fam = w->sp.fams[v];
        w->flw_cur = w->sp.flw[v];
        w->numw_cur = w->sp.numw[v];
        if (mix) {
            w->aloc_of = w->sp.smap;   w->aloc_cap = w->sp.smap_cap;
            w->afl_loc = w->sp.sfl_loc; w->apr_loc = w->sp.spr_loc;
            w->afl_list = w->sp.sfl_list; w->nafl = w->sp.nsfl;
            w->fam_ver = w->struct_ver;
            return;
        }
    }
    w->aloc_of = w->loc_of;  w->aloc_cap = w->loc_cap;    /* dense actives */
    w->afl_loc = w->fl_loc;  w->apr_loc = w->pr_loc;
    w->afl_list = NULL;      w->nafl = 0;
    w->fam_ver = w->struct_ver;        /* the active family is fresh */
}

/* Load current-state facts (closed-world fluents + numeric guard atoms) into the
 * judgment family and solve it — the columnar analog of the scalar theory
 * world_query used to rebuild per call. Cached until a state edit (jfam_solved). */
static void solve_judgment_family(world *w)
{
    dlcol *f = w->jfam;
    w->teg_ready = false;
    for (int phase = 0; ; phase++) {               /* two-phase iff test-guards (#86) */
        dlcol_clear_facts(f);
        /* Closed-world facts for the family's OWN atoms only (#77): iterate the
         * sparse locations, not the declared-fluent sweep — O(jfam atoms) where
         * the dense map is O(all fluents ever declared). A declared fluent
         * outside the family answers through lazy_judgment_verdict instead. */
        for (uint32_t loc = 0; loc < w->njloc; loc++) {
            int i = fluent_index(w, w->jloc_atom[loc]);
            if (i >= 0)
                dlcol_add_fact(f, (dl_lit){ loc, !w->vals[i] }, 0);  /* f or ~f */
            else if (schema_knows(w, w->jloc_atom[loc]))
                dlcol_add_fact(f, (dl_lit){ loc, true }, 0);
                /* #92: a rule references a never-touched fluent — closed-world
                 * false, exactly the fact the dense universe would have loaded */
        }
        load_guards(w, f, w->jloc_of, w->jloc_cap);
        load_providers(w, f, w->jloc_of, w->jloc_cap);
        load_eguards(w, f, w->jloc_of, w->jloc_cap);
        dlcol_solve(f);
        if (phase == 1 || w->n_teg == 0) break;
        eval_test_guards(w, f, w->jloc_of, w->jloc_cap);
    }
    w->teg_ready = false;
    w->jfam_solved = true;
}

/* ---- numeric write side: expression VM + commit pipeline (§5.8) ---- */

static bool lit_solved_proved(const world *w, uint32_t atom, bool neg);

/* Stack VM over `long`; integer-only. Reads numeric fluents' *current* values
 * (the store double-buffers, so every effect this step sees the pre-step
 * state). Bytecode is compiler-emitted and well-formed; depth 64 covers any
 * M1 effect expression. `undef` (may be NULL) is the out-of-band UNDEFINED
 * channel (#116): set true when an EXPR_REQDEF found a partial value with no
 * applicable definition — the result long is then meaningless, never an
 * in-band sentinel. Evaluation continues past it so every roll site is still
 * visited (§5.10 replay stability). */
static long eval_expr(const world *w, const expr_ins *code, int n, bool *undef)
{
    long st[64];
    long pstk[16];                /* layered-value prior stack (#82/#94) */
    int sp = 0, psp = 0;
    for (int i = 0; i < n; i++) {
        switch (code[i].op) {
        case EXPR_CONST: st[sp++] = code[i].arg; break;
        case EXPR_LOAD:  st[sp++] = world_get_num(w, (uint32_t)code[i].arg); break;
        case EXPR_ROLL:  st[sp++] = roll_value(w, (int)code[i].arg); break;
        case EXPR_CALL: {
            /* value-returning fn provider (§5.6): arg packs (pred<<8 | nargs);
             * the call args are the top `nargs` stack cells (contiguous longs),
             * replaced in place by the returned value. No callback -> 0. */
            unsigned long packed = (unsigned long)code[i].arg;
            uint32_t pred = (uint32_t)(packed >> 8);
            int nargs = (int)(packed & 0xff);
            sp -= nargs;
            long r = w->fn_provider_fn
                ? w->fn_provider_fn(w->fn_provider_ctx, pred, &st[sp], nargs)
                : 0;
            st[sp++] = r;
            break;
        }
        case EXPR_NEG:   st[sp-1] = -st[sp-1]; break;
        case EXPR_ADD:   sp--; st[sp-1] += st[sp]; break;
        case EXPR_SUB:   sp--; st[sp-1] -= st[sp]; break;
        case EXPR_MUL:   sp--; st[sp-1] *= st[sp]; break;
        case EXPR_MIN:   sp--; if (st[sp] < st[sp-1]) st[sp-1] = st[sp]; break;
        case EXPR_MAX:   sp--; if (st[sp] > st[sp-1]) st[sp-1] = st[sp]; break;
        case EXPR_DIV: { /* floored (round toward -inf); x/0 = 0 — see world.h */
            sp--;
            long a = st[sp-1], b = st[sp];
            long q = b == 0 ? 0 : a / b;
            if (b != 0 && a % b != 0 && (a < 0) != (b < 0)) q--;
            st[sp-1] = q;
            break;
        }
        case EXPR_TEST: {  /* solved verdict as 0/1 (#86); commit-side only */
            uint32_t atom = (uint32_t)((unsigned long)code[i].arg >> 1);
            st[sp++] = lit_solved_proved(w, atom, code[i].arg & 1) ? 1 : 0;
            break;
        }
        case EXPR_LOADN: {   /* primed numeric read (#84 modeled pipeline) */
            int li = num_index(w, (uint32_t)code[i].arg);
            st[sp++] = (li >= 0 && w->nn_cur && w->num_stratum &&
                        w->num_stratum[li] < w->cur_stratum)
                       ? w->nn_cur[li]
                       : (li >= 0 ? w->nums[li].value : 0);
            break;
        }
        case EXPR_PPUSH: if (psp < 16) pstk[psp++] = st[--sp]; break;
        case EXPR_P:     st[sp++] = psp > 0 ? pstk[psp-1] : 0; break;
        case EXPR_PPOP:  if (psp > 0) psp--; break;
        case EXPR_REQDEF:                 /* #116: definedness check, see world.h */
            if (st[--sp] <= 0 && undef) *undef = true;
            break;
        }
    }
    return sp ? st[sp-1] : 0;
}

/* Effective clamp bounds for numeric fluent `idx`: a dynamic bound
 * (`int in 0 .. hp_max(X)`) evaluates its bytecode against the value store; a
 * static bound uses the declared constant. Read against pre-step values (the
 * store double-buffers and swaps once), consistent with effect reads. */
static void num_clamp_bounds(const world *w, int idx, long *lo, long *hi)
{
    /* A partial-value read in a clamp bound is a compile error (#116's static
     * safety rule — a bound has no guarding body), so the undef channel is
     * dead here by construction; pass NULL and keep the check an assertion
     * in spirit. */
    *lo = w->nums[idx].lo_code ? eval_expr(w, w->nums[idx].lo_code, w->nums[idx].n_lo, NULL)
                               : w->nums[idx].min;
    *hi = w->nums[idx].hi_code ? eval_expr(w, w->nums[idx].hi_code, w->nums[idx].n_hi, NULL)
                               : w->nums[idx].max;
}

/* ---- the value algebra, instantiated at the integers (§5.8) --------------
 *
 * The numeric tier is one algebra: a JOIN resolving `:=` collisions, a
 * commutative MONOID accumulating the deltas, that monoid acting on the
 * carrier, and a RETRACTION onto the declared range. Both commit paths run
 * exactly it — the N=1 loop over named atoms and the routed lane fold over
 * columns — so it is written here once instead of in two copies that drift
 * apart the first time either grows a case.
 *
 * `static inline` rather than a vtable, deliberately. Naming the operations is
 * what a second domain needs; parameterizing by domain is a later step, and
 * one whose cost on the lane path (which folds inlined arithmetic over `long`
 * columns) has to be measured rather than assumed. */

/* `:=` join under the fluent's declared merge (#85). Returns 1 when `v` becomes
 * the winner (the caller may record its provenance), 0 when the incumbent
 * stands, and -1 when the collision is irreconcilable — REGISTER's two
 * different constants, the contested step of §5.8. */
static inline int num_join(world_merge mode, bool have, long *cur, long v)
{
    if (!have)                     { *cur = v; return 1; }
    if (mode == WORLD_MERGE_MIN)   { if (v < *cur) { *cur = v; return 1; } return 0; }
    if (mode == WORLD_MERGE_MAX)   { if (v > *cur) { *cur = v; return 1; } return 0; }
    return v == *cur ? 0 : -1;
}

/* The delta monoid's signed contribution. One definition of the sign
 * convention, so a receipt item and a lane column cannot disagree about it. */
static inline long num_signed(world_numop op, long k)
{
    return op == WORLD_OP_ADD ? k : -k;
}

/* The fixed commit pipeline for one fluent: base (the winning `:=`, else the
 * carried value) -> the accumulated deltas -> retract onto the declared range.
 * The clamp is outermost because the schema's range is the last word (§5.8). */
static inline long num_commit(const world *w, int idx, bool have,
                              long assigned, long delta)
{
    long v = (have ? assigned : w->nums[idx].value) + delta;
    if (w->nums[idx].has_range) {
        long lo, hi;
        num_clamp_bounds(w, idx, &lo, &hi);
        if (v < lo) v = lo;
        if (v > hi) v = hi;
    }
    return v;
}

/* A step rule fires this step iff its action occurred (or it is a ramification)
 * and every body literal holds in the settled step theory. Numeric effects run
 * in the commit phase, so their firing is read off the solved columns rather
 * than resolved inside the fixpoint (that is why suppression-by-superiority
 * over numeric effects is a later slice — here every fired effect contributes). */
static bool srule_fired(const world *w, const srule *r,
                        const uint32_t *actions, int nactions)
{
    if (r->action != INTERN_NONE) {
        bool occurred = false;
        for (int i = 0; i < nactions; i++)
            if (actions[i] == r->action) { occurred = true; break; }
        if (!occurred) return false;
    }
    for (int i = 0; i < r->nbody; i++) {
        dl_lit l = r->body[i].primed ? primed_lit(w, r->body[i].lit)
                                     : r->body[i].lit;
        if (l.atom >= w->aloc_cap || w->aloc_of[l.atom] == LOC_NONE)
            return false;
        dl_lit loc = { w->aloc_of[l.atom], l.neg };
        if (dlcol_defeasible(w->fam, loc, 0) != DL_PROVED)
            return false;
    }
    return true;
}

/* Is a literal defeasibly proved in the solved step theory? An atom no rule
 * mentions has no location — absent (0), never an error. Serves EXPR_TEST
 * (#86), which runs commit-side, strictly after the fixpoint settles. */
static bool lit_solved_proved(const world *w, uint32_t atom, bool neg)
{
    if (atom == INTERN_NONE) return false;
    /* Base fluents are closed-world state: answer from the store — identical
     * to the fact any solve loads for them, and it keeps test() working when
     * a SPARSE family carries no location for an atom referenced only inside
     * test() bytecode. Same for provider atoms (host-answered, load-time). */
    int fi = fluent_index(w, atom);
    if (fi >= 0) return neg ? !w->vals[fi] : w->vals[fi];
    int pvi = prov_index(w, atom);
    if (pvi >= 0) {
        bool holds = w->provider_fn &&
            w->provider_fn(w->provider_ctx, w->provs[pvi].pred,
                           w->provs[pvi].args, w->provs[pvi].nargs);
        return neg ? !holds : holds;
    }
    /* derived judgments read the solved family — pass-B guard evaluation
     * (#86) may point EXPR_TEST at a non-step family (the query-layer jfam);
     * everything else (effects) reads the step family */
    dlcol *f = w->tctx_fam ? w->tctx_fam : w->fam;
    const uint32_t *of = w->tctx_fam ? w->tctx_of : w->aloc_of;
    uint32_t cap = w->tctx_fam ? w->tctx_cap : w->aloc_cap;
    if (atom >= cap || of[atom] == LOC_NONE)
        return false;
    dl_lit loc = { of[atom], neg };
    return dlcol_defeasible(f, loc, 0) == DL_PROVED;
}

static void rcpt_push(num_receipt *rc, const char *rule, world_numop op, long amt)
{
    if (rc->n == rc->cap) {
        rc->cap = rc->cap ? rc->cap * 2 : 4;
        rc->items = realloc(rc->items, (size_t)rc->cap * sizeof *rc->items);
    }
    rc->items[rc->n].rule = rule;
    rc->items[rc->n].op = op;
    rc->items[rc->n].amount = amt;
    rc->n++;
}

/* Load a state (closed-world fluents from `vals` + numeric guard atoms) and the
 * occurring actions into the N=1 step family, and solve it. Shared by world_step
 * and world_step_lanes_check (the latter reads the primed columns without
 * committing), and by world_step_why replaying a routed step's snapshot. An
 * action atom no rule mentions is semantically inert; skip it. */
static void solve_step_family_vals(world *w, const bool *vals,
                                   const uint32_t *actions, int nactions)
{
    dlcol *f = w->fam;
    w->teg_ready = false;
    for (int phase = 0; ; phase++) {               /* two-phase iff test-guards (#86) */
        dlcol_clear_facts(f);
        if (w->afl_list) {                         /* sparse residue: its fluents only */
            for (int k = 0; k < w->nafl; k++) {
                int i = (int)w->afl_list[k];
                dl_lit l = { w->afl_loc[i], !vals[i] };
                dlcol_add_fact(f, l, 0);
            }
        } else {
            for (int i = 0; i < w->nfl; i++) {
                dl_lit l = { w->afl_loc[i], !vals[i] };
                dlcol_add_fact(f, l, 0);
            }
        }
        for (int i = 0; i < nactions; i++) {
            uint32_t a = actions[i];
            if (a < w->aloc_cap && w->aloc_of[a] != LOC_NONE) {
                dl_lit l = { w->aloc_of[a], false };
                dlcol_add_fact(f, l, 0);
            }
        }
        /* numeric guard atoms feed judgment rules inside the step theory too */
        load_guards(w, f, w->aloc_of, w->aloc_cap);
        load_providers(w, f, w->aloc_of, w->aloc_cap);
        load_eguards(w, f, w->aloc_of, w->aloc_cap);
        /* primed-guard facts minted by settled lower strata (§5.8 #87): strict
         * inputs about the NEXT value; empty until the stratum loop mints them */
        for (int i = 0; i < w->npg_cur; i++) {
            uint32_t a = w->pg_cur[i].atom;
            if (a < w->aloc_cap && w->aloc_of[a] != LOC_NONE)
                dlcol_add_fact(f, (dl_lit){ w->aloc_of[a], !w->pg_cur[i].val }, 0);
        }
        /* mixed routing (#121): the lane half already settled these fluents'
         * next state — inject it as strict primed facts (the lane half is a
         * stratum below the residue, same mechanism as the #87 mints above).
         * Only fluents in the residue's write-set matter: flw is exactly the
         * set whose primed locs any residue rule reads (the commit takes
         * lane-managed fluents straight from mix_next, never from here). */
        if (w->mix_managed)
            for (int i = 0; i < w->nfl; i++)
                if (w->mix_managed[i] && (!w->flw_cur || w->flw_cur[i]))
                    dlcol_add_fact(f, (dl_lit){ w->apr_loc[i], !w->mix_next[i] }, 0);
        dlcol_solve(f);
        if (phase == 1 || w->n_teg == 0) break;
        eval_test_guards(w, f, w->loc_of, w->loc_cap);
    }
    w->teg_ready = false;
}

static void solve_step_family(world *w, const uint32_t *actions, int nactions)
{
    solve_step_family_vals(w, w->vals, actions, nactions);
}

/* Snapshot the pre-step state + actions so a subsequent world_step_why can
 * replay a routed transition on the N=1 family (which the fast path skips). */
static void save_step_snapshot(world *w, const uint32_t *actions, int nactions)
{
    if (w->step_snap_cap < w->nfl) {
        w->step_snap = realloc(w->step_snap, (size_t)w->nfl * sizeof *w->step_snap);
        w->step_snap_cap = w->nfl;
    }
    if (w->nfl)
        memcpy(w->step_snap, w->vals, (size_t)w->nfl * sizeof *w->vals);
    if (w->last_actions_cap < nactions) {
        w->last_actions = realloc(w->last_actions,
                                  (size_t)nactions * sizeof *w->last_actions);
        w->last_actions_cap = nactions;
    }
    if (nactions)
        memcpy(w->last_actions, actions, (size_t)nactions * sizeof *actions);
    w->last_nactions = nactions;
}

/* Column-parallel numeric commit for a routed step (§5.8, bit-parallel firing).
 * Each numeric effect's `marker` was solved bit-parallel across lanes; read it per
 * lane and sum deltas / take the winning assign into the numeric column. Fills a
 * freshly-allocated `*out` [numsc*nent] with the next values (reads pre-step
 * values, writes nothing — the caller commits only if the whole step is clean).
 * Returns -1 on a contested `:=` without mutating. */
static int compute_step_lane_numerics(world *w, step_lane_family *sf,
                                      long **out, char *err, size_t errsz)
{
    int nent = sf->nent, numsc = sf->numsc;
    size_t cells = (size_t)numsc * (size_t)nent;
    long *delta = calloc(cells ? cells : 1, sizeof *delta);
    long *aval  = calloc(cells ? cells : 1, sizeof *aval);
    bool *have  = calloc(cells ? cells : 1, sizeof *have);
    bool *confl = calloc(cells ? cells : 1, sizeof *confl);

    for (int k = 0; k < sf->nnumeff; k++) {
        struct num_lane_eff *ef = &sf->numeff[k];
        dl_lit m = { ef->marker, false };
        for (int e = 0; e < nent; e++) {
            if (dlcol_defeasible(sf->fam, m, e) != DL_PROVED) continue;   /* did not fire */
            size_t c = (size_t)ef->schema * nent + e;
            /* #165: this lane's own verdicts select the RHS value. Bit j is
             * test j's verdict, exactly as EXPR_TEST reads it (PROVED = 1). */
            long konst = ef->konst;
            if (ef->ntest > 0) {
                unsigned mask = 0;
                for (int j = 0; j < ef->ntest; j++) {
                    dl_lit tl = { ef->tloc[j], ef->tneg[j] != 0 };
                    if (dlcol_defeasible(sf->fam, tl, e) == DL_PROVED)
                        mask |= 1u << j;
                }
                konst = ef->table[mask];
            }
            if (ef->op == WORLD_OP_ASSIGN) {
                world_merge mg = w->nums[sf->num_cell[c]].merge;   /* #85 */
                if (num_join(mg, have[c], &aval[c], konst) < 0)
                    confl[c] = true;
                have[c] = true;
            } else {
                delta[c] += num_signed(ef->op, konst);
            }
        }
    }

    int rc = 0;
    long *nextcol = malloc((cells ? cells : 1) * sizeof *nextcol);
    for (int s = 0; s < numsc && rc == 0; s++)
        for (int e = 0; e < nent; e++) {
            size_t c = (size_t)s * nent + e;
            int idx = sf->num_cell[c];
            if (confl[c]) {
                if (err)
                    snprintf(err, errsz, "conflicting `:=` effects on numeric fluent '%s'",
                             intern_name(w->syms, w->nums[idx].atom));
                rc = -1;
                break;
            }
            nextcol[c] = num_commit(w, idx, have[c], aval[c], delta[c]);
        }

    free(delta); free(aval); free(have); free(confl);
    if (rc != 0) { free(nextcol); *out = NULL; return rc; }
    *out = nextcol;
    return 0;
}

/* The routed transition: solve the step lane family bit-parallel and commit the
 * next state per lane, instead of the N=1 per-named-atom path. Contested/undecided
 * next values are an authoring error — return -1 without mutating (as N=1 does).
 * Booleans commit via the primed readout; when the family covers numerics, the
 * numeric columns commit in the same atomic step. Precondition: exactly one step
 * lane family covering every boolean fluent (and every numeric, if covers_numeric).
 * NOTE: a routed numeric step does not build per-fluent receipts (world_num_receipt
 * reflects the last N=1 step) — receipts on the fast path are a follow-up. */
static int world_step_lanes(world *w, const uint32_t *actions, int nactions,
                            char *err, size_t errsz)
{
    step_lane_family *sf = &w->steplanes[0];
    solve_step_lane_family(w, sf, actions, nactions);

    int rc = 0;
    bool *next = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *next);
    for (int i = 0; i < w->nfl; i++) next[i] = w->vals[i];   /* full coverage anyway */
    for (int a = 0; a < sf->nloc && rc == 0; a++) {
        if (sf->kind[a] != WORLD_STEP_PRIMED) continue;
        for (int e = 0; e < sf->nent; e++) {
            int i = sf->fl_of[(size_t)a * sf->nent + e];
            if (i < 0) continue;                        /* a marker / non-fluent readout */
            dl_lit p = { (uint32_t)a, false };
            if (dlcol_defeasible(sf->fam, p, e) == DL_PROVED) {
                next[i] = true;
            } else if (dlcol_defeasible(sf->fam, dl_complement(p), e) == DL_PROVED) {
                next[i] = false;
            } else {
                if (err)
                    snprintf(err, errsz,
                             "conflicting or undecided effects on fluent '%s'",
                             intern_name(w->syms, w->fluents[i]));
                rc = -1;
                break;
            }
        }
    }

    long *nextcol = NULL;
    if (rc == 0 && sf->covers_numeric && sf->numsc > 0)
        rc = compute_step_lane_numerics(w, sf, &nextcol, err, errsz);

    if (rc == 0) {
        save_step_snapshot(w, actions, nactions);   /* before overwriting vals */
        reindex_commit(w, next);                     /* index diff (before vals move) */
        if (w->nfl)
            memcpy(w->vals, next, (size_t)w->nfl * sizeof *next);
        for (int s = 0; s < sf->numsc; s++)          /* commit numeric columns */
            for (int e = 0; e < sf->nent; e++)
                w->nums[sf->num_cell[(size_t)s * sf->nent + e]].value =
                    nextcol[(size_t)s * sf->nent + e];
        invalidate_state_solved(w);
        w->last_routed = true;
    }
    free(next);
    free(nextcol);
    return rc;
}

int world_step(world *w, const uint32_t *actions, int nactions,
               char *err, size_t errsz)
{
    w->nemitbuf = 0;               /* burst cues are the LAST step's (#11): a
                                    * rejected step below emits nothing */

    /* Loud no-op actions (#119): an action atom that triggers ZERO step rules
     * can never do anything in this world — that is a host bug (typo'd atom,
     * wrong intern, protocol drift), not a legal no-op. Report it and leave
     * the world untouched. An action whose rules all fail their guards is NOT
     * this case: it matches by trigger and steps normally (the turn is spent). */
    for (int i = 0; i < nactions; i++)
        if (actions[i] >= w->trig_of_cap || w->trig_of[actions[i]] < 0) {
            if (err)
                snprintf(err, errsz, "action '%s' matches no step rule",
                         intern_name(w->syms, actions[i]));
            return -1;
        }

    /* Split loudness (#121, completing #119): an action ALL of whose rules
     * are statically dead under the current split value can do nothing this
     * step — that is protocol drift ("cast_shield during declare"), reported
     * with the action and the value, state untouched. Guard-failure within a
     * live rule stays a normal step. */
    if (w->sp.nvals) {
        int v = split_cur_value(w);
        if (v >= 0) {
            ensure_split_trig_masks(w);
            for (int i = 0; i < nactions; i++) {
                int m = actions[i] < w->sp.trig_cap ? w->sp.trig_of[actions[i]]
                                                    : -1;
                if (m >= 0 && !(m & (1 << v))) {
                    if (err)
                        snprintf(err, errsz,
                                 "action '%s' matches no live step rule while "
                                 "'%s' (split)",
                                 intern_name(w->syms, actions[i]),
                                 intern_name(w->syms, w->sp.vatoms[v]));
                    return -1;
                }
            }
        }
    }

    /* Exclusivity groups (#159): a declared group admits at most one member
     * action instance per (group, key) this step — the checked form of "the
     * host must not co-submit these" (§5.13; host-protocol class, retired
     * for bound hosts by the §6.3 binding). Pre-solve, state untouched. The
     * seen-scan is linear in group memberships of the SUBMITTED actions —
     * fine while grouped submissions are few; index if a profile says so. */
    if (w->nexcl) {
        struct eseen { int grp; uint32_t key, atom; };
        struct eseen sbuf[32], *seen = sbuf;
        int nseen = 0, capseen = 32, rc2 = 0;
        for (int i = 0; i < nactions && rc2 == 0; i++) {
            uint32_t a = actions[i];
            int e = a < w->excl_of_cap ? w->excl_of[a] : -1;
            for (; e >= 0 && rc2 == 0; e = w->excl_ents[e].next) {
                int g = w->excl_ents[e].grp;
                uint32_t k = w->excl_ents[e].key;
                bool have = false;
                for (int x = 0; x < nseen; x++) {
                    if (seen[x].grp != g || seen[x].key != k) continue;
                    if (seen[x].atom == a) { have = true; break; }   /* the same
                                                * action twice: one action */
                    if (err)
                        snprintf(err, errsz,
                                 "actions '%s' and '%s' are declared "
                                 "exclusive (%s%s%s) — a step may contain at "
                                 "most one of the group; submit them in "
                                 "separate steps",
                                 intern_name(w->syms, seen[x].atom),
                                 intern_name(w->syms, a),
                                 w->excl_grps[g].label,
                                 w->excl_grps[g].prov ? "; " : "",
                                 w->excl_grps[g].prov ? w->excl_grps[g].prov
                                                      : "");
                    rc2 = -1;
                    break;
                }
                if (rc2 || have) continue;
                if (nseen == capseen) {
                    capseen *= 2;
                    struct eseen *ns = malloc((size_t)capseen * sizeof *ns);
                    memcpy(ns, seen, (size_t)nseen * sizeof *ns);
                    if (seen != sbuf) free(seen);
                    seen = ns;
                }
                seen[nseen].grp = g;
                seen[nseen].key = k;
                seen[nseen].atom = a;
                nseen++;
            }
        }
        if (seen != sbuf) free(seen);
        if (rc2) return -1;
    }

    ensure_fam(w);

    /* the hot path: a homogeneous step world lanes its whole transition, so solve
     * it bit-parallel across entities. Numerics ride when the family covers them
     * (covers_numeric): the boolean firing solves bit-parallel and the numeric
     * columns commit column-parallel. (w->lanes_ok guards post-compile edits.) */
    if (w->lanes_ok && w->nsteplanes == 1 && w->npg == 0 && w->sp.nvals == 0 &&
        w->nemit == 0 &&           /* lanes carry no emit columns (#11) */
        (w->nnum == 0 || w->steplanes[0].covers_numeric)) {
        int rc = world_step_lanes(w, actions, nactions, err, errsz);
        if (rc == 0) w->tick++;                    /* monotone step counter (§5.10) */
        return rc;
    }

    /* Mixed lane/N=1 routing (#121): with split active, the grounder may have
     * built a per-value lane family for the current value's homogeneous
     * residue (its split guards erased — statically true here). Solve it
     * FIRST, bit-parallel; its fluents' next-state then enters the N=1
     * residue solve below as strict primed facts (the lane half is a stratum
     * under the residue), and the commit takes those fluents from the lane
     * result. Falls through to plain N=1 when no family matches the value. */
    bool *mix_next = NULL;
    uint8_t *mix_managed = NULL;
    if (w->sp.nvals) {
        int v = split_cur_value(w);
        step_lane_family *sf = NULL;
        if (v >= 0 && w->sp.mixed[v])      /* the residue fam omits covered
                                            * rules — the lane half MUST run */
            for (int k = 0; k < w->nsteplanes; k++)
                if (w->steplanes[k].split_value == v) { sf = &w->steplanes[k]; break; }
        if (sf) {
            solve_step_lane_family(w, sf, actions, nactions);
            mix_next    = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *mix_next);
            mix_managed = calloc((size_t)(w->nfl ? w->nfl : 1), 1);
            for (int a = 0; a < sf->nloc; a++) {
                if (sf->kind[a] != WORLD_STEP_PRIMED) continue;
                for (int e = 0; e < sf->nent; e++) {
                    int i = sf->fl_of[(size_t)a * sf->nent + e];
                    if (i < 0) continue;           /* marker / non-fluent readout */
                    dl_lit p = { (uint32_t)a, false };
                    if (dlcol_defeasible(sf->fam, p, e) == DL_PROVED) {
                        mix_next[i] = true;
                    } else if (dlcol_defeasible(sf->fam, dl_complement(p), e)
                               == DL_PROVED) {
                        mix_next[i] = false;
                    } else {
                        if (err)
                            snprintf(err, errsz,
                                     "conflicting or undecided effects on "
                                     "fluent '%s'",
                                     intern_name(w->syms, w->fluents[i]));
                        free(mix_next);
                        free(mix_managed);
                        return -1;
                    }
                    mix_managed[i] = 1;
                }
            }
            w->mix_next = mix_next;
            w->mix_managed = mix_managed;
        }
    }

    dlcol *f = w->fam;

    /* §5.8 strata (#87): with primed guards, the tick runs one solve per
     * stratum — each solve additionally sees the primed-guard facts minted by
     * the strata below it; the numeric pipeline runs for the fluents each
     * stratum owns; the boolean next-state is read from the FINAL solve; and
     * the state write stays atomic at the very end, so the action log records
     * ONE step (I4 — replay never sees a half-tick). Zero primed guards is
     * the degenerate one-stratum case: exactly one solve, today's tick. */
    /* structure-only scaffolding, cached per struct_ver (stored as ver+1 so a
     * fresh world at ver 0 still computes once): the O(nsr) sweeps here were
     * the mixed route's hot spot once the solves themselves were narrowed */
    if (w->scaffold_ver != w->struct_ver + 1 ||
        w->scaffold_nsr != w->nsr || w->scaffold_nnum != w->nnum) {
        int nst = 1;
        for (int s = 0; s < w->nsr; s++)
            if (w->srules[s].stratum + 1 > nst) nst = w->srules[s].stratum + 1;
        w->nstrata_cache = nst;
        if (w->num_stratum_cap < w->nnum) {
            w->num_stratum = realloc(w->num_stratum,
                                     (size_t)w->nnum * sizeof *w->num_stratum);
            w->num_stratum_cap = w->nnum;
        }
        for (int i = 0; i < w->nnum; i++) w->num_stratum[i] = 0;
        w->n_neff_rules = 0;
        for (int s = 0; s < w->nsr; s++) {         /* fluent stratum = max writer */
            if (w->srules[s].nneff == 0)
                continue;
            for (int e = 0; e < w->srules[s].nneff; e++) {
                int i = num_index(w, w->srules[s].neffs[e].num_atom);
                if (i >= 0 && w->srules[s].stratum > w->num_stratum[i])
                    w->num_stratum[i] = w->srules[s].stratum;
            }
            if (w->n_neff_rules == w->cap_neff_rules) {
                w->cap_neff_rules = w->cap_neff_rules ? w->cap_neff_rules * 2 : 16;
                w->neff_rules = realloc(w->neff_rules,
                                        (size_t)w->cap_neff_rules
                                            * sizeof *w->neff_rules);
            }
            w->neff_rules[w->n_neff_rules++] = s;
        }
        w->scaffold_ver = w->struct_ver + 1;
        w->scaffold_nsr = w->nsr;
        w->scaffold_nnum = w->nnum;
    }
    int nstrata = w->nstrata_cache;
    w->npg_cur = 0;

    int rc = 0;
    /* numeric commit pipeline (§5.8): base (winning := else inertia) + Σ deltas,
     * clamped to the declared range. Built into scratch + receipts, committed
     * with the boolean state only if nothing is contested. */
    long *nextnum = malloc((size_t)(w->nnum ? w->nnum : 1) * sizeof *nextnum);
    if (w->nnum > w->caprcpt) {
        w->rcpt = realloc(w->rcpt, (size_t)w->nnum * sizeof *w->rcpt);
        memset(&w->rcpt[w->caprcpt], 0,
               (size_t)(w->nnum - w->caprcpt) * sizeof *w->rcpt);
        w->caprcpt = w->nnum;
    }
    /* One pass over the step rules per stratum — NOT nnum × nsr (that double
     * scan was the O(N²) crowd wall). Each fired numeric effect routes to its
     * fluent's accumulator via the O(1) num index, at the stratum the fluent
     * settles; a per-fluent pass runs the pipeline (base + Σ deltas, clamp)
     * and finishes the receipts. Accumulators live across the whole tick —
     * each fluent is touched at exactly one stratum. */
    struct nacc { long delta, assign_val; const char *rule;
                  bool have, conflict; } *acc =
        calloc((size_t)(w->nnum ? w->nnum : 1), sizeof *acc);
    for (int i = 0; i < w->nnum; i++) w->rcpt[i].n = 0;

    for (int st = 0; st < nstrata && rc == 0; st++) {
        w->nn_cur = nextnum;                       /* EXPR_LOADN context (#84) */
        w->cur_stratum = st;
        solve_step_family(w, actions, nactions);   /* injects pg_cur facts */

        for (int k = 0; k < w->n_neff_rules; k++) {
            const srule *r = &w->srules[w->neff_rules[k]];
            if (!srule_fired(w, r, actions, nactions))
                continue;
            for (int e = 0; e < r->nneff; e++) {
                const num_effect *ef = &r->neffs[e];
                int i = num_index(w, ef->num_atom);
                if (i < 0 || w->num_stratum[i] != st) continue;
                bool und = false;
                long v = eval_expr(w, ef->code, ef->ncode, &und);
                if (und) {
                    /* Unreachable from a clean compile (#116's static safety
                     * rule blocks unguarded partial reads in effect RHS) —
                     * defense in depth against compiler bugs, outside the
                     * language contract. */
                    if (err)
                        snprintf(err, errsz,
                                 "internal: effect of rule '%s' read an "
                                 "undefined partial value (compiler bug — "
                                 "#116 static safety)", r->name);
                    rc = -1;
                    break;
                }
                if (ef->op == WORLD_OP_ASSIGN) {
                    int took = num_join(w->nums[i].merge, acc[i].have,
                                        &acc[i].assign_val, v);
                    acc[i].have = true;
                    if (took < 0)
                        acc[i].conflict = true;         /* register: contested (§5.8) */
                    else if (took > 0)
                        acc[i].rule = r->name;          /* the winner's provenance */
                } else {
                    long d = num_signed(ef->op, v);
                    acc[i].delta += d;
                    rcpt_push(&w->rcpt[i], r->name, ef->op, d);
                }
            }
            if (rc != 0) break;                    /* internal undef trap above */
        }

        for (int i = 0; rc == 0 && i < w->nnum; i++) {
            if (w->num_stratum[i] != st) continue;   /* owned by another stratum */
            if (w->numw_cur && !w->numw_cur[i] &&
                !w->nums[i].lo_code && !w->nums[i].hi_code) {
                /* outside the split write-set (#121) with static bounds: no
                 * live effect can touch it and the stored value is already
                 * clamped — copy through. Dynamic bounds still re-clamp (a
                 * bound expression may read fluents that DID change). */
                nextnum[i] = w->nums[i].value;
                w->rcpt[i].base = nextnum[i];
                continue;
            }
            num_receipt *rcp = &w->rcpt[i];
            if (acc[i].conflict) {
                if (err)
                    snprintf(err, errsz,
                             "conflicting `:=` effects on numeric fluent '%s'",
                             intern_name(w->syms, w->nums[i].atom));
                rc = -1;
                break;
            }
            /* receipt order: winning assign first, then the deltas in scan order */
            if (acc[i].have) {
                rcpt_push(rcp, NULL, WORLD_OP_ASSIGN, 0);   /* grow, then shift */
                memmove(&rcp->items[1], &rcp->items[0],
                        (size_t)(rcp->n - 1) * sizeof *rcp->items);
                rcp->items[0].rule = acc[i].rule;
                rcp->items[0].op = WORLD_OP_ASSIGN;
                rcp->items[0].amount = acc[i].assign_val;
            }
            rcp->base = acc[i].have ? acc[i].assign_val : w->nums[i].value;
            nextnum[i] = num_commit(w, i, acc[i].have, acc[i].assign_val,
                                    acc[i].delta);
        }

        /* mint this stratum's primed-guard facts (§5.8 #87): strict inputs
         * about the settled next values, visible to every solve above. A
         * pguard's fluent always sits below the top stratum (its readers are
         * above it by construction), so nothing is minted uselessly. */
        for (int g = 0; rc == 0 && g < w->npg; g++) {
            int i = num_index(w, w->pguards[g].num);
            if (i < 0 || w->num_stratum[i] != st) continue;
            long v = nextnum[i], t = w->pguards[g].threshold;
            bool holds;
            switch (w->pguards[g].op) {
            case WORLD_CMP_LE: holds = v <= t; break;
            case WORLD_CMP_LT: holds = v <  t; break;
            case WORLD_CMP_GE: holds = v >= t; break;
            case WORLD_CMP_GT: holds = v >  t; break;
            default:           holds = v == t; break;
            }
            if (w->npg_cur == w->cappg_cur) {
                w->cappg_cur = w->cappg_cur ? w->cappg_cur * 2 : 8;
                w->pg_cur = realloc(w->pg_cur,
                                    (size_t)w->cappg_cur * sizeof *w->pg_cur);
            }
            w->pg_cur[w->npg_cur].atom = w->pguards[g].guard;
            w->pg_cur[w->npg_cur].val = holds;
            w->npg_cur++;
        }
    }
    free(acc);
    w->nn_cur = NULL;                              /* commit-loop context down */

    /* boolean next-state + contested check, from the FINAL solve */
    bool *next = malloc((size_t)(w->nfl ? w->nfl : 1) * sizeof *next);
    for (int i = 0; rc == 0 && i < w->nfl; i++) {
        if (w->mix_managed && w->mix_managed[i]) {
            next[i] = w->mix_next[i];  /* the lane half owns it (#121 mixed) */
            continue;
        }
        if (w->flw_cur && !w->flw_cur[i]) {
            next[i] = w->vals[i];      /* outside the split write-set (#121):
                                        * no live writer, inertia only — copy */
            continue;
        }
        dl_lit p = { w->apr_loc[i], false };
        if (dlcol_defeasible(f, p, 0) == DL_PROVED) {
            next[i] = true;
        } else if (dlcol_defeasible(f, dl_complement(p), 0) == DL_PROVED) {
            next[i] = false;
        } else {
            if (err)
                snprintf(err, errsz,
                         "conflicting or undecided effects on fluent '%s'",
                         intern_name(w->syms, w->fluents[i]));
            rc = -1;
            break;
        }
    }

    /* burst cues (#11, §12): read the transition's emissions off the SAME final
     * solve the boolean next-state came from, in declaration order (I4). An
     * emission that nothing concluded is simply absent — there is no inertia to
     * carry it and no closed-world fact to refute it. */
    for (int i = 0; rc == 0 && i < w->nemit; i++) {
        uint32_t pr = w->emit_primed[i];
        uint32_t loc = pr < w->aloc_cap ? w->aloc_of[pr] : LOC_NONE;
        if (loc == LOC_NONE)
            continue;                                /* outside this schema */
        if (dlcol_defeasible(f, (dl_lit){ loc, false }, 0) != DL_PROVED)
            continue;
        GROW(w->emitbuf, w->nemitbuf, w->capemitbuf);
        w->emitbuf[w->nemitbuf++] = w->emits[i];
    }

    if (rc == 0) {
        reindex_commit(w, next);                     /* index diff (before vals move) */
        if (w->nfl)
            memcpy(w->vals, next, (size_t)w->nfl * sizeof *next);
        for (int i = 0; i < w->nnum; i++)
            w->nums[i].value = nextnum[i];
        invalidate_state_solved(w);                /* new state: judgments stale */
        w->last_routed = false;                    /* w->fam holds this transition */
    }

    if (rc == 0) w->tick++;                        /* monotone step counter (§5.10) */
    w->mix_managed = NULL;                         /* mixed context down (#121) */
    w->mix_next = NULL;
    free(mix_managed);
    free(mix_next);
    free(next);
    free(nextnum);
    return rc;
}

int world_num_receipt(const world *w, uint32_t atom, long *base,
                      world_contrib *out, int cap)
{
    int i = num_index(w, atom);
    if (i < 0 || !w->rcpt) {
        if (base) *base = 0;
        return 0;
    }
    const num_receipt *rcp = &w->rcpt[i];
    if (base) *base = rcp->base;
    for (int k = 0; k < rcp->n && k < cap; k++)
        out[k] = rcp->items[k];
    return rcp->n;
}
