// The wire protocol (server/src/proto.rs), the same bytes as over UDP.

export function hello(match: number, player: number, token: number): Uint8Array {
  const b = new DataView(new ArrayBuffer(10));
  b.setUint8(0, 1);
  b.setUint32(1, match, true);
  b.setUint8(5, player);
  b.setUint32(6, token, true);
  return new Uint8Array(b.buffer);
}

export function input(match: number, player: number, haveRemote: number, first: number, inputs: number[], checksums: [number, string][]): Uint8Array {
  const n = 1 + 4 + 1 + 4 + 4 + 1 + inputs.length + 1 + checksums.length * 12;
  const b = new DataView(new ArrayBuffer(n));
  let p = 0;
  b.setUint8(p++, 2);
  b.setUint32(p, match, true); p += 4;
  b.setUint8(p++, player);
  b.setUint32(p, haveRemote, true); p += 4;
  b.setUint32(p, first, true); p += 4;
  b.setUint8(p++, inputs.length);
  for (const v of inputs) b.setUint8(p++, v);
  b.setUint8(p++, checksums.length);
  for (const [f, h] of checksums) {
    b.setUint32(p, f, true); p += 4;
    b.setBigUint64(p, BigInt("0x" + h), true); p += 8;
  }
  return new Uint8Array(b.buffer);
}

export type ServerMsg =
  | { kind: "welcome"; match: number; player: number; delay: number }
  | { kind: "start"; delay: number }
  | { kind: "inputs"; haveYours: number; first: number; inputs: number[] }
  | { kind: "result"; winner: number; desyncs: number }
  | { kind: "error"; message: string };

export function parse(buf: ArrayBuffer): ServerMsg | null {
  const b = new DataView(buf);
  if (b.byteLength < 1) return null;
  switch (b.getUint8(0)) {
    case 10:
      return b.byteLength >= 7 ? { kind: "welcome", match: b.getUint32(1, true), player: b.getUint8(5), delay: b.getUint8(6) } : null;
    case 11:
      return { kind: "start", delay: b.byteLength >= 2 ? b.getUint8(1) : -1 }; // START may overtake WELCOME
    case 12: {
      if (b.byteLength < 10) return null;
      const n = b.getUint8(9);
      if (b.byteLength < 10 + n) return null;
      return { kind: "inputs", haveYours: b.getUint32(1, true), first: b.getUint32(5, true), inputs: Array.from(new Uint8Array(buf, 10, n)) };
    }
    case 13:
      return b.byteLength >= 6 ? { kind: "result", winner: b.getInt8(1), desyncs: b.getUint32(2, true) } : null;
    case 14:
      return { kind: "error", message: new TextDecoder().decode(new Uint8Array(buf, 1)) };
  }
  return null;
}

/** The input byte from the keys held: 8 directions, dash, pulse. */
export function keysToInput(k: { up: boolean; down: boolean; left: boolean; right: boolean; dash: boolean; pulse: boolean }): number {
  const x = (k.right ? 1 : 0) - (k.left ? 1 : 0), y = (k.up ? 1 : 0) - (k.down ? 1 : 0);
  // direction codes 1..8: E, NE, N, NW, W, SW, S, SE (y up)
  const table: Record<string, number> = { "1,0": 1, "1,1": 2, "0,1": 3, "-1,1": 4, "-1,0": 5, "-1,-1": 6, "0,-1": 7, "1,-1": 8 };
  return (table[`${x},${y}`] ?? 0) | (k.dash ? 16 : 0) | (k.pulse ? 32 : 0);
}
