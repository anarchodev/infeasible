// spellbook5e.story — 5e spells as DATA, via the set-quantified effect binder
// (DESIGN.md §13). Each spell's rule lives in the .story file, not in host code:
// `for each T: actor where <guard>: <effect> [when <cond>]` applies an effect to
// a computed set of targets, resolved at tick time. The host supplies the
// dynamic inputs an AoE needs — who is in the blast, who made their save —
// as ordinary actions/markers, keeping randomness above the core (I4, srd_probe
// P5). Compiles today; drive it from host code against world_*.

sort actor
enum school { evocation, enchantment, abjuration }   // §13 value domain (unused mechanic, shown)

entity (
    vera, thorn         : actor    // the party: a wizard and a fighter
    grik, gnok, gob     : actor    // a goblin cluster — the AoE fodder
)

state (
    hp(actor) : int in 0 .. 40
    foe(actor)
    ally(actor)
    clustered(actor)               // host-marked: inside the area this cast
    saved(actor)                   // host-marked: made its save this cast
    blessed(actor)
    outlined(actor)
    concentrating(actor)
)

init (
    hp(vera)=22  hp(thorn)=30  hp(grik)=7  hp(gnok)=7  hp(gob)=7
    foe(grik) foe(gnok) foe(gob)
    ally(vera) ally(thorn)
    clustered(grik) clustered(gnok) clustered(gob) clustered(thorn)   // thorn caught in the blast
)

// The host rolls each save (above the core, I4) and reports the outcome by
// taking this fixed-arity action for each creature that succeeds.
action pass_save(C: actor, T: actor): causes saved(T)

// FIREBALL — "each creature in a 20-ft sphere makes a DEX save: 8d6 fire, or
// half on a success." The target set is host-marked (`clustered`); friendly
// fire falls out for free — thorn is in the blast too, no special case.
action fireball(C: actor):
    causes for each T: actor where clustered(T): {
        hp(T) -= 8 when ~saved(T) ,      // failed the save: full damage
        hp(T) -= 4 when saved(T)         // made the save: half
    }

// FAERIE FIRE (concentration) — outline every foe in the area.
action faerie_fire(C: actor):
    causes concentrating(C)
         & for each T: actor where foe(T) & clustered(T): outlined(T)

// BLESS (concentration) — bless every ally; other rules key off the mark.
action bless(C: actor):
    causes concentrating(C)
         & for each T: actor where ally(T): blessed(T)

// derived reads
rule down(X: actor):     hp(X) <= 0        -> down(X)
rule easy_mark(X: actor): outlined(X)      => attacked_at_adv(X)   // outlined = advantage
