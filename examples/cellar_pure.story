// cellar_pure.story — the cellar with NO HOST CODE (DESIGN.md §12, the
// infeasible cart).
//
// `cellar_play.story` is a world; a JS cart decides how it looks. This file is
// the same world plus its PRESENTATION, so that a generic renderer — one that
// has never heard of cellars, doors or torches — can draw it from the
// conclusions alone. The falsifiable test §12 asks for: how many rules does it
// cost, and what cannot be said?
//
// The ontology is ordinary vocabulary, not new syntax. Every predicate below
// is a plain judgment and every coordinate a plain numeric fluent, so the
// renderer finds them through the §6.3 interface artifact and asks the engine
// which hold. Nothing here required a compiler change.
//
//   BLESSED VOCABULARY the renderer reads
//     ax/ay/aw/ah(anchor)     geometry, as a numeric state table
//     panel(anchor, style)    a box is drawn there
//     caption(anchor, word)   text — the ATOM IS THE LABEL (`w_the_cellar`)
//     shows/prop_shows(E, sprite)   that thing draws as that sprite
//     in_anchor/prop_in(E, anchor)  ...packed into that region, declaration order
//     here(actor, room)       where an actor is — also fills a room-sorted
//                             action parameter when a command is submitted
//     held(item, actor)       ...or carried beside its holder
//     shaded(anchor)          the composite op over the region
//     gauge(anchor, actor)    a bar, filled by the blessed hp / hp_max
//     offers(actor, cmd)      a command, in cmd declaration order
//     blocked(actor, cmd)     ...offered but refused, and `why` explains it
//     picked(actor)           the selected actor
//     cue_sound(cue, sound)   an emission plays a sound
//     cue_word(cue, word)     ...and floats a word
//
//   ENUM ORDER IS MEANING: a `sprite` member's position is its atlas index. Declaration order is already the
//   engine's tie-break for emissions (I4); the renderer inherits it, so two
//   clients cannot disagree about what is on top.

sort actor, item
enum room   { cellar, hall, vault }

// ---- the presentation vocabulary -------------------------------------------

enum sprite { s_hero, s_guard, s_key, s_torch, s_flask }
enum style  { st_room, st_bar, st_title, st_button, st_button_off }
enum anchor {
    a_title, a_cellar, a_hall, a_vault, a_door,
    a_bar,   a_who,    a_note, a_menu,  a_status
}
// The atom is the label, so UI copy is vocabulary. That is the constraint the
// no-string-type decision buys: one source of truth, no punctuation, one
// language, and entity names duplicated here when they must be *said*.
enum word {
    w_the_cellar, w_cellar, w_hall, w_vault,
    w_hero, w_guard, w_locked, w_jammed, w_open,
    w_weakened, w_oof, w_aah, w_got_it, w_tab_switches
}
enum cue { q_footstep, q_pickup, q_clunk, q_heave, q_sip }
enum sound { snd_step, snd_chime, snd_lock, snd_thud, snd_gulp }

entity (
    hero, guard                : actor
    rusty_key, torch, antidote : item
)

state (
    at(actor)          : room
    on_floor(item, room)
    holding(actor, item)
    door               : { locked, jammed, open }
    poisoned(actor)
    hp(actor)          : int in 0 .. 12
    hp_max(actor)      : int in 0 .. 99
    selected(actor)                    // per-viewer, and it shows: see the note
    // The screen gate. A rule needs a body, and "this is the game screen" is
    // the honest body for the parts of the frame that are simply always there
    // — the seam a title screen or a menu would later hang off.
    showing
    // Geometry is a numeric table, not derived. Positions are store-backed
    // fluents (§5.6), so a coordinate costs nothing to represent.
    ax(anchor) : int in 0 .. 640
    ay(anchor) : int in 0 .. 360
    aw(anchor) : int in 0 .. 640
    ah(anchor) : int in 0 .. 360
)

emit (
    footstep(actor)  pickup(actor, item)  clunk  heave(actor)  sip(actor)
)

init (
    at(hero)  = cellar
    at(guard) = hall
    on_floor(rusty_key, cellar)
    on_floor(torch,     hall)
    on_floor(antidote,  vault)
    door = locked
    poisoned(hero)
    hp(hero)  = 12   hp_max(hero)  = 12
    hp(guard) = 12   hp_max(guard) = 12
    selected(hero)
    showing

    // the layout, once
    ax(a_title)  = 0    ay(a_title)  = 0    aw(a_title)  = 640  ah(a_title)  = 14
    ax(a_cellar) = 8    ay(a_cellar) = 36   aw(a_cellar) = 192  ah(a_cellar) = 176
    ax(a_hall)   = 224  ay(a_hall)   = 36   aw(a_hall)   = 192  ah(a_hall)   = 176
    ax(a_vault)  = 440  ay(a_vault)  = 36   aw(a_vault)  = 192  ah(a_vault)  = 176
    ax(a_door)   = 418  ay(a_door)   = 120  aw(a_door)   = 20   ah(a_door)   = 8
    ax(a_bar)    = 0    ay(a_bar)    = 224  aw(a_bar)    = 640  ah(a_bar)    = 1
    ax(a_who)    = 0    ay(a_who)    = 229  aw(a_who)    = 200  ah(a_who)    = 12
    ax(a_menu)   = 8    ay(a_menu)   = 244  aw(a_menu)   = 176  ah(a_menu)   = 12
    ax(a_status) = 216  ay(a_status) = 229  aw(a_status) = 200  ah(a_status) = 8
    ax(a_note)   = 208  ay(a_note)   = 253  aw(a_note)   = 400  ah(a_note)   = 12
)

// ---- the world: judgments, unchanged in spirit from cellar_play ------------

rule poison_weakens(X: actor): poisoned(X) => weakened(X) unless holding(X, antidote)

rule here_cellar(X: actor): at(X) = cellar => here(X, cellar)
rule here_hall(X: actor):   at(X) = hall   => here(X, hall)
rule here_vault(X: actor):  at(X) = vault  => here(X, vault)

rule carried_in(Y: actor, R: room): here(Y, R) & holding(Y, torch) => torch_in(R)
rule lying_in(R: room):             on_floor(torch, R)             => torch_in(R)

rule gloomy(X: actor):       at(X) = cellar => in_dark(X)
rule bright_hall(X: actor):  at(X) = hall   => ~in_dark(X)
rule bright_vault(X: actor): at(X) = vault  => ~in_dark(X)
rule torchlit(X: actor, R: room): here(X, R) & torch_in(R) => ~in_dark(X)
torchlit > gloomy

rule may_enter(X: actor):  at(X) = hall                  => can_enter_vault(X)
rule locked_out(X: actor): at(X) = hall & door = locked  => ~can_enter_vault(X)
rule jammed_shut(X: actor):at(X) = hall & door = jammed  => ~can_enter_vault(X)
locked_out  > may_enter
jammed_shut > may_enter

rule can_unlock(X: actor):
    at(X) = hall & door = locked & holding(X, rusty_key) => can_unlock_door(X)

rule can_force(X: actor): at(X) = hall & door = jammed => can_force_door(X)
rule too_weak(X: actor):  weakened(X)                  => ~can_force_door(X)
too_weak > can_force

rule downed(X: actor): hp(X) <= 0 -> down(X)

// ---- actions ----------------------------------------------------------------
//
// Commands are per-verb-object here rather than parameterized, because a menu
// entry IS a concrete order. That is the shape a point-and-click wants anyway,
// and it costs six actions to buy a command vocabulary the renderer can name.

action go_hall(X: actor):     requires at(X) = cellar  causes at(X) = hall   & footstep(X)
action go_cellar(X: actor):   requires at(X) = hall    causes at(X) = cellar & footstep(X)
action enter_vault(X: actor): requires can_enter_vault(X) causes at(X) = vault & footstep(X)
action leave_vault(X: actor): requires at(X) = vault   causes at(X) = hall   & footstep(X)

action take_torch(X: actor, R: room):
    requires here(X, R) & on_floor(torch, R) & ~in_dark(X)
    causes   holding(X, torch) & ~on_floor(torch, R) & pickup(X, torch)
action take_key(X: actor, R: room):
    requires here(X, R) & on_floor(rusty_key, R) & ~in_dark(X)
    causes   holding(X, rusty_key) & ~on_floor(rusty_key, R) & pickup(X, rusty_key)
action take_antidote(X: actor, R: room):
    requires here(X, R) & on_floor(antidote, R) & ~in_dark(X)
    causes   holding(X, antidote) & ~on_floor(antidote, R) & pickup(X, antidote)

action drop_torch(X: actor, R: room):
    requires here(X, R) & holding(X, torch)
    causes   ~holding(X, torch) & on_floor(torch, R)
action drop_key(X: actor, R: room):
    requires here(X, R) & holding(X, rusty_key)
    causes   ~holding(X, rusty_key) & on_floor(rusty_key, R)
action drop_antidote(X: actor, R: room):
    requires here(X, R) & holding(X, antidote)
    causes   ~holding(X, antidote) & on_floor(antidote, R)

action unlock(X: actor):     requires can_unlock_door(X) causes door = jammed & clunk
action force_door(X: actor): requires can_force_door(X)  causes door = open & hp(X) -= 2 & heave(X)
action drink(X: actor):      requires holding(X, antidote) & poisoned(X)
                             causes   ~poisoned(X) & sip(X)

// Selection is an ACTION, and the cost is worth noticing: per-viewer state in
// the SHARED world means the action log records who you were looking at, two
// players clicking different actors contest one fluent, and "select" needs one
// concrete action per actor because a set-one-clear-the-rest is not a thing a
// parameterized action can say. §5.5's private-vocabulary scopes are where this
// belongs; until they exist a pure cart pays for it here, in the world.
action pick_hero:  causes selected(hero)  & ~selected(guard)
action pick_guard: causes selected(guard) & ~selected(hero)

exclusive pick_hero, pick_guard

exclusive go_hall(X), go_cellar(X), enter_vault(X), leave_vault(X),
          unlock(X), force_door(X), drink(X),
          take_torch(X, _), take_key(X, _), take_antidote(X, _),
          drop_torch(X, _), drop_key(X, _), drop_antidote(X, _)
exclusive take_torch(_, R), drop_torch(_, R)
exclusive take_key(_, R), drop_key(_, R)
exclusive take_antidote(_, R), drop_antidote(_, R)
exclusive unlock(_), force_door(_)

// ---- PRESENTATION: everything below here is what a cart used to be ----------

// the frame
rule f_title: showing => panel(a_title, st_title)
rule f_cellar: showing => panel(a_cellar, st_room)
rule f_hall: showing => panel(a_hall, st_room)
rule f_vault: showing => panel(a_vault, st_room)
rule f_bar: showing => panel(a_bar, st_bar)

rule cap_title: showing => caption(a_title, w_the_cellar)
rule cap_cellar: showing => caption(a_cellar, w_cellar)
rule cap_hall: showing => caption(a_hall, w_hall)
rule cap_vault: showing => caption(a_vault, w_vault)

// who is where — one rule per room, because an anchor is an atom
rule pin_c(X: actor): at(X) = cellar => in_anchor(X, a_cellar)
rule pin_h(X: actor): at(X) = hall   => in_anchor(X, a_hall)
rule pin_v(X: actor): at(X) = vault  => in_anchor(X, a_vault)
rule item_c(T: item): on_floor(T, cellar) => prop_in(T, a_cellar)
rule item_h(T: item): on_floor(T, hall)   => prop_in(T, a_hall)
rule item_v(T: item): on_floor(T, vault)  => prop_in(T, a_vault)
rule carried(X: actor, T: item): holding(X, T) => held(T, X)

// what each thing looks like
rule spr_hero: showing => shows(hero, s_hero)
rule spr_guard: showing => shows(guard, s_guard)
rule spr_key: showing => prop_shows(rusty_key, s_key)
rule spr_torch: showing => prop_shows(torch, s_torch)
rule spr_flask: showing => prop_shows(antidote, s_flask)

// the dark, as a region rather than a person: the renderer shades an anchor
rule fog_c(X: actor): at(X) = cellar & in_dark(X) => shaded(a_cellar)
rule fog_h(X: actor): at(X) = hall   & in_dark(X) => shaded(a_hall)
rule fog_v(X: actor): at(X) = vault  & in_dark(X) => shaded(a_vault)

// the door reads its own state
rule door_l: door = locked => caption(a_door, w_locked)
rule door_j: door = jammed => caption(a_door, w_jammed)
rule door_o: door = open   => caption(a_door, w_open)

// status
rule g_hero: showing => gauge(a_status, hero)
rule g_guard: showing => gauge(a_status, guard)
value ( gauge_value(actor) : int   gauge_max(actor) : int )
rule gv(X: actor): => gauge_value(X) = hp(X)
rule gm(X: actor): => gauge_max(X)   = 12
rule weak_note(X: actor): weakened(X) & selected(X) => caption(a_note, w_weakened)
rule who_h: selected(hero)  => caption(a_who, w_hero)
rule who_g: selected(guard) => caption(a_who, w_guard)
rule sel(X: actor): selected(X) => picked(X)

// ---- the menu is not here any more ----------------------------------------
//
// It used to be a `cmd` enum mirroring the actions plus ~20 rules restating
// each action's `requires` as an `offers`/`blocked` judgment — the rule written
// twice, free to drift. The engine answers it directly now
// (`world_actions` / `world_action_status_of` / `world_action_blockers`), so
// the vocabulary and the rules are simply gone. What the story still says is
// which actions a client should SURFACE, and it says it by declaring them.

// ---- cues: the sound and the floating word, declared not coded -------------

rule cue_step: showing => cue_sound(q_footstep, snd_step)
rule cue_pick: showing => cue_sound(q_pickup, snd_chime)
rule cue_lock: showing => cue_sound(q_clunk, snd_lock)
rule cue_heave: showing => cue_sound(q_heave, snd_thud)
rule cue_sip: showing => cue_sound(q_sip, snd_gulp)
rule word_heave: showing => cue_word(q_heave, w_oof)
rule word_sip: showing => cue_word(q_sip, w_aah)
rule word_pick: showing => cue_word(q_pickup, w_got_it)
