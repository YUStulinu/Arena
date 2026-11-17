// Arena in the browser: join the matchmaker's queue, play online over a
// WebSocket with rollback (the engine in Wasm), or practice against a bot.

import { Engine, type Game, type Observation, type Session } from "./engine.js";
import { hello, input, keysToInput, parse } from "./proto.js";

const $ = <T extends HTMLElement>(id: string) => document.getElementById(id) as T;
const canvas = $<HTMLCanvasElement>("view");
const ctx = canvas.getContext("2d")!;
const keys = { up: false, down: false, left: false, right: false, dash: false, pulse: false };
const KEYMAP: Record<string, keyof typeof keys> = {
  KeyW: "up", ArrowUp: "up", KeyS: "down", ArrowDown: "down", KeyA: "left", ArrowLeft: "left", KeyD: "right", ArrowRight: "right",
  Space: "dash", KeyJ: "pulse", ShiftLeft: "pulse", ShiftRight: "pulse",
};
const COLORS = ["#3b82f6", "#f59e0b"];

let engine: Engine;
let game: Game;


function status(text: string) {
  $("status").textContent = text;
}

// ---- drawing -------------------------------------------------------------------------

function draw(o: Observation, me: number, info: string) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  if (canvas.width !== Math.round(w * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.fillStyle = "#0e1116";
  ctx.fillRect(0, 0, w, h);
  const scale = Math.min(w, h) / 2 / 10.5;
  ctx.save();
  ctx.translate(w / 2, h / 2);
  ctx.scale(scale, -scale); // y up
  // the platform, shrinking
  ctx.beginPath();
  ctx.arc(0, 0, 9, 0, 2 * Math.PI);
  ctx.strokeStyle = "#2a3140";
  ctx.lineWidth = 0.05;
  ctx.stroke();
  ctx.beginPath();
  ctx.arc(0, 0, o.arenaRadius, 0, 2 * Math.PI);
  ctx.fillStyle = "#1b2230";
  ctx.fill();
  ctx.strokeStyle = "#8b95a7";
  ctx.lineWidth = 0.06;
  ctx.stroke();
  o.players.forEach((p, i) => {
    if (p.pulseFlash > 0) {
      ctx.beginPath();
      ctx.arc(p.x, p.y, 2.6 * (1 - p.pulseFlash / 12) + 0.3, 0, 2 * Math.PI);
      ctx.strokeStyle = `rgba(255,255,255,${p.pulseFlash / 12})`;
      ctx.lineWidth = 0.08;
      ctx.stroke();
    }
    ctx.beginPath();
    ctx.arc(p.x, p.y, p.radius, 0, 2 * Math.PI);
    ctx.fillStyle = COLORS[i];
    ctx.fill();
    if (i === me) {
      ctx.strokeStyle = "#ffffff";
      ctx.lineWidth = 0.07;
      ctx.stroke();
    }
    const a = (p.facing / 4096) * 2 * Math.PI;
    ctx.beginPath();
    ctx.moveTo(p.x, p.y);
    ctx.lineTo(p.x + Math.cos(a) * p.radius * 1.4, p.y + Math.sin(a) * p.radius * 1.4);
    ctx.strokeStyle = "#0e1116";
    ctx.lineWidth = 0.12;
    ctx.stroke();
  });
  ctx.restore();
  ctx.fillStyle = "#e6e8eb";
  ctx.font = "600 20px system-ui, sans-serif";
  ctx.textAlign = "center";
  ctx.fillText(`${o.players[0].score}  :  ${o.players[1].score}`, w / 2, 32);
  ctx.font = "13px system-ui, sans-serif";
  ctx.fillStyle = "#9aa3ae";
  ctx.fillText(`round ${o.round}` + (o.pause > 0 ? " — next round" : ""), w / 2, 52);
  ctx.textAlign = "left";
  ctx.fillText(info, 12, h - 12);
  const mine = o.players[me];
  ctx.textAlign = "right";
  ctx.fillText(`dash ${mine.dashCd ? "…" : "ready"} · pulse ${mine.pulseCd ? "…" : "ready"}`, w - 12, h - 12);
  if (o.phase === 1) {
    ctx.textAlign = "center";
    ctx.font = "700 32px system-ui, sans-serif";
    ctx.fillStyle = "#ffffff";
    ctx.fillText(o.winner === me ? "You win" : "You lose", w / 2, h / 2);
  }
}

// ---- practice against a simple bot (offline) ----------------------------------------------

function chaseInput(o: Observation, me: number): number {
  const a = o.players[me], b = o.players[1 - me];
  const ang = Math.atan2(b.y - a.y, b.x - a.x);
  const d = (Math.round(((ang + 2 * Math.PI) % (2 * Math.PI)) / (Math.PI / 4)) % 8) + 1;
  const dist = Math.hypot(b.x - a.x, b.y - a.y);
  return d | (dist < 3 && Math.random() < 0.05 ? 16 : 0) | (dist < 1.8 && Math.random() < 0.2 ? 32 : 0);
}

function practice() {
  const s = game.initial();
  let last = performance.now(), acc = 0;
  const loop = (now: number) => {
    acc += now - last;
    last = now;
    let o = engine.observe(s);
    while (acc >= 1000 / 60) {
      acc -= 1000 / 60;
      game.step(s, keysToInput(keys), chaseInput(o, 1));
      o = engine.observe(s);
    }
    draw(o, 0, "practice against the chase bot");
    if (o.phase !== 1) requestAnimationFrame(loop);
    else game.free(s);
  };
  requestAnimationFrame(loop);
}

// ---- online --------------------------------------------------------------------------------

async function online(name: string) {
  const r = await fetch("/api/queue", { method: "POST", body: JSON.stringify({ name }), headers: { "content-type": "application/json" } });
  if (!r.ok) return status((await r.json()).error);
  const { ticket } = await r.json();
  status("waiting for an opponent…");
  let t: any;
  for (;;) {
    t = await (await fetch(`/api/ticket/${ticket}`)).json();
    if (t.status === "matched") break;
    await new Promise((res) => setTimeout(res, 300));
  }
  status(`match ${t.match}: connecting…`);
  const ws = new WebSocket(t.ws);
  ws.binaryType = "arraybuffer";
  let session: Session | null = null;
  let haveMine = 0, t0 = 0, delay = t.delay;
  let finished: { winner: number; desyncs: number } | null = null;
  const send = (b: Uint8Array) => ws.readyState === WebSocket.OPEN && ws.send(b);
  ws.onopen = () => send(hello(t.match, t.player, t.token));
  ws.onmessage = (ev) => {
    const m = parse(ev.data as ArrayBuffer);
    if (!m) return;
    if (m.kind === "welcome") delay = m.delay;
    else if (m.kind === "start" && !session) {
      session = game.session(t.player, delay);
      t0 = performance.now();
      status(`match ${t.match}: you are ${t.player === 0 ? "blue" : "orange"}`);
      requestAnimationFrame(frame);
    } else if (m.kind === "inputs" && session) {
      haveMine = Math.max(haveMine, m.haveYours);
      m.inputs.forEach((v, k) => session!.addRemote(m.first + k, v));
    } else if (m.kind === "result") finished = m;
    else if (m.kind === "error") status(m.message);
  };
  const frame = () => {
    const s = session!;
    const target = Math.floor(((performance.now() - t0) / 1000) * 60);
    let advanced = false;
    for (let k = 0; k < 3 && s.frame < target; k++) {
      if (s.localKnown <= s.frame + delay) s.addLocal(keysToInput(keys));
      if (!s.advance()) break;
      advanced = true;
    }
    if (advanced) {
      const known = s.localKnown, first = Math.max(haveMine, known - 64);
      const inputs: number[] = [];
      for (let f = first; f < known; f++) inputs.push(s.localInput(f));
      send(input(t.match, t.player, s.remoteKnown, first, inputs, s.checksums().slice(0, 20)));
    }
    const st = s.stats();
    draw(s.observe(), t.player, `rollbacks ${st.rollbacks} · max depth ${st.maxRollback} · stalls ${st.stalls}` + (finished ? ` · desyncs ${finished.desyncs}` : ""));
    if (!finished) requestAnimationFrame(frame);
    else {
      status(finished.winner === t.player ? "you won" : finished.winner === -1 ? "no result" : "you lost");
      ws.close();
      leaderboard();
    }
  };
}

async function leaderboard() {
  try {
    const rows = await (await fetch("/api/leaderboard")).json();
    $("board").innerHTML = rows
      .slice(0, 12)
      .map((r: any) => `<tr><td>${r.name.replace(/[<>&]/g, "")}</td><td>${r.rating.toFixed(0)}</td><td>±${(2 * r.rd).toFixed(0)}</td><td>${r.wins}/${r.games}</td></tr>`)
      .join("");
  } catch {
    $("board").innerHTML = "<tr><td colspan=4>no matchmaker</td></tr>";
  }
}

async function main() {
  addEventListener("keydown", (e) => {
    if (KEYMAP[e.code] && document.activeElement?.tagName !== "INPUT") {
      keys[KEYMAP[e.code]] = true;
      e.preventDefault();
    }
  });
  addEventListener("keyup", (e) => {
    if (KEYMAP[e.code]) keys[KEYMAP[e.code]] = false;
  });
  const [wasm, lua] = await Promise.all([fetch("public/arena.wasm").then((r) => r.arrayBuffer()), fetch("public/arena.lua").then((r) => r.text())]);
  engine = await Engine.load(wasm);

  game = engine.newGame(lua);
  $("play").addEventListener("click", () => {
    const name = $<HTMLInputElement>("name").value.trim();
    if (name) online(name);
  });
  $("practice").addEventListener("click", practice);
  status("ready");
  leaderboard();
  draw(engine.observe(game.initial()), 0, "WASD / arrows: move · Space: dash · J or Shift: pulse");
}

main().catch((e) => status(String(e)));
