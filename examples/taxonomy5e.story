// taxonomy5e.story — the EPIC #123 probe (#127): the SRD d20 space authored
// end-to-end with KINDS AS RULES. There is no kind construct: the taxonomy is
// kind predicates (boolean values over the built-in `value` meta-sort),
// membership `fact`s, and BUILD-TIME rules — solved at grounding by the same
// engine that answers runtime queries, with the same why-trace.
//
// What this file states, PHB-verbatim-adjacently:
//   - "a d20 roll" ⊇ ability checks, saving throws, attack rolls — the
//     d20(V) hierarchy, derived by three one-line rules;
//   - Bless: "+d4 on attack rolls and saving throws" — a UNION, written as a
//     derived kind (bless_roll), the sentence that broke flat kinds;
//   - Halfling Luck selects ALL of d20(V) — the second level of the pair
//     (the reroll-a-1 mechanic itself is out of scope; the modifier here is
//     a floor, the SELECTION is what the probe proves);
//   - Rage / Bracers of Archery / War Caster: melee-weapon vs ranged-weapon
//     vs any-spell — three axes cross-cutting ONE attack space; no nesting
//     was ever chosen, which is the product-not-tree claim;
//   - "nonmagical bludgeoning, piercing, or slashing damage" — brutal(V),
//     closed-world negation in the sealed stratum, one named predicate;
//   - the thrown dagger: a melee weapon attacking at range —
//     thrown_not_melee > melee_by_weapon, defeasible taxonomy with the
//     defeat visible in the build-time why (pinned by the test);
//   - "initiative is a Dexterity check": ONE membership fact at the bottom
//     of the file and every upstream modifier (Luck) covers it — program
//     union within one file, the degenerate module case.
//
// Gaps found and filed (per the probe ground rule):
//   - #143: cross-value links (`dmg_of(V)`) — real Rage lands +2 on the
//     DAMAGE roll of melee weapon attacks; this file scopes Rage to the
//     attack-roll-adjacent formulation and the damage half waits there.
//   - #144: ordered non-commuting kind modifiers — resistance halves
//     (`prior / 2`) and penalties subtract (`prior - e`); the commuting
//     class (#94) excludes both even when explicitly ordered, so the ward
//     below is spelled as a `min` cap instead of a halving.
//   - #145: kind-level superiority — "Luck applies above Bless, everywhere
//     they meet" costs TEN dotted sup lines below (one per shared member);
//     `halfling_luck > bless` should desugar to them. The probe's biggest
//     authoring-friction finding.

sort actor
enum ability { str, dex, con, wis }
enum reach   { melee, ranged }
enum source  { weapon, spell }
enum dtype   { bludgeoning, piercing, slashing, fire }

entity ( bran, grik : actor )

state (
    blessed(actor)  lucky(actor)  raging(actor)  archer(actor)
    warcaster(actor)  warded(actor)
    ss(actor) : int   cs(actor) : int   ath(actor) : int
    swa(actor) : int  bwa(actor) : int  fba(actor) : int  dta(actor) : int
    swd(actor) : int  fbd(actor) : int  ini(actor) : int
)

// ---- the taxonomy: kind predicates + membership facts ----------------------

value ( save(value, ability)
        check(value, ability)
        attack(value, reach, source)
        damage(value, dtype)
        magical(value)
        uses_melee_weapon(value)
        thrown_mode(value)
        // derived below — nobody asserts these
        d20(value)  bless_roll(value)  brutal(value)

        // the values themselves (runtime, per-actor)
        spell_save(actor)    : int
        con_save(actor)      : int
        athletics(actor)     : int
        sword_atk(actor)     : int
        bow_atk(actor)       : int
        firebolt_atk(actor)  : int
        dagger_throw(actor)  : int
        sword_dmg(actor)     : int
        firebolt_dmg(actor)  : int )

fact ( save(spell_save, wis)   save(con_save, con)
       check(athletics, str)
       attack(sword_atk, melee, weapon)
       attack(bow_atk, ranged, weapon)
       attack(firebolt_atk, ranged, spell)
       uses_melee_weapon(dagger_throw)  thrown_mode(dagger_throw)
       damage(sword_dmg, slashing)
       damage(firebolt_dmg, fire)      magical(firebolt_dmg) )

// ---- the hierarchy: d20 ⊇ saves, checks, attacks (overlapping, no tree) ----

rule saves_roll_d20(V: value, A: ability):  save(V, A)      => d20(V)
rule checks_roll_d20(V: value, A: ability): check(V, A)     => d20(V)
rule attacks_roll_d20(V: value):            attack(V, _, _) => d20(V)

// Bless's union — saves and attacks, not checks — as a derived kind
rule bless_saves(V: value, A: ability): save(V, A)      => bless_roll(V)
rule bless_attacks(V: value):           attack(V, _, _) => bless_roll(V)

// "nonmagical bludgeoning, piercing, or slashing" — one named predicate
rule nonmagical_physical(V: value, D: dtype):
    damage(V, D) & D in {bludgeoning, piercing, slashing} & ~magical(V)
    => brutal(V)

// the thrown dagger: a melee weapon, attacking at range — defeasibly
rule melee_by_weapon(V: value):  uses_melee_weapon(V) => attack(V, melee, weapon)
rule thrown_not_melee(V: value): thrown_mode(V)       => ~attack(V, melee, weapon)
rule thrown_is_ranged(V: value): thrown_mode(V)       => attack(V, ranged, weapon)
thrown_not_melee > melee_by_weapon

// ---- base definitions ------------------------------------------------------

rule b1(X: actor): => spell_save(X)   = 10
rule b2(X: actor): => con_save(X)     = 10
rule b3(X: actor): => athletics(X)    = 10
rule b4(X: actor): => sword_atk(X)    = 10
rule b5(X: actor): => bow_atk(X)      = 10
rule b6(X: actor): => firebolt_atk(X) = 10
rule b7(X: actor): => dagger_throw(X) = 10
rule b8(X: actor): => sword_dmg(X)    = 10
rule b9(X: actor): => firebolt_dmg(X) = 10

// ---- the modifiers: each written ONCE, selection by kind atoms -------------

// Bless: +d4 on every save and attack roll (the union, cross-arity ready)
rule bless(A: actor, V: value):
    bless_roll(V) & blessed(A) => V(A) = prior + roll(4)

// Halfling Luck: every d20 roll (the widest net; floor stands in for reroll)
rule halfling_luck(A: actor, V: value):
    d20(V) & lucky(A) => V(A) = max(prior, 2)

// the product cross-cuts: three axes over one attack space
rule rage(A: actor, V: value):
    attack(V, melee, weapon) & raging(A)  => V(A) = prior + 2
rule bracers_of_archery(A: actor, V: value):
    attack(V, ranged, weapon) & archer(A) => V(A) = prior + 2
rule war_caster_wand(A: actor, V: value):
    attack(V, _, spell) & warcaster(A)    => V(A) = prior + 1

// a brutal-damage ward: caps nonmagical physical damage (see gap #144 —
// a real resistance HALVES, which needs the ordered non-commuting class)
rule brutal_ward(A: actor, V: value):
    brutal(V) & warded(A) => V(A) = min(prior, 8)

// Luck (a max layer) meets the add layers on six values — mixed classes
// demand explicit order (#94), and today that is one dotted line PER
// member pair (#145: `halfling_luck > bless` should say all of this):
halfling_luck.spell_save   > bless.spell_save
halfling_luck.con_save     > bless.con_save
halfling_luck.sword_atk    > bless.sword_atk
halfling_luck.sword_atk    > rage.sword_atk
halfling_luck.bow_atk      > bless.bow_atk
halfling_luck.bow_atk      > bracers_of_archery.bow_atk
halfling_luck.firebolt_atk > bless.firebolt_atk
halfling_luck.firebolt_atk > war_caster_wand.firebolt_atk
halfling_luck.dagger_throw > bless.dagger_throw
halfling_luck.dagger_throw > bracers_of_archery.dagger_throw

// ---- host-facing snapshots -------------------------------------------------

action snap(X: actor):
    causes ss(X)  := spell_save(X)   & cs(X)  := con_save(X)
         & ath(X) := athletics(X)    & swa(X) := sword_atk(X)
         & bwa(X) := bow_atk(X)      & fba(X) := firebolt_atk(X)
         & dta(X) := dagger_throw(X) & swd(X) := sword_dmg(X)
         & fbd(X) := firebolt_dmg(X)
action snap_ini(X: actor): causes ini(X) := initiative(X)

action  b(X: actor): causes blessed(X)
action  l(X: actor): causes lucky(X)
action  r(X: actor): causes raging(X)
action  a(X: actor): causes archer(X)
action wc(X: actor): causes warcaster(X)
action  w(X: actor): causes warded(X)

// ---- "module-shaped" extension: the bottom of the file adds one value -----
// One declaration + ONE membership fact, and initiative is a d20 (via the
// check route), so Halfling Luck — written far above, knowing nothing of
// initiative — covers it. Program union; a real module does exactly this
// from another file (M4).

value initiative(actor) : int
fact check(initiative, dex)
rule b10(X: actor): => initiative(X) = 1
