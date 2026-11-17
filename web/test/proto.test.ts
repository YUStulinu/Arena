import { test } from "node:test";
import assert from "node:assert/strict";

import { hello, input, keysToInput, parse } from "../src/proto.js";

test("hello and input packets have the server's layout", () => {
  assert.deepEqual(Array.from(hello(7, 1, 0x01020304)), [1, 7, 0, 0, 0, 1, 4, 3, 2, 1]);
  const p = input(7, 0, 30, 28, [5, 6], [[30, "00000000deadbeef"]]);
  assert.equal(p.length, 1 + 4 + 1 + 4 + 4 + 1 + 2 + 1 + 12);
  assert.equal(p[14], 2); // count
  assert.deepEqual(Array.from(p.slice(15, 17)), [5, 6]);
  assert.equal(p[17], 1); // checksums
  assert.deepEqual(Array.from(p.slice(22, 26)), [0xef, 0xbe, 0xad, 0xde]);
});

test("server messages parse, garbage does not", () => {
  const b = new Uint8Array([12, 3, 0, 0, 0, 10, 0, 0, 0, 2, 9, 8]);
  assert.deepEqual(parse(b.buffer), { kind: "inputs", haveYours: 3, first: 10, inputs: [9, 8] });
  assert.equal(parse(new Uint8Array([12, 3, 0]).buffer), null);
  assert.equal(parse(new Uint8Array([12, 0, 0, 0, 0, 0, 0, 0, 0, 5, 1]).buffer), null); // claims 5 inputs, has 1
  assert.deepEqual(parse(new Uint8Array([13, 0xff, 2, 0, 0, 0]).buffer), { kind: "result", winner: -1, desyncs: 2 });
  assert.equal(parse(new Uint8Array([99]).buffer), null);
});

test("keys map to the engine's direction codes", () => {
  const none = { up: false, down: false, left: false, right: false, dash: false, pulse: false };
  assert.equal(keysToInput(none), 0);
  assert.equal(keysToInput({ ...none, right: true }), 1);
  assert.equal(keysToInput({ ...none, up: true, left: true }), 4);
  assert.equal(keysToInput({ ...none, left: true, right: true, dash: true }), 16);
  assert.equal(keysToInput({ ...none, down: true, pulse: true }), 7 | 32);
});
