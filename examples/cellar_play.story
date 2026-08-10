// cellar_play.story — the world behind the browser cart (DESIGN.md §11 M2).
//
// This is the cellar as something you can actually play: three rooms, two
// actors, a locked door, a torch, a key and an antidote. It exists to be
// driven by `web/carts/cellar.mjs` through the generated typed binding, so it
// deliberately uses every channel the presentation interface has — judgments
// to grey out a button, burst cues to fire a sound, a numeric fluent whose
// commit receipt is worth showing, and `exclusive` groups that encode the
// game's protocol rather than leaving it to host discipline.
//
// Nothing here is new engine surface. It is the M1 language pointed at a
// playable shape, which is the point: a cart is `.story` plus host JS.

sort actor, item
enum room { cellar, hall, vault }

entity (
    hero, guard                : actor
    rusty_key, torch, antidote : item
)

state (
    at(actor)          : room       // multi-valued: exactly one room, always
    on_floor(item, room)
    holding(actor, item)
    // Three states, and the middle one is the puzzle: turning the lock does
    // not open the door, it only stops the lock from being the reason you
    // cannot. A jammed door still has to be shouldered — which is why solving
    // this needs two actors, one with the key and one with the shoulders.
    door               : { locked, jammed, open }
    poisoned(actor)
    hp(actor)          : int in 0 .. 12
)

// The write-only half of the interface (#11): what the renderer hears from a
// step that no fluent records.
emit (
    footstep(actor)
    pickup(actor, item)
    clunk                      // the lock turning
    heave(actor)               // a shoulder against the door
    sip(actor)
)

init (
    at(hero)  = cellar
    at(guard) = hall
    on_floor(rusty_key, cellar)
    on_floor(torch,     cellar)
    on_floor(antidote,  vault)
    door = locked
    poisoned(hero)
    hp(hero)  = 12
    hp(guard) = 12
)

// ---- judgments: what the UI asks before it offers a button ----------------

// Poison weakens you, unless you are carrying the antidote.
rule poison_weakens(X: actor): poisoned(X) => weakened(X) unless holding(X, antidote)

// The vault door is the whole game, so it is stated the way the engine likes
// arguments stated: a norm, and two exceptions that beat it. `why
// can_enter_vault(hero)` then reads back as the reason the way is shut, which
// is what the cart puts under a greyed-out button.
rule may_enter(X: actor):  at(X) = hall                  => can_enter_vault(X)
rule locked_out(X: actor): at(X) = hall & door = locked  => ~can_enter_vault(X)
rule jammed_shut(X: actor):at(X) = hall & door = jammed  => ~can_enter_vault(X)
locked_out > may_enter
jammed_shut > may_enter

rule can_unlock(X: actor):
    at(X) = hall & door = locked & holding(X, rusty_key) => can_unlock_door(X)

// Strong shoulders free a jammed door — but not a weakened one's.
rule can_force(X: actor): at(X) = hall & door = jammed => can_force_door(X)
rule too_weak(X: actor):  weakened(X)                  => ~can_force_door(X)
too_weak > can_force

// The cellar is dark: a judgment the *renderer* reads, not the rules.
// Presentation asking the world a question is the ordinary case — there is no
// second, presentation-side notion of "dark".
//
// Light belongs to the ROOM, not to whoever happens to be carrying it, which
// is why the exception quantifies over a SECOND actor: a torch in the cellar
// lights it for everyone standing there. `Y = X` is the case where you are
// carrying it yourself, so the one rule covers both. Writing this as
// `~holding(X, torch)` — the obvious version — makes darkness a fact about a
// person rather than about a place, and leaves you standing in the dark beside
// a friend holding a lit torch.
rule gloomy(X: actor): at(X) = cellar => in_dark(X)
rule torchlit(X: actor, Y: actor, R: room):
    here(X, R) & here(Y, R) & holding(Y, torch) => ~in_dark(X)
torchlit > gloomy

rule downed(X: actor): hp(X) <= 0 -> down(X)

// A multi-valued fluent answers "which value?", and a rule turns that into the
// variable-indexed reading "is X here?" that the item actions need — an
// effect or guard takes a value, never a variable, so the bridge is a
// judgment. Three rules, and `take`/`drop` become room-generic.
rule here_cellar(X: actor): at(X) = cellar => here(X, cellar)
rule here_hall(X: actor):   at(X) = hall   => here(X, hall)
rule here_vault(X: actor):  at(X) = vault  => here(X, vault)

// ---- actions: the only mutation -------------------------------------------
//
// Four rooms' worth of passage is four actions rather than one `go(X, T)`,
// because a multi-valued effect takes a value, not a variable — the map is
// small enough that enumerating it costs nothing and reads better than a
// generic mover would.

action go_hall(X: actor):
    requires at(X) = cellar
    causes   at(X) = hall & footstep(X)

action go_cellar(X: actor):
    requires at(X) = hall
    causes   at(X) = cellar & footstep(X)

action enter_vault(X: actor):
    requires can_enter_vault(X)
    causes   at(X) = vault & footstep(X)

action leave_vault(X: actor):
    requires at(X) = vault
    causes   at(X) = hall & footstep(X)

action take(X: actor, T: item, R: room):
    requires here(X, R) & on_floor(T, R)
    causes   holding(X, T) & ~on_floor(T, R) & pickup(X, T)

action drop(X: actor, T: item, R: room):
    requires here(X, R) & holding(X, T)
    causes   ~holding(X, T) & on_floor(T, R)

action unlock(X: actor):
    requires can_unlock_door(X)
    causes   door = jammed & clunk

// Forcing the door costs you: the numeric fluent whose receipt the cart shows.
action force_door(X: actor):
    requires can_force_door(X)
    causes   door = open & hp(X) -= 2 & heave(X)

action drink(X: actor):
    requires holding(X, antidote) & poisoned(X)
    causes   ~poisoned(X) & sip(X)

// ---- the protocol, declared rather than merely observed (#159) -------------
//
// Three groups, each discharging a different way two orders could contest one
// fluent in one tick. The generated binding refuses the second order at ADD
// time, so a cart's click handler cannot build an illegal tick at all.

// One order per actor per tick — the turn protocol, keyed on the actor.
exclusive go_hall(X), go_cellar(X), enter_vault(X), leave_vault(X),
          take(X, _, _), drop(X, _, _), unlock(X), force_door(X), drink(X)

// One pair of hands per item per room: two actors reaching for the same key
// contest `on_floor`, and no per-actor key can see that.
exclusive take(_, T, R), drop(_, T, R)

// The door is global, so its group takes no key at all: any unlock and any
// force in one tick contest, whoever submits them.
exclusive unlock(_), force_door(_)
