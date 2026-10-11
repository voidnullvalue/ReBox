#!/usr/bin/env node
/* Headless UI/JS acceptance for assets/index.html.
 *
 * The webpage has no build step and must run untouched in an Android WebView.
 * This harness extracts the real <script>, runs it in a Node VM against a small
 * fake DOM and a controllable WebSocket, and drives the host confirmation gate
 * and the exit/leave controls exactly as a phone would. It proves:
 *   - host QUIT/RESTART are receiver-acknowledged, two-step and cancellable;
 *   - an ordinary player/spectator can never emit a host command;
 *   - "Leave controller" is local only and preserves the stored identity;
 *   - a pending confirmation fails closed on disconnect and stale epochs, and
 *     nothing auto-quits on reconnect;
 *   - the Android back gesture during an armed/pending confirmation cancels it
 *     on the core and waits for the ack instead of dropping the gate;
 *   - a host leave disarms any armed/pending confirmation with CANCEL before it
 *     fires the close event, and fails closed (no leave) if that can't be acked;
 *   - a left controller never reconnects from a late timer;
 *   - window.ReboxJeopardy.requestExit() is the only exposed API and carries no
 *     admin capability.
 */
import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ASSET = path.join(HERE, "..", "assets", "index.html");

let CHECKS = 0;
let FAILS = 0;
function check(cond, msg) {
  CHECKS++;
  if (!cond) {
    FAILS++;
    console.error("FAIL:", msg);
  }
}

/* ------------------------------------------------------------------ */
/* Minimal fake DOM                                                    */
/* ------------------------------------------------------------------ */
class ClassList {
  constructor() { this.set = new Set(); }
  add(...c) { c.forEach((x) => this.set.add(x)); }
  remove(...c) { c.forEach((x) => this.set.delete(x)); }
  toggle(c, force) {
    if (force === undefined) force = !this.set.has(c);
    if (force) this.set.add(c); else this.set.delete(c);
    return force;
  }
  contains(c) { return this.set.has(c); }
}

const TAG_RE = /<([a-zA-Z][\w-]*)((?:[^>"']|"[^"]*"|'[^']*')*?)\/?>/g;

class El {
  constructor(tag, doc) {
    this.tagName = String(tag || "div").toUpperCase();
    this.doc = doc;
    this.children = [];
    this.parent = null;
    this.classList = new ClassList();
    this.style = {};
    this.dataset = {};
    this._className = "";
    this._text = "";
    this._value = "";
    this._id = "";
    this.disabled = false;
    this.checked = false;
    this.onclick = null;
    this._innerHTML = "";
    this._listeners = {};
    this._descIds = [];
    this.__owner = null;
  }
  get id() { return this._id; }
  set id(v) { this._id = v; if (v) this.doc.byId.set(v, this); }
  get className() { return this._className; }
  set className(v) {
    this._className = v || "";
    this.classList.set = new Set(String(this._className).split(/\s+/).filter(Boolean));
  }
  get textContent() { return this._text; }
  set textContent(v) { this._text = v == null ? "" : String(v); }
  get value() { return this._value; }
  set value(v) { this._value = v == null ? "" : v; }
  get innerHTML() { return this._innerHTML; }
  set innerHTML(html) {
    this._innerHTML = html == null ? "" : String(html);
    for (const id of this._descIds) {
      const e = this.doc.byId.get(id);
      if (e && e.__owner === this) this.doc.byId.delete(id);
    }
    this._descIds = [];
    this.children = [];
    TAG_RE.lastIndex = 0;
    let m;
    while ((m = TAG_RE.exec(this._innerHTML))) {
      const el = new El(m[1], this.doc);
      el.__owner = this;
      this._parseAttrs(el, m[2] || "");
      const after = TAG_RE.lastIndex;
      const nextLt = this._innerHTML.indexOf("<", after);
      const text = this._innerHTML.slice(after, nextLt < 0 ? this._innerHTML.length : nextLt);
      if (text) el.textContent = text.replace(/\s+/g, " ").trim();
      this.appendChild(el);
    }
  }
  _parseAttrs(el, attrs) {
    const re = /([a-zA-Z_:][\w:.-]*)(?:\s*=\s*"([^"]*)")?/g;
    let a;
    while ((a = re.exec(attrs))) {
      const name = a[1];
      const val = a[2];
      if (name === "id" && val !== undefined) { el.id = val; this._descIds.push(val); }
      else if (name === "value" && val !== undefined) el.value = val;
      else if (name === "disabled") el.disabled = true;
      else if (name === "checked") el.checked = true;
    }
  }
  appendChild(c) { this.children.push(c); c.parent = this; return c; }
  removeChild(c) { const i = this.children.indexOf(c); if (i >= 0) this.children.splice(i, 1); return c; }
  addEventListener(t, fn) { (this._listeners[t] = this._listeners[t] || []).push(fn); }
  removeEventListener(t, fn) {
    const a = this._listeners[t] || [];
    const i = a.indexOf(fn);
    if (i >= 0) a.splice(i, 1);
  }
  _fire(t, ev) {
    const e = ev || { preventDefault() {}, stopPropagation() {} };
    if (typeof this["on" + t] === "function") this["on" + t](e);
    for (const fn of this._listeners[t] || []) fn(e);
  }
  get firstChild() { return this.children[0] || null; }
}

class Doc {
  constructor() { this.byId = new Map(); this._listeners = {}; }
  createElement(tag) { return new El(tag, this); }
  getElementById(id) {
    let e = this.byId.get(id);
    if (!e) { e = new El("div", this); e.id = id; }
    return e;
  }
  addEventListener(t, fn) { (this._listeners[t] = this._listeners[t] || []).push(fn); }
  _fire(t, ev) { for (const fn of this._listeners[t] || []) fn(ev); }
}

class FakeWS {
  constructor(url) {
    this.url = url;
    this.readyState = 0;
    this.sent = [];
    this.onopen = null;
    this.onclose = null;
    this.onerror = null;
    this.onmessage = null;
  }
  send(data) { this.sent.push(JSON.parse(data)); }
  close() { this.readyState = 3; if (this.onclose) this.onclose(); }
  _open() { this.readyState = 1; if (this.onopen) this.onopen(); }
  _msg(obj) { if (this.onmessage) this.onmessage({ data: typeof obj === "string" ? obj : JSON.stringify(obj) }); }
  _serverClose() { this.readyState = 3; if (this.onclose) this.onclose(); }
}

class CustomEvent {
  constructor(type, opts) { this.type = type; this.detail = opts && opts.detail; }
}

function makeEnv(html) {
  const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
  if (!scriptMatch) throw new Error("no <script> in index.html");
  const script = scriptMatch[1];

  const doc = new Doc();
  const store = new Map();
  const timers = [];
  let tid = 0;
  let vnow = 0;

  function fakeSetTimeout(fn, ms) { const id = ++tid; timers.push({ id, fn, at: vnow + (ms || 0) }); return id; }
  function fakeClearTimeout(id) { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); }
  function advance(ms) {
    const target = vnow + (ms || 0);
    let guard = 0;
    while (guard++ < 100000) {
      timers.sort((a, b) => a.at - b.at);
      const t = timers[0];
      if (!t || t.at > target) break;
      timers.shift();
      vnow = t.at;
      try { t.fn(); } catch (e) { /* keep the harness driving */ }
    }
    vnow = target;
  }

  const wsInstances = [];
  class WebSocket extends FakeWS {
    constructor(url) { super(url); wsInstances.push(this); }
  }

  const windowObj = {
    _listeners: {},
    _events: [],
    AudioContext: undefined,
    webkitAudioContext: undefined,
    addEventListener(t, fn) { (this._listeners[t] = this._listeners[t] || []).push(fn); },
    removeEventListener(t, fn) {
      const a = this._listeners[t] || []; const i = a.indexOf(fn); if (i >= 0) a.splice(i, 1);
    },
    dispatchEvent(ev) {
      this._events.push(ev);
      for (const fn of this._listeners[ev.type] || []) fn(ev);
      return true;
    },
  };

  const localStorage = {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => store.set(k, String(v)),
    removeItem: (k) => store.delete(k),
    clear: () => store.clear(),
  };

  const location = { protocol: "http:", host: "127.0.0.1:8135", hash: "" };
  const navigator = { vibrate() {} };

  let fetchHandler = async () => ({ ok: false, status: 404, json: async () => ({}) });
  const fetch = (p, opts) => fetchHandler(p, opts);

  // Pre-create every element declared in the static markup (with its class).
  const staticPart = html.slice(0, html.indexOf("<script>"));
  TAG_RE.lastIndex = 0;
  let sm;
  while ((sm = TAG_RE.exec(staticPart))) {
    const el = new El(sm[1], doc);
    el._parseAttrs(el, sm[2] || "");
    el.__owner = null;
  }

  const sandbox = {
    window: windowObj,
    document: doc,
    location,
    localStorage,
    navigator,
    WebSocket,
    fetch,
    setTimeout: fakeSetTimeout,
    clearTimeout: fakeClearTimeout,
    requestAnimationFrame: () => 0,
    CustomEvent,
    console,
  };
  vm.createContext(sandbox);
  vm.runInContext(script, sandbox);

  return {
    sandbox,
    doc,
    window: windowObj,
    store,
    advance,
    ws: () => wsInstances[wsInstances.length - 1],
    wsCount: () => wsInstances.length,
    setFetch: (fn) => { fetchHandler = fn; },
  };
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */
function player(id, name) {
  return {
    id, name, score: 0, ready: 0, connected: 1, active: 1, eligible: 1, answered: 0,
    judged: 0, correct: 0, wager_set: 0, excluded: 0, can_buzz: 0,
    wager: null, answer: null, wager_min: null, wager_max: null,
  };
}

function baseState() {
  return {
    epoch: 1, revision: 1, phase: 1, paused: 0, round: 1, selector: 0, buzzed: -1,
    dd_selector: -1, tiebreak: 0, quit: 0, quit_confirm: 0, restart_confirm: 0,
    judge_cursor: 0, tiebreak_winner: -1, timer: { kind: 0, remaining_ms: 0 },
    players: [player(1, "Ann"), player(2, "Ben")],
    config: {
      read_ms: 30000, buzz_ms: 5000, answer_ms: 10000, dd_wager_ms: 10000,
      dd_answer_ms: 10000, final_wager_ms: 10000, final_answer_ms: 10000,
      tiebreak_answer_ms: 10000, min_dd_wager: 0, allow_negative: 1,
      dd_count_j: 1, dd_count_dj: 1, random_categories: 0, allow_tiebreak: 1,
    },
  };
}

function stateMsg(role, overState, overTop) {
  const st = Object.assign(baseState(), overState || {});
  const m = {
    type: "state", epoch: 1, revision: 1, window: 1, seq: 0, role,
    you: role === "player" ? { playerId: 1, color: "cobalt" } : null,
    colors: { 1: "cobalt", 2: "gold" }, state: st,
  };
  return Object.assign(m, overTop || {});
}

function cmds(ws) { return ws.sent.filter((x) => x && x.type === "cmd" && typeof x.action === "string"); }

const HOST_ONLY = new Set([
  "QUIT", "RESTART", "PAUSE", "RESUME", "START", "RESET_LOBBY", "CONFIG",
  "FILTER", "REMOVE", "SCORE", "REVEAL", "ADVANCE", "TIEBREAK", "PASS", "JUDGE",
]);

function hostBtn(doc, text) {
  return doc.getElementById("hostbar").children.find((b) => (b.textContent || "").toLowerCase() === text);
}

async function authHost(env) {
  env.setFetch(async (p) => {
    if (p === "/api/host") return { ok: true, status: 200, json: async () => ({ ok: true, role: "host", token: "HOSTTOKEN", seq: 0 }) };
    if (p === "/api/join") return { ok: true, status: 200, json: async () => ({ ok: true, role: "host", token: "HOSTTOKEN", seq: 0 }) };
    return { ok: false, status: 404, json: async () => ({}) };
  });
  env.doc.getElementById("h-pin").value = "123456";
  await env.doc.getElementById("do-host").onclick();
  const ws = env.ws();
  ws._open();
  ws._msg(stateMsg("host"));
  return ws;
}

async function authPlayer(env) {
  env.setFetch(async (p) => {
    if (p === "/api/join") return { ok: true, status: 200, json: async () => ({ ok: true, role: "player", playerId: 1, color: "cobalt", token: "PLAYERTOKEN" }) };
    return { ok: false, status: 404, json: async () => ({}) };
  });
  env.doc.getElementById("p-name").value = "Ann";
  await env.doc.getElementById("do-join").onclick();
  const ws = env.ws();
  ws._open();
  ws._msg(stateMsg("player"));
  return ws;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */
function boot(env) { env.advance(400); } // drain the 300 ms resume probe

async function testHostQuitGate() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);

  hostBtn(env.doc, "quit").onclick();
  check(!env.doc.getElementById("modal").classList.contains("hide"), "quit opens the in-page modal");
  check(cmds(ws).length === 0, "opening the quit modal sends no command");
  let prim = env.doc.getElementById("hg-primary");
  check(prim && !prim.disabled, "arm button enabled");
  check(prim.textContent.toLowerCase().includes("quit"), "primary labels the quit action");

  prim.onclick(); // arm
  check(cmds(ws).length === 1 && cmds(ws)[0].action === "QUIT", "first press arms with a QUIT");
  prim = env.doc.getElementById("hg-primary");
  check(prim.disabled, "commit disabled until the receiver acknowledges");

  ws._msg(stateMsg("host", { quit_confirm: 1 }));
  prim = env.doc.getElementById("hg-primary");
  check(prim && !prim.disabled, "commit enabled after quit_confirm ack");
  check(prim.textContent.toLowerCase().includes("confirm"), "primary becomes the commit button");

  prim.onclick(); // commit
  check(cmds(ws).filter((c) => c.action === "QUIT").length === 2, "second press commits a second QUIT");
  ws._msg(stateMsg("host", { quit: 1 }));
  check(env.doc.getElementById("modal").classList.contains("hide"), "modal closes once the quit is committed");
}

async function testHostQuitCancel() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  hostBtn(env.doc, "quit").onclick();
  env.doc.getElementById("hg-primary").onclick(); // arm
  ws._msg(stateMsg("host", { quit_confirm: 1 })); // acked
  env.doc.getElementById("hg-cancel").onclick();
  const last = cmds(ws)[cmds(ws).length - 1];
  check(last && last.action === "CANCEL", "cancel sends JPQ_ACT_CANCEL");
  check(!env.doc.getElementById("modal").classList.contains("hide"), "modal stays until the core clears");
  ws._msg(stateMsg("host", { quit_confirm: 0 }));
  check(env.doc.getElementById("modal").classList.contains("hide"), "cancel closes after the flags clear");
  check(cmds(ws).filter((c) => c.action === "QUIT").length === 1, "cancel never commits the armed quit");
}

async function testHostRestartGate() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  hostBtn(env.doc, "restart").onclick();
  env.doc.getElementById("hg-primary").onclick(); // arm
  check(cmds(ws).length === 1 && cmds(ws)[0].action === "RESTART", "restart first press arms");
  ws._msg(stateMsg("host", { restart_confirm: 1 }));
  const prim = env.doc.getElementById("hg-primary");
  check(prim && !prim.disabled, "restart commit enabled after ack");
  prim.onclick();
  check(cmds(ws).filter((c) => c.action === "RESTART").length === 2, "restart second press commits");
  ws._msg(stateMsg("host", { phase: 0, restart_confirm: 0, quit: 0 }));
  check(env.doc.getElementById("modal").classList.contains("hide"), "restart modal closes on the fresh lobby");
}

async function testPauseResumeAnyPhase() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  hostBtn(env.doc, "pause").onclick();
  check(cmds(ws).some((c) => c.action === "PAUSE"), "host can pause from the board");
  ws._msg(stateMsg("host", { paused: 1, phase: 4 }));
  hostBtn(env.doc, "resume").onclick();
  check(cmds(ws).some((c) => c.action === "RESUME"), "host can resume from any phase");
  check(hostBtn(env.doc, "quit") !== undefined, "quit stays visible while paused");
}

async function testPlayerNeverHostCommands() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authPlayer(env);
  check(env.doc.getElementById("hostbar").classList.contains("hide"), "host bar hidden for a player");
  check(hostBtn(env.doc, "quit") === undefined, "player has no quit control");

  env.doc.getElementById("do-ready").onclick();
  env.doc.getElementById("buzz").disabled = false;
  env.doc.getElementById("buzz")._fire("pointerdown");
  const sent = cmds(ws);
  check(sent.length >= 1, "player controls do issue player commands");
  check(sent.every((c) => !HOST_ONLY.has(c.action)), "player never emits a host-only command");
}

async function testRequestExitPlayerLocalLeave() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authPlayer(env);
  const closeEvents = [];
  env.window.addEventListener("rebox-controller-close", (e) => closeEvents.push(e));

  const handled = env.sandbox.window.ReboxJeopardy.requestExit();
  check(handled === true, "requestExit handles the back gesture for an authenticated player");
  check(!env.doc.getElementById("modal").classList.contains("hide"), "requestExit opens Stay/Leave");
  check(env.doc.getElementById("exit-stay") !== undefined && env.doc.getElementById("exit-leave") !== undefined, "Stay and Leave offered");

  env.doc.getElementById("exit-stay").onclick();
  check(env.doc.getElementById("modal").classList.contains("hide"), "Stay dismisses the modal");
  check(closeEvents.length === 0, "Stay fires no close event");

  const before = cmds(ws).length;
  env.sandbox.window.ReboxJeopardy.requestExit();
  env.doc.getElementById("exit-leave").onclick();
  check(cmds(ws).length === before, "Leave controller sends no command");
  check(closeEvents.length === 1, "Leave controller fires rebox-controller-close once");
  check(env.store.get("jpq.token") === "PLAYERTOKEN", "identity token is preserved across leave");
  check(JSON.stringify(closeEvents[0].detail || {}).indexOf("TOKEN") === -1, "close event carries no credential");

  // The page can return to the same seat.
  env.doc.getElementById("left-return").onclick();
  const ws2 = env.ws();
  check(ws2 !== ws, "returning opens a fresh socket");
  ws2._open();
  check(ws2.sent.some((m) => m.type === "hello" && m.token === "PLAYERTOKEN"), "return re-authenticates the preserved identity");
}

async function testDisconnectPendingFailsClosed() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  hostBtn(env.doc, "quit").onclick();
  env.doc.getElementById("hg-primary").onclick(); // arm, still pending
  check(cmds(ws).filter((c) => c.action === "QUIT").length === 1, "quit armed once");
  ws._serverClose();
  check(cmds(ws).filter((c) => c.action === "QUIT").length === 1, "disconnect does not auto-resend QUIT");
  const prim = env.doc.getElementById("hg-primary");
  check(prim && !prim.disabled && !prim.textContent.toLowerCase().includes("confirm"), "pending commit fails closed to the arm button");

  env.advance(2000); // reconnect timer fires
  const ws2 = env.ws();
  check(ws2 !== ws, "reconnect creates a new socket");
  ws2._open();
  ws2._msg(stateMsg("host", { quit_confirm: 0 }));
  check(cmds(ws2).length === 0, "reconnect never auto-quits");
}

async function testStaleEpochResetsGate() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  hostBtn(env.doc, "quit").onclick();
  env.doc.getElementById("hg-primary").onclick(); // pending, epoch 1
  ws._msg(stateMsg("host", { quit_confirm: 0 }, { epoch: 2 }));
  check(env.doc.getElementById("modal").classList.contains("hide"), "a stale epoch drops the pending confirmation");
  check(cmds(ws).filter((c) => c.action === "QUIT").length === 1, "stale epoch sends no extra command");
}

async function testNoDialogsOrTimedDoubleSend() {
  const html = fs.readFileSync(ASSET, "utf8");
  const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
  check(!/\bwindow\.confirm\b/.test(script), "no window.confirm");
  check(!/(^|[^.\w])confirm\s*\(/.test(script), "no confirm() dialog");
  check(!/(^|[^.\w])prompt\s*\(/.test(script), "no prompt() dialog");
  check(!/(^|[^.\w])alert\s*\(/.test(script), "no alert() dialog");
  check(!/setTimeout\s*\([^)]*cmd\s*\(/.test(script), "no timing-based double-send");
  check(!/management|adminToken|X-Admin/i.test(script), "no management/admin token use");
  check(!/addJavascriptInterface|AndroidBridge|ReboxAndroid/i.test(script), "no Java Android bridge dependency");
  // Host controls must precede the scrolling board in document order.
  const hb = html.indexOf('id="hostbar"');
  const boardIdx = html.indexOf('id="board"');
  const footer = html.indexOf('id="footer"');
  check(hb > 0 && boardIdx > 0 && hb < boardIdx, "host bar precedes the board (never buried by scroll)");
  check(hb < footer, "host bar precedes the BUZZ footer");
}

async function testOrdinaryBrowserApiSurface() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const api = env.sandbox.window.ReboxJeopardy;
  check(api && typeof api.requestExit === "function", "window.ReboxJeopardy.requestExit is exposed");
  check(api.closeEvent === "rebox-controller-close", "the close event name is documented");
  check(Object.keys(api).sort().join(",") === "closeEvent,requestExit,version", "the API exposes requestExit only (no admin/host capability)");
  // Unauthenticated: the page declines the back gesture instead of acting.
  check(api.requestExit() === false, "requestExit declines before authentication");
}

async function testBackDuringArmedQuitCancels() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  const closeEvents = [];
  env.window.addEventListener("rebox-controller-close", (e) => closeEvents.push(e));

  hostBtn(env.doc, "quit").onclick();
  env.doc.getElementById("hg-primary").onclick(); // arm
  ws._msg(stateMsg("host", { quit_confirm: 1 })); // acked -> armed
  check(cmds(ws).filter((c) => c.action === "QUIT").length === 1, "armed quit sent once");

  const handled = env.sandbox.window.ReboxJeopardy.requestExit();
  check(handled === true, "Android back is handled while a quit is armed");
  check(env.doc.getElementById("modal").innerHTML.indexOf("hg-primary") !== -1, "back keeps the armed gate on screen");
  check(env.doc.getElementById("modal").innerHTML.indexOf("hx-quit") === -1, "back never swaps the armed gate for the host exit menu");
  const last = cmds(ws)[cmds(ws).length - 1];
  check(last && last.action === "CANCEL", "back cancels the armed quit on the server");
  check(env.doc.getElementById("hg-primary").disabled, "gate stays waiting until the core acks the cancel");
  check(closeEvents.length === 0, "back does not leave the controller");

  ws._msg(stateMsg("host", { quit_confirm: 0 }));
  check(env.doc.getElementById("modal").classList.contains("hide"), "gate closes only after the core clears the confirm");
  check(cmds(ws).filter((c) => c.action === "QUIT").length === 1, "back-cancel never commits the quit");
}

async function testBackDuringPendingQuitCancels() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  hostBtn(env.doc, "quit").onclick();
  env.doc.getElementById("hg-primary").onclick(); // arm, still waiting for the TV
  const handled = env.sandbox.window.ReboxJeopardy.requestExit();
  check(handled === true, "Android back is handled while the quit is pending");
  const last = cmds(ws)[cmds(ws).length - 1];
  check(last && last.action === "CANCEL", "back cancels the still-pending quit");
  check(!env.doc.getElementById("modal").classList.contains("hide"), "pending gate stays up until the cancel is acked");
  check(env.doc.getElementById("modal").innerHTML.indexOf("hx-quit") === -1, "no leave menu replaces the pending gate");
  ws._msg(stateMsg("host", { quit_confirm: 0 }));
  check(env.doc.getElementById("modal").classList.contains("hide"), "pending gate closes after the cancel ack");
}

async function testBackDuringArmedRestartCancels() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  hostBtn(env.doc, "restart").onclick();
  env.doc.getElementById("hg-primary").onclick(); // arm
  ws._msg(stateMsg("host", { restart_confirm: 1 }));
  const handled = env.sandbox.window.ReboxJeopardy.requestExit();
  check(handled === true, "Android back is handled while a restart is armed");
  const last = cmds(ws)[cmds(ws).length - 1];
  check(last && last.action === "CANCEL", "back cancels the armed restart");
  check(env.doc.getElementById("modal").innerHTML.indexOf("hx-quit") === -1, "restart gate is not replaced by the exit menu");
  ws._msg(stateMsg("host", { restart_confirm: 0 }));
  check(env.doc.getElementById("modal").classList.contains("hide"), "restart gate closes after the cancel ack");
  check(cmds(ws).filter((c) => c.action === "RESTART").length === 1, "cancel never commits the restart");
}

async function testLocalLeaveArmedDisarmsWithCancelAck() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  const closeEvents = [];
  env.window.addEventListener("rebox-controller-close", (e) => closeEvents.push(e));
  hostBtn(env.doc, "quit").onclick();
  env.doc.getElementById("hg-primary").onclick(); // arm
  ws._msg(stateMsg("host", { quit_confirm: 1 })); // armed

  env.doc.getElementById("hg-leave").onclick();
  const last = cmds(ws)[cmds(ws).length - 1];
  check(last && last.action === "CANCEL", "leaving with an armed quit first cancels on the server");
  check(closeEvents.length === 0, "leave waits for the cancel ack before the close event");
  check(!env.doc.getElementById("modal").classList.contains("hide"), "leave keeps the gate visible until acked");

  ws._msg(stateMsg("host", { quit_confirm: 0 }));
  check(closeEvents.length === 1, "leave fires the close event once the core confirms the cancel");
  check(!env.doc.getElementById("left").classList.contains("hide"), "the leave screen is shown");
  check(env.store.get("jpq.host") === "HOSTTOKEN", "host identity is preserved across leave");
  check(cmds(ws).filter((c) => c.action === "QUIT").length === 1, "leaving never commits the armed quit");
}

async function testLocalLeaveCancelFailsClosedWhenDisconnected() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  const closeEvents = [];
  env.window.addEventListener("rebox-controller-close", (e) => closeEvents.push(e));
  hostBtn(env.doc, "quit").onclick();
  env.doc.getElementById("hg-primary").onclick(); // arm
  ws._msg(stateMsg("host", { quit_confirm: 1 }));
  env.doc.getElementById("hg-leave").onclick(); // sends CANCEL, waits
  ws._serverClose(); // drop before the ack
  check(closeEvents.length === 0, "a dropped cancel never leaves the controller");
  check(env.doc.getElementById("left").classList.contains("hide"), "the leave screen was not shown");
  check(/Disconnected/.test(env.doc.getElementById("modal").innerHTML), "a dropped leave-cancel shows clear feedback");
}

async function testDelayedReconnectNeverReopensAfterLeave() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authPlayer(env);
  ws._serverClose(); // schedules a reconnect
  const afterClose = env.wsCount();
  env.sandbox.window.ReboxJeopardy.requestExit();
  env.doc.getElementById("exit-leave").onclick();
  env.advance(5000); // well past the 1200 ms reconnect timer
  check(env.wsCount() === afterClose, "a left controller never reopens a socket from a late timer");
  check(env.store.get("jpq.token") === "PLAYERTOKEN", "the seat token survives the leave");
  env.doc.getElementById("left-return").onclick();
  const ws2 = env.ws();
  check(ws2 !== ws, "only Return to game opens a fresh socket");
  ws2._open();
  check(ws2.sent.some((m) => m.type === "hello"), "Return to game explicitly re-authenticates");
}

async function testEarlyCommitWhileCancellingNeverWire() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  hostBtn(env.doc, "quit").onclick();
  env.doc.getElementById("hg-primary").onclick(); // arm
  ws._msg(stateMsg("host", { quit_confirm: 1 })); // armed
  env.doc.getElementById("hg-cancel").onclick(); // cancelling
  const quits = cmds(ws).filter((c) => c.action === "QUIT").length;
  const cancels = cmds(ws).filter((c) => c.action === "CANCEL").length;
  env.doc.getElementById("hg-primary").onclick(); // stale commit attempt
  check(cmds(ws).filter((c) => c.action === "QUIT").length === quits, "a stale commit during cancel never sends QUIT");
  check(cmds(ws).filter((c) => c.action === "CANCEL").length === cancels, "cancel is never re-sent");
}

async function testDisconnectedCancelShowsFeedbackNoAutoQuit() {
  const env = makeEnv(fs.readFileSync(ASSET, "utf8"));
  boot(env);
  const ws = await authHost(env);
  hostBtn(env.doc, "quit").onclick();
  env.doc.getElementById("hg-primary").onclick(); // arm
  ws._msg(stateMsg("host", { quit_confirm: 1 })); // armed
  env.doc.getElementById("hg-cancel").onclick(); // cancelling
  ws._serverClose();
  check(!env.doc.getElementById("modal").classList.contains("hide"), "the gate stays up after a dropped cancel");
  check(/Disconnected/.test(env.doc.getElementById("modal").innerHTML), "a dropped cancel shows clear feedback");
  const prim = env.doc.getElementById("hg-primary");
  check(prim && !prim.disabled, "the gate falls back to a safe button, never a silent commit");
  env.advance(2000);
  const ws2 = env.ws();
  check(ws2 !== ws, "the phone reconnects");
  ws2._open();
  ws2._msg(stateMsg("host", { quit_confirm: 0 }));
  check(cmds(ws2).length === 0, "reconnect never auto-quits after a dropped cancel");
}

async function main() {
  const tests = [
    testHostQuitGate,
    testHostQuitCancel,
    testHostRestartGate,
    testPauseResumeAnyPhase,
    testPlayerNeverHostCommands,
    testRequestExitPlayerLocalLeave,
    testDisconnectPendingFailsClosed,
    testStaleEpochResetsGate,
    testBackDuringArmedQuitCancels,
    testBackDuringPendingQuitCancels,
    testBackDuringArmedRestartCancels,
    testLocalLeaveArmedDisarmsWithCancelAck,
    testLocalLeaveCancelFailsClosedWhenDisconnected,
    testDelayedReconnectNeverReopensAfterLeave,
    testEarlyCommitWhileCancellingNeverWire,
    testDisconnectedCancelShowsFeedbackNoAutoQuit,
    testNoDialogsOrTimedDoubleSend,
    testOrdinaryBrowserApiSurface,
  ];
  for (const t of tests) {
    try {
      await t();
    } catch (e) {
      check(false, `${t.name} threw: ${e && e.stack ? e.stack : e}`);
    }
  }
  console.log(`webpage: ${CHECKS} checks, ${FAILS} failures`);
  process.exit(FAILS ? 1 : 0);
}

main();
