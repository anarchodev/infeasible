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
/** The LIVE source: what the cart proposed is what happens. */
export const live = () => ({ orders: (proposed) => proposed, done: () => false });

/** The REPLAY source: the log decides, and the cart's proposal is discarded.
 *  A log entry is a list of ground action terms — the save format — so replay
 *  needs no binding-time argument structure and cannot re-run protocol checks
 *  that were already passed when the orders were first collected. */
export const replay = (log) => {
  let i = 0;
  return {
    orders: () => (i < log.length ? { terms: log[i++] } : null),
    done: () => i >= log.length,
  };
};

/**
 * @param {object} o
 * @param {object} [o.source]    an action source: live (default) or replay(log)
 * @param {object} [o.identity]  { story, game } — the save's compatibility key
 */
export function createRuntime({ platform, backend, cart, world, tps = 20,
                               source = live(), identity = {} }) {
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

    // The cart PROPOSES; the source DISPOSES. Live, that is the same thing.
    // Replaying, the log decides and the proposal is thrown away — which is
    // what makes a save loadable rather than merely recorded, and it is the
    // seat a network source takes for lockstep.
    const proposed = cart.tick(ctx);
    const orders = source.orders(proposed, ctx.tick);
    const terms = orders?.items ? orders.items.map((a) => a.term) : orders?.terms;
    if (terms && terms.length) {
      try {
        world.stepTerms(terms);
        ctx.log.push(terms);
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

  /** A save is (engine-hash, game-hash, action-log) — §12. Not a state dump:
   *  state written outside the log is state replay cannot reproduce, which
   *  forfeits shareable playthroughs, branching and time travel in one move. */
  function save() {
    return { format: 1, story: identity.story ?? null,
             game: identity.game ?? null, engine: identity.engine ?? null,
             log: ctx.log.map((t) => [...t]) };
  }

  /** Replay a save into THIS world, which must be freshly opened: the log is
   *  from genesis, so loading into a played world would append a second
   *  history to the first. Refuses a save from a different story rather than
   *  replaying orders whose atoms mean something else now. */
  function load(saved) {
    if (saved.game && identity.game && saved.game !== identity.game)
      throw new Error('this save is from a different version of ' +
        (saved.story ?? 'the story') + ' — its action log names atoms this one may not have');
    if (ctx.tick !== 0 || ctx.log.length)
      throw new Error('load into a fresh world: a log is a history from genesis');
    source = replay(saved.log);
    return saved.log.length;
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
    save,
    load,
    /** Has the current source run out? True once a replay reaches its end. */
    replaying: () => !source.done(),
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
