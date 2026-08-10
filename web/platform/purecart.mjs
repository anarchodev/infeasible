// purecart.mjs — the cart that isn't one (DESIGN.md §12, the infeasible cart).
//
// `createRuntime` wants a cart: `init/tick/draw`. This supplies one that
// contains no game. It reads input, asks the scene what is under the pointer,
// submits the command the world offered, and draws what the world concluded.
// Point it at a different `.story` and it is a different game with no edit
// here — which is the whole claim, and the reason this file is short.
//
// What is left in JS after this is exactly the residue worth naming: the loop,
// the layout arithmetic inside the scene, and the ASSETS. Assets are not code —
// a sprite atlas is pixels, and pixels are not expressible as rules for the
// same reason a `.png` is not.

import { createScene } from './scene.mjs';

const verb = (term) => term.replace(/\(.*/, '');

/**
 * @param {object} o
 * @param {object} o.world   the generated binding session
 * @param {object} o.iface   its IFACE export
 * @param {object} o.doms    { ...SORTS, ...ENUMS }
 * @param {object} o.sheets  the atlases, by name — assets, not logic
 */
export function pureCart({ world, iface, doms, sheets, resolution = [640, 360] }) {
  const scene = createScene(world, iface, doms);

  return {
    resolution,
    sheets,

    init(ctx) {
      this.scene = scene;
      this.why = '';
      this.fx = [];
      ctx.why = '';
      scene.rebuild();
    },

    tick(ctx) {
      const { input } = ctx;
      if (!input.pressed(0)) return null;
      const target = scene.hit(input.pointer());
      if (!target) { this.why = ctx.why = ''; return null; }

      if (target.kind === 'entity') {
        this.why = ctx.why = '';
        const act = scene.pick(target.entity);
        return act ? ctx.world.actions().add(act) : null;
      }
      if (target.ok) {
        this.why = ctx.why = '';
        // the ground action IS the term the engine named; nothing to bind
        return ctx.world.actions().add({ action: verb(target.term),
                                         args: {}, term: target.term });
      }
      // A refused command explains itself, and the literal to explain is the
      // GUARD the engine says refused it — the world's own argument, not a
      // message this file invented or a judgment the story wrote to mirror it.
      this.why = ctx.why = target.blockers?.length
        ? ctx.world.why(target.blockers[0].atom, target.blockers[0].neg) : '';
      return null;
    },

    after(ctx) {
      // Cues are a LOOKUP, not a handler per event: the story already said
      // which sound a cue plays and which word it floats, so this reads the
      // table it declared. An emission `heave(guard)` is the cue `q_heave`.
      const sounds = scene.pairs('cue_sound'), words = scene.pairs('cue_word');
      for (const emitted of ctx.step.emits) {
        const q = `q_${emitted.replace(/\(.*/, '')}`;
        for (const [c, snd] of sounds) if (c === q) ctx.audio.sound(snd, 0.6);
        for (const [c, wrd] of words) if (c === q)
          this.fx.push({ text: scene.say(wrd), life: 40 });
      }
      scene.rebuild();
    },

    draw(ctx) {
      ctx.why = this.why;
      scene.draw(ctx, 'main');
      let i = 0;
      for (const f of this.fx) {
        ctx.draw.print(f.text, 300, 200 - (40 - f.life) / 2 - i * 10, 6, { big: true });
        f.life--; i++;
      }
      this.fx = this.fx.filter((f) => f.life > 0);
    },
  };
}
