/* Interactive cellar demo: toggle base facts, watch judgments re-derive,
 * fire the force-door action through the step function. The renderer only
 * reads; all mutation goes through world_set (debug toggles) / world_step. */

#include "raylib.h"
#include "state/world.h"
#include "core/intern.h"

#include <string.h>

typedef struct {
    intern *sy;
    world *w;
    uint32_t poisoned, antidote, strong, closed, open;
    uint32_t weakened, can_force;
    uint32_t a_force;
    char status[256];
} demo;

static void demo_build(demo *d)
{
    d->sy = intern_new();
    d->w = world_new(d->sy);

    d->poisoned = intern_id(d->sy, "poisoned");
    d->antidote = intern_id(d->sy, "has_antidote");
    d->strong = intern_id(d->sy, "strong");
    d->closed = intern_id(d->sy, "door_closed");
    d->open = intern_id(d->sy, "door_open");
    d->weakened = intern_id(d->sy, "weakened");
    d->can_force = intern_id(d->sy, "can_force_door");
    d->a_force = intern_id(d->sy, "act_force_door");

    world_declare_fluent(d->w, d->poisoned);
    world_declare_fluent(d->w, d->antidote);
    world_declare_fluent(d->w, d->strong);
    world_declare_fluent(d->w, d->closed);
    world_declare_fluent(d->w, d->open);
    world_set(d->w, d->poisoned, true);
    world_set(d->w, d->strong, true);
    world_set(d->w, d->closed, true);

    dl_lit po = dl_pos(d->poisoned), an = dl_pos(d->antidote);
    world_add_rule(d->w, "poison_weakens", DL_DEFEASIBLE,
                   dl_pos(d->weakened), &po, 1);
    world_add_rule(d->w, "antidote_blocks", DL_DEFEATER,
                   dl_neg(d->weakened), &an, 1);
    dl_lit fb[] = { dl_pos(d->strong), dl_pos(d->closed) };
    int r_force = world_add_rule(d->w, "can_force", DL_DEFEASIBLE,
                                 dl_pos(d->can_force), fb, 2);
    dl_lit wk = dl_pos(d->weakened);
    int r_weak = world_add_rule(d->w, "too_weak", DL_DEFEASIBLE,
                                dl_neg(d->can_force), &wk, 1);
    world_add_sup(d->w, r_weak, r_force);

    step_cond fc[] = {
        { { d->can_force, false }, false },
        { { d->closed, false }, false },
    };
    dl_lit fe[] = { dl_pos(d->open), dl_neg(d->closed) };
    world_add_step_rule(d->w, "force_door", d->a_force, fc, 2, fe, 2);

    strcpy(d->status, "The cellar door is shut fast.");
}

static void demo_destroy(demo *d)
{
    world_free(d->w);
    intern_free(d->sy);
}

static const char *verdict_str(dl_verdict v)
{
    return v == DL_PROVED ? "PROVED" : v == DL_REFUTED ? "refuted" : "undecided";
}

static void draw_fluent(const demo *d, uint32_t f, int y)
{
    bool on = world_get(d->w, f);
    DrawText(intern_name(d->sy, f), 60, y, 20, on ? GREEN : GRAY);
    DrawText(on ? "true" : "false", 300, y, 20, on ? GREEN : GRAY);
}

static void draw_judgment(const demo *d, uint32_t j, int y)
{
    dl_verdict v = world_query(d->w, dl_pos(j));
    Color c = v == DL_PROVED ? SKYBLUE : DARKGRAY;
    DrawText(intern_name(d->sy, j), 60, y, 20, c);
    DrawText(verdict_str(v), 300, y, 20, c);
}

int main(void)
{
    demo d;
    demo_build(&d);

    InitWindow(900, 560, "infeasible - cellar demo");
    SetTargetFPS(60);

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_P))
            world_set(d.w, d.poisoned, !world_get(d.w, d.poisoned));
        if (IsKeyPressed(KEY_A))
            world_set(d.w, d.antidote, !world_get(d.w, d.antidote));
        if (IsKeyPressed(KEY_R)) {
            demo_destroy(&d);
            demo_build(&d);
        }
        if (IsKeyPressed(KEY_SPACE)) {
            bool could = world_query(d.w, dl_pos(d.can_force)) == DL_PROVED;
            char err[256];
            if (world_step(d.w, &d.a_force, 1, err, sizeof err) != 0)
                snprintf(d.status, sizeof d.status, "step error: %s", err);
            else if (world_get(d.w, d.open))
                strcpy(d.status, "The door gives way in a shower of splinters!");
            else if (!could)
                strcpy(d.status, "You brace yourself - and your poisoned limbs betray you.");
        }
        if (IsKeyPressed(KEY_W))
            world_why(d.w, dl_pos(d.can_force), stdout);

        BeginDrawing();
        ClearBackground((Color){ 24, 24, 32, 255 });
        DrawText("infeasible - defeasible cellar", 40, 24, 28, RAYWHITE);
        DrawText(d.status, 40, 64, 20, GOLD);

        DrawText("base facts (the only mutable state)", 40, 110, 18, LIGHTGRAY);
        draw_fluent(&d, d.poisoned, 140);
        draw_fluent(&d, d.antidote, 165);
        draw_fluent(&d, d.strong, 190);
        draw_fluent(&d, d.closed, 215);
        draw_fluent(&d, d.open, 240);

        DrawText("derived judgments (recomputed, never stored)", 40, 290, 18, LIGHTGRAY);
        draw_judgment(&d, d.weakened, 320);
        draw_judgment(&d, d.can_force, 345);

        DrawText("[P] toggle poison   [A] toggle antidote   [SPACE] force door",
                 40, 420, 18, LIGHTGRAY);
        DrawText("[W] print 'why can_force_door?' to stdout   [R] reset",
                 40, 445, 18, LIGHTGRAY);
        EndDrawing();
    }

    CloseWindow();
    demo_destroy(&d);
    return 0;
}
