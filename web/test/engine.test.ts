import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

import { Engine } from "../src/engine.js";

const wasm = readFileSync(new URL("../../public/arena.wasm", import.meta.url));
const script = readFileSync(new URL("../../../engine/scripts/arena.lua", import.meta.url), "utf8");
const golden = JSON.parse(readFileSync(new URL("../../../engine/tests/golden.json", import.meta.url), "utf8"));

test("the Wasm engine reproduces the native golden replay", async () => {
  const engine = await Engine.load(wasm);
  const game = engine.newGame(script);
  const s = game.initial();
  let x = 12345;
  const got: string[] = [];
  for (let f = 1; f <= 3000; f++) {
    const inp: number[] = [];
    for (let k = 0; k < 2; k++) {
      x = (Math.imul(x, 1103515245) + 12345) >>> 0;
      inp.push((x >>> 16) % 64);
    }
    game.step(s, inp[0], inp[1]);
    if (f % 1000 === 0) got.push(game.hash(s));
  }
  assert.deepEqual(got, golden.hashes);
  game.free(s);
});

test("script errors come back as exceptions (Lua errors through Wasm exception handling)", async () => {
  const engine = await Engine.load(wasm);
  assert.throws(() => engine.newGame("this is not lua"), /gameplay script/);
  const game = engine.newGame("function tick() w.set_pos(0, 1.5, 0) end");
  const s = game.initial();
  assert.throws(() => game.step(s, 0, 0), /script error/);
});

test("two Wasm sessions linked directly stay in step", async () => {
  const engine = await Engine.load(wasm);
  const game = engine.newGame(script);
  const a = game.session(0, 2), b = game.session(1, 2);
  for (let t = 0; t < 600; t++) {
    const fa = a.addLocal((t * 7) % 64), fb = b.addLocal((t * 13) % 64);
    b.addRemote(fa, a.localInput(fa));
    a.addRemote(fb, b.localInput(fb));
    assert.ok(a.advance() && b.advance());
  }
  assert.equal(a.observe().players[0].x, b.observe().players[0].x);
  const ca = a.checksums(), cb = b.checksums();
  assert.ok(ca.length > 10);
  assert.deepEqual(ca, cb.slice(0, ca.length));
});
