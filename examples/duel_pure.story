// duel_pure.story — a SECOND skin for the generic renderer (DESIGN.md §12).
//
// `cellar_pure.story` proved a cart can be `.story` and assets. It could not
// say which of its vocabulary was universal and which was one game's shape,
// because one game never can. This is the second, chosen to be as far from the
// first as a game can be while still being drawn by the same renderer:
//
//   the cellar             this
//   ------------------     -----------------------------------------------
//   space is CONTAINMENT   there is no space at all — things live in ZONES
//   one actor acts         you act ON a target, so a command needs two things
//   numbers are scenery    numbers are the game: costs, damage, hit points
//   the menu is verbs      the menu is a HAND, and it changes every turn
//
// A duel: three fighters, a hand of ability cards, energy that refills. Play a
// card at a target, spend energy, end the turn and take what the foes give.
//
// What matters is not the game. It is which of the cellar's blessed vocabulary
// this needed unchanged, which it could not use at all, and what it had to
// invent — because the intersection is the part that deserves to be a
// primitive, and the rest was one game's furniture.

sort fighter, card
enum zone   { z_hand, z_spent }

// ---- presentation vocabulary, kept deliberately identical where it fits ----

enum sprite { s_you, s_gnoll, s_imp, s_strike, s_bolt, s_mend }
enum style  { st_room, st_bar, st_title, st_button, st_button_off }
enum anchor {
    a_title, a_foes, a_self, a_hand, a_bar, a_who, a_note, a_menu, a_status
}
enum word {
    w_the_duel, w_foes, w_you, w_hand, w_energy, w_downed, w_aimed, w_end_turn
}
enum cue   { q_hit, q_ward, q_heal }
enum sound { snd_thud, snd_ring, snd_chime }

entity (
    you, gnoll, imp                        : fighter
    edge_a, edge_b, spark, salve     : card
)

state (
    in_zone(card)      : zone
    is_strike(card)  is_bolt(card)  is_mend(card)
    hostile(fighter)
    alive(fighter)
    hp(fighter)        : int in 0 .. 20
    hp_max(fighter)    : int in 0 .. 20
    energy             : int in 0 .. 9

    // per-viewer, twice over: whose menu is showing, and what it is aimed at.
    // The cellar needed one of these; a game where you act ON something needs
    // two, and neither belongs in a shared world (§5.5).
    selected(fighter)
    aiming(fighter)

    showing
)

emit ( hit(fighter)  ward(fighter)  heal(fighter) )

init (
    alive(you)  alive(gnoll)  alive(imp)
    hostile(gnoll)  hostile(imp)
    hp(you)   = 20  hp_max(you)   = 20
    hp(gnoll) = 12  hp_max(gnoll) = 12
    hp(imp)   = 8   hp_max(imp)   = 8
    energy = 3
    in_zone(edge_a) = z_hand   is_strike(edge_a)
    in_zone(edge_b) = z_hand   is_strike(edge_b)
    in_zone(spark)   = z_hand   is_bolt(spark)
    in_zone(salve)   = z_hand   is_mend(salve)
    selected(you)
    aiming(gnoll)
    showing

)

// ---- the layout, as a VALUE TABLE rather than as state ----------------------
//
// These are constants: `init` set them and no action ever moves them. Left in
// the fact store they were configuration masquerading as state — carried in
// every save, and *removing* an anchor became a schema migration for a purely
// cosmetic edit. As value rows (#94) they leave the EDB entirely, so a layout
// change is a rule change, which §12 already says is free, and a year-old
// action log replays under a new skin and gets the same world with a new look.
value ( ax(anchor) : int   ay(anchor) : int
        aw(anchor) : int   ah(anchor) : int )

rule g_title_x: => ax(a_title) = 0
rule g_title_y: => ay(a_title) = 0
rule g_title_w: => aw(a_title) = 640
rule g_title_h: => ah(a_title) = 14
rule g_foes_x: => ax(a_foes) = 8
rule g_foes_y: => ay(a_foes) = 36
rule g_foes_w: => aw(a_foes) = 400
rule g_foes_h: => ah(a_foes) = 96
rule g_self_x: => ax(a_self) = 424
rule g_self_y: => ay(a_self) = 36
rule g_self_w: => aw(a_self) = 208
rule g_self_h: => ah(a_self) = 96
rule g_hand_x: => ax(a_hand) = 8
rule g_hand_y: => ay(a_hand) = 152
rule g_hand_w: => aw(a_hand) = 624
rule g_hand_h: => ah(a_hand) = 64
rule g_bar_x: => ax(a_bar) = 0
rule g_bar_y: => ay(a_bar) = 224
rule g_bar_w: => aw(a_bar) = 640
rule g_bar_h: => ah(a_bar) = 1
rule g_who_x: => ax(a_who) = 0
rule g_who_y: => ay(a_who) = 229
rule g_who_w: => aw(a_who) = 200
rule g_who_h: => ah(a_who) = 12
rule g_menu_x: => ax(a_menu) = 8
rule g_menu_y: => ay(a_menu) = 244
rule g_menu_w: => aw(a_menu) = 200
rule g_menu_h: => ah(a_menu) = 12
rule g_status_x: => ax(a_status) = 240
rule g_status_y: => ay(a_status) = 229
rule g_status_w: => aw(a_status) = 200
rule g_status_h: => ah(a_status) = 8
rule g_note_x: => ax(a_note) = 240
rule g_note_y: => ay(a_note) = 268
rule g_note_w: => aw(a_note) = 380
rule g_note_h: => ah(a_note) = 12

// ---- the world -------------------------------------------------------------

rule downed(F: fighter): hp(F) <= 0 -> down(F)
rule gone(F: fighter):   down(F) => ~alive(F)

rule playable(C: card):  in_zone(C) = z_hand => in_hand(C)
rule targetable(F: fighter): hostile(F) & alive(F) => enemy(F)
rule friendly(F: fighter):  ~hostile(F) & alive(F) => ally(F)

rule aim(F: fighter): aiming(F) & alive(F) => target(F)

// SUBJECT and OBJECT are different selections, and a game where you act ON
// something needs both. The cellar needed only the first, which is why one
// game could not have told us.
rule subject(F: fighter): selected(F) => picked(F)
rule object(F: fighter):  target(F)   => aimed(F)

// ---- actions: a card is played AT something --------------------------------

action strike(C: card, T: fighter):
    requires in_hand(C) & is_strike(C) & enemy(T) & energy >= 1
    causes   in_zone(C) = z_spent & energy -= 1 & hp(T) -= 4 & hit(T)

action bolt(C: card, T: fighter):
    requires in_hand(C) & is_bolt(C) & enemy(T) & energy >= 2
    causes   in_zone(C) = z_spent & energy -= 2 & hp(T) -= 7 & hit(T)

action mend(C: card, T: fighter):
    requires in_hand(C) & is_mend(C) & ally(T) & energy >= 1
    causes   in_zone(C) = z_spent & energy -= 1 & hp(T) += 5 & heal(T)

// Ending the turn refills, returns every spent card, and takes what the foes
// give — a set-quantified effect over whoever is still standing.
action end_turn:
    causes   energy := 3
           & for each C: card where in_zone(C) = z_spent: in_zone(C) = z_hand
           & for each F: fighter where hostile(F) & alive(F): hp(you) -= 3
           & ward(you)

// "Select one, clear the rest" has no expression — a parameterized action
// cannot say "the others" — so aiming costs one concrete action per fighter,
// exactly as selection cost the cellar one per actor. Two games, one wall.
action aim_gnoll: requires alive(gnoll) causes aiming(gnoll) & ~aiming(imp) & ~aiming(you)
action aim_imp:   requires alive(imp)   causes aiming(imp)   & ~aiming(gnoll) & ~aiming(you)
action aim_you:   requires alive(you)   causes aiming(you)   & ~aiming(gnoll) & ~aiming(imp)

exclusive strike(_, _), bolt(_, _), mend(_, _), end_turn
exclusive aim_gnoll, aim_imp, aim_you

// ---- PRESENTATION ----------------------------------------------------------

rule f_title: showing => panel(a_title, st_title)
rule f_foes:  showing => panel(a_foes, st_room)
rule f_self:  showing => panel(a_self, st_room)
rule f_hand:  showing => panel(a_hand, st_room)
rule f_bar:   showing => panel(a_bar, st_bar)

rule cap_title: showing => caption(a_title, w_the_duel)
rule cap_foes:  showing => caption(a_foes, w_foes)
rule cap_self:  showing => caption(a_self, w_you)
rule cap_hand:  showing => caption(a_hand, w_hand)

rule spr_you:   showing => shows(you, s_you)
rule spr_gnoll: showing => shows(gnoll, s_gnoll)
rule spr_imp:   showing => shows(imp, s_imp)
rule spr_str(C: card): is_strike(C) => prop_shows(C, s_strike)
rule spr_blt(C: card): is_bolt(C)   => prop_shows(C, s_bolt)
rule spr_mnd(C: card): is_mend(C)   => prop_shows(C, s_mend)

// no containment: a fighter's place is a fact about which side it is on
rule pin_foe(F: fighter):  hostile(F) & alive(F)  => in_anchor(F, a_foes)
rule pin_you(F: fighter): ~hostile(F) & alive(F)  => in_anchor(F, a_self)
rule pin_card(C: card):    in_zone(C) = z_hand    => prop_in(C, a_hand)

rule g_you(F: fighter):   ~hostile(F) => gauge(a_status, F)
rule g_foe(F: fighter):    hostile(F) => gauge(a_status, F)

// THE NUMBERS ARE THE STORY'S, not the renderer's. A gauge used to read a
// fluent the renderer knew by name, which froze one source, one maximum and
// one colour rule into the widget. These are ordinary derived values (#82) —
// so a bar can show hp, or hp plus a ward, or anything else — and the colour
// is an ordinary judgment, which is the sort of thing defeasible logic should
// be deciding.
value ( gauge_value(fighter) : int   gauge_max(fighter) : int )
rule gv(F: fighter): => gauge_value(F) = hp(F)
rule gm(F: fighter): => gauge_max(F)   = hp_max(F)
rule glow(F: fighter): hp(F) * 2 <= hp_max(F) => gauge_low(F)

rule note_down(F: fighter): down(F) => caption(a_note, w_downed)

rule cue_hit:  showing => cue_sound(q_hit, snd_thud)
rule cue_ward: showing => cue_sound(q_ward, snd_ring)
rule cue_heal: showing => cue_sound(q_heal, snd_chime)
