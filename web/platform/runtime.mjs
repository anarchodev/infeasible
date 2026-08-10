// runtime.mjs — the loop that joins a cart, a platform backend and a world
// (DESIGN.md §11 M2, §12).
//
// Two clocks, deliberately separated:
//
//   TICKS are the world's. One tick = one `world_step`, and the only things
//   that reach it are the action set the cart returned and the input snapshot
//   taken at that tick's boundary. The tick count, the action log and the
//   resulting state are a pure function of each other (I4), which is what
//   makes a save an action log and a replay exact.
//
//   FRAMES are the renderer's. A frame may repaint several times per tick or
//   skip ticks entirely; movement interpolation, easing and cue animations are
//   presentation state and live above the line. Nothing a frame does can
//   change a fact.
//
// The wall clock is therefore allowed to decide HOW MANY ticks elapse (that is
// what the accumulator does) and never WHAT a tick contains. Under lockstep
// the accumulator is replaced by the network's tick schedule and nothing else
// about a cart changes.

/**
 * @param {object} o
 * @param {object} o.platform  from createPlatform(backend)
 * @param {object} o.backend   the same backend (for present/resize)
 * @param {object} o.cart      { tick(ctx), draw(ctx), init?(ctx) }
 * @param {object} o.world     the generated typed binding's session
 * @param {number} [o.tps]     world ticks per second
 */
export function createRuntime({ platform, backend, cart, world, tps = 20 }) {
  const ctx = {
    ...platform.cart,
    world,
    /** ticks elapsed — the cart's only legitimate notion of time */
    tick: 0,
    /** what the last step handed back (§12's three streams). Cleared every
     *  tick, exactly like the engine's own per-tick buffers, so a cue cannot
     *  be rendered twice by a frame that happens to run late. */
    step: { emits: [], changed: [], edges: [], rejected: null },
    /** the action log — a save is (engine-hash, game-hash, THIS) */
    log: [],
  };

  // Assets are registered before the cart runs: loading is above the durability
  // line (a backend decides what an atlas IS), naming one is below it.
  for (const [name, def] of Object.entries(cart.sheets ?? {}))
    backend.defineSheet(name, def);

  cart.init?.(ctx);

  /** Advance the world exactly one tick. */
  function step() {
    platform.sampleInput();
    ctx.step = { emits: [], changed: [], edges: [], rejected: null };

    const orders = cart.tick(ctx);
    const items = orders && orders.length ? orders : null;
    if (items) {
      try {
        world.step(orders);
        ctx.log.push(orders.items.map((a) => a.term));
        ctx.step.emits = world.emits();
        ctx.step.changed = world.changed();
        ctx.step.edges = world.edges();
      } catch (e) {
        // A rejected step is a contested world or a protocol violation — the
        // world did not move, so neither does the log. Surfaced to the cart
        // rather than thrown past it: a client that cannot render "that did
        // not happen" is a client that hides authoring errors.
        ctx.step.rejected = e.message;
      }
    }
    ctx.tick++;
    cart.after?.(ctx);
  }

  /** Repaint once. Never mutates the world. */
  function render() {
    backend.beginFrame?.();
    cart.draw(ctx);
    backend.present?.();
  }

  return {
    ctx,
    step,
    render,
    /** Run n ticks with a repaint after each — the headless/test driver. */
    advance(n = 1) { for (let i = 0; i < n; i++) { step(); render(); } },
    /** The browser loop: rAF for frames, an accumulator for ticks. */
    start() {
      const dt = 1000 / tps;
      let last = null, acc = 0, stop = false;
      const frame = (now) => {
        if (stop) return;
        if (last === null) last = now;
        acc += Math.min(250, now - last);        // a long pause is not 200 ticks
        last = now;
        while (acc >= dt) { acc -= dt; step(); }
        render();
        requestAnimationFrame(frame);
      };
      requestAnimationFrame(frame);
      return () => { stop = true; };
    },
  };
}
