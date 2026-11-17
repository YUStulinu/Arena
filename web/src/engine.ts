// The engine compiled to Wasm (engine/build_wasm.sh), with a minimal WASI
// layer: the engine needs no files, clocks or environment, so every WASI call
// answers "nothing there".

export const OBS = 32;

export interface Observation {
  frame: number;
  arenaRadius: number; // units
  phase: number;
  winner: number;
  players: { x: number; y: number; vx: number; vy: number; radius: number; score: number; dashCd: number; pulseCd: number; facing: number; pulseFlash: number; fallen: number }[];
  round: number;
  pause: number;
}

const FX = 65536;
const ENOSYS = 52;

function wasiStub(memory: () => WebAssembly.Memory): Record<string, (...a: number[]) => number> {
  const zero32 = (...ptrs: number[]) => {
    const v = new DataView(memory().buffer);
    for (const p of ptrs) v.setUint32(p, 0, true);
    return 0;
  };
  return {
    environ_sizes_get: (count: number, size: number) => zero32(count, size),
    environ_get: () => 0,
    clock_time_get: (_id: number, _prec: number, out: number) => {
      new DataView(memory().buffer).setBigUint64(out, 0n, true);
      return 0;
    },
    fd_write: (_fd: number, _iovs: number, _n: number, written: number) => zero32(written),
    fd_close: () => 0,
    fd_seek: () => ENOSYS,
    fd_read: () => ENOSYS,
    fd_fdstat_get: () => ENOSYS,
    fd_fdstat_set_flags: () => ENOSYS,
    fd_prestat_get: () => 8, // EBADF: no preopened directories
    fd_prestat_dir_name: () => 8,
    fd_renumber: () => ENOSYS,
    path_open: () => ENOSYS,
    proc_exit: (code: number) => {
      throw new Error(`engine exited (${code})`);
    },
  };
}

interface Exports {
  memory: WebAssembly.Memory;
  _initialize(): void;
  arena_alloc(n: number): number;
  arena_free(p: number): void;
  arena_game_new(script: number, len: number, err: number, errlen: number): number;
  arena_game_free(g: number): void;
  arena_state_size(): number;
  arena_initial(g: number, state: number): void;
  arena_step(g: number, state: number, a: number, b: number): number;
  arena_hash(state: number): bigint;
  arena_observe(state: number, out: number): void;
  arena_session_new(g: number, local: number, delay: number, window: number): number;
  arena_session_free(s: number): void;
  arena_session_add_local(s: number, input: number): number;
  arena_session_add_remote(s: number, frame: number, input: number): void;
  arena_session_advance(s: number): number;
  arena_session_frame(s: number): number;
  arena_session_remote_known(s: number): number;
  arena_session_local_known(s: number): number;
  arena_session_local_input(s: number, frame: number): number;
  arena_session_state(s: number): number;
  arena_session_stats(s: number, out: number): void;
  arena_session_checksums(s: number, frames: number, hashes: number, max: number): number;
}

export class Engine {
  private constructor(private x: Exports) {}

  static async load(bytes: BufferSource): Promise<Engine> {
    let mem: WebAssembly.Memory | null = null;
    const { instance } = await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasiStub(() => mem!) });
    const x = instance.exports as unknown as Exports;
    mem = x.memory;
    x._initialize();
    return new Engine(x);
  }

  get exports() {
    return this.x;
  }

  private bytes(text: string): [number, number] {
    const b = new TextEncoder().encode(text);
    const p = this.x.arena_alloc(b.length + 1);
    new Uint8Array(this.x.memory.buffer, p, b.length).set(b);
    return [p, b.length];
  }

  /** A game for a gameplay script; throws with the script's error. */
  newGame(script: string): Game {
    const [p, n] = this.bytes(script);
    const err = this.x.arena_alloc(512);
    const g = this.x.arena_game_new(p, n, err, 512);
    this.x.arena_free(p);
    if (!g) {
      const msg = new TextDecoder().decode(new Uint8Array(this.x.memory.buffer, err, 512)).split("\0")[0];
      this.x.arena_free(err);
      throw new Error(msg);
    }
    this.x.arena_free(err);
    return new Game(this, g);
  }

  observe(state: number): Observation {
    const out = this.x.arena_alloc(OBS * 4);
    this.x.arena_observe(state, out);
    const o = Array.from(new Int32Array(this.x.memory.buffer, out, OBS));
    this.x.arena_free(out);
    return decode(o);
  }
}

/** Raw observation values to units (fixed point / 65536). */
export function decode(o: number[]): Observation {
  const players = [0, 1].map((i) => {
    const b = 4 + 11 * i;
    return {
      x: o[b] / FX, y: o[b + 1] / FX, vx: o[b + 2] / FX, vy: o[b + 3] / FX, radius: o[b + 4] / FX, score: o[b + 5],
      dashCd: o[b + 6], pulseCd: o[b + 7], facing: o[b + 8], pulseFlash: o[b + 9], fallen: o[b + 10],
    };
  });
  return { frame: o[0], arenaRadius: o[1] / FX, phase: o[2], winner: o[3], players, round: o[26], pause: o[27] };
}

export class Game {
  constructor(readonly engine: Engine, readonly handle: number) {}

  /** A fresh state block (the caller frees it with free()). */
  initial(): number {
    const x = this.engine.exports;
    const s = x.arena_alloc(x.arena_state_size());
    x.arena_initial(this.handle, s);
    return s;
  }

  step(state: number, a: number, b: number) {
    if (this.engine.exports.arena_step(this.handle, state, a, b) !== 0) throw new Error("gameplay script error");
  }

  hash(state: number): string {
    return this.engine.exports.arena_hash(state).toString(16).padStart(16, "0");
  }

  free(state: number) {
    this.engine.exports.arena_free(state);
  }

  session(local: number, delay: number, window = 12): Session {
    const s = this.engine.exports.arena_session_new(this.handle, local, delay, window);
    if (!s) throw new Error("cannot create a session");
    return new Session(this.engine, s);
  }
}

export interface Stats {
  frames: number; rollbacks: number; resimulated: number; maxRollback: number; mispredicted: number; predicted: number; stalls: number;
}

/** The rollback session (engine/include/arena/session.hpp) seen from JS. */
export class Session {
  constructor(private engine: Engine, readonly handle: number) {}
  private get x() {
    return this.engine.exports;
  }
  addLocal(input: number): number {
    return this.x.arena_session_add_local(this.handle, input);
  }
  addRemote(frame: number, input: number) {
    this.x.arena_session_add_remote(this.handle, frame, input);
  }
  advance(): boolean {
    const r = this.x.arena_session_advance(this.handle);
    if (r < 0) throw new Error("gameplay script error");
    return r === 1;
  }
  get frame() {
    return this.x.arena_session_frame(this.handle);
  }
  get remoteKnown() {
    return this.x.arena_session_remote_known(this.handle);
  }
  get localKnown() {
    return this.x.arena_session_local_known(this.handle);
  }
  localInput(frame: number) {
    return this.x.arena_session_local_input(this.handle, frame);
  }
  observe(): Observation {
    return this.engine.observe(this.x.arena_session_state(this.handle));
  }
  stats(): Stats {
    const p = this.x.arena_alloc(7 * 8);
    this.x.arena_session_stats(this.handle, p);
    const v = Array.from(new BigInt64Array(this.x.memory.buffer, p, 7), Number);
    this.x.arena_free(p);
    return { frames: v[0], rollbacks: v[1], resimulated: v[2], maxRollback: v[3], mispredicted: v[4], predicted: v[5], stalls: v[6] };
  }
  /** Newly confirmed (frame, hash) checksums. */
  checksums(): [number, string][] {
    const max = 64;
    const f = this.x.arena_alloc(max * 4), h = this.x.arena_alloc(max * 8);
    const n = this.x.arena_session_checksums(this.handle, f, h, max);
    const frames = Array.from(new Int32Array(this.x.memory.buffer, f, n));
    const hashes = Array.from(new BigUint64Array(this.x.memory.buffer, h, n), (v) => v.toString(16).padStart(16, "0"));
    this.x.arena_free(f);
    this.x.arena_free(h);
    return frames.map((fr, i) => [fr, hashes[i]]);
  }
  free() {
    this.x.arena_session_free(this.handle);
  }
}
