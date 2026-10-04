"use strict";

const $ = (id) => document.getElementById(id);
const state = { libraries: [], parent: null, items: [], selected: null, playing: false, elapsed: 0, paused: false };

function toast(message) {
  const node = $("toast");
  node.textContent = message;
  node.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { node.hidden = true; }, 2600);
}

async function api(path, options) {
  const response = await fetch(path, options);
  let payload = {};
  try { payload = await response.json(); } catch (error) { payload = {}; }
  if (!response.ok) throw new Error(payload.error || ("HTTP " + response.status));
  return payload;
}

const post = (path, body) => api(path, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify(body || {}),
});

function runtime(ticks) {
  if (!ticks) return "";
  const total = Math.round(ticks / 1e7 / 60);
  const h = Math.floor(total / 60);
  const m = total % 60;
  return h ? h + "h " + m + "m" : m + "m";
}

function clock(seconds) {
  const s = Math.max(0, Math.floor(seconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const r = s % 60;
  const pad = (n) => String(n).padStart(2, "0");
  return h ? h + ":" + pad(m) + ":" + pad(r) : m + ":" + pad(r);
}

function card(item) {
  const node = document.createElement("button");
  node.className = "card";
  node.type = "button";

  const art = document.createElement("img");
  art.loading = "lazy";
  art.alt = "";
  art.src = "/art/" + item.id;
  art.addEventListener("error", () => {
    const fallback = document.createElement("div");
    fallback.className = "fallback";
    fallback.textContent = item.name;
    art.replaceWith(fallback);
  }, { once: true });
  node.appendChild(art);

  const cap = document.createElement("div");
  cap.className = "cap";
  const name = document.createElement("strong");
  name.textContent = item.name;
  const meta = document.createElement("span");
  meta.textContent = [item.year, runtime(item.runtime), item.type].filter(Boolean).join(" · ");
  cap.append(name, meta);
  node.appendChild(cap);

  node.addEventListener("click", () => showDetail(item));
  return node;
}

function showDetail(item) {
  state.selected = item;
  $("dName").textContent = item.name;
  $("dMeta").textContent = [item.year, runtime(item.runtime), item.type].filter(Boolean).join(" · ");
  $("dOverview").textContent = item.overview || "No description available.";
  $("dArt").src = "/art/" + item.id;
  $("dArt").hidden = false;
  $("dPlay").disabled = !item.playable;
  $("detail").hidden = false;
}

function renderItems(payload) {
  state.items = payload.items || [];
  $("heading").textContent = payload.total
    ? payload.total + " title" + (payload.total === 1 ? "" : "s")
    : "Library";
  const grid = $("grid");
  grid.replaceChildren(...state.items.map(card));
  $("empty").hidden = state.items.length > 0;
}

async function loadLibraries() {
  const payload = await api("/api/libraries");
  state.libraries = payload.libraries || [];
  const nav = $("libs");
  nav.replaceChildren();
  const all = document.createElement("button");
  all.type = "button";
  all.textContent = "All titles";
  all.addEventListener("click", () => selectLibrary(null, all));
  nav.appendChild(all);
  for (const library of state.libraries) {
    const button = document.createElement("button");
    button.type = "button";
    button.textContent = library.name;
    button.addEventListener("click", () => selectLibrary(library.id, button));
    nav.appendChild(button);
  }
  const first = state.libraries[0];
  selectLibrary(first ? first.id : null, first ? nav.children[1] : all);
}

async function selectLibrary(parent, button) {
  state.parent = parent;
  for (const node of $("libs").children) node.classList.remove("active");
  if (button) button.classList.add("active");
  $("search").value = "";
  await loadItems();
}

async function loadItems(search) {
  const query = new URLSearchParams();
  if (state.parent) query.set("parent", state.parent);
  if (search) query.set("search", search);
  renderItems(await api("/api/items?" + query.toString()));
}

async function play(item) {
  try {
    toast("Starting playback…");
    const result = await post("/api/play", { itemId: item.id });
    state.playing = true;
    state.elapsed = 0;
    state.paused = false;
    $("nowplaying").hidden = false;
    $("npLabel").textContent = "Now playing on TV";
    $("npName").textContent = result.name;
    $("detail").hidden = true;
    toast("Playing " + result.name + " on the TV");
  } catch (error) {
    toast("Play failed: " + error.message);
  }
}

async function refreshStatus() {
  try {
    const status = await api("/api/status");
    $("dot").className = "dot " + (status.receiver ? "ok" : "bad");
    $("who").textContent = (status.user ? status.user + " · " : "") +
      (status.receiver ? "receiver online" : "receiver offline");
    if (status.playing) {
      state.playing = true;
      state.elapsed = status.elapsed || 0;
      state.paused = !!status.paused;
      $("nowplaying").hidden = false;
      $("npLabel").textContent = state.paused ? "Paused on TV" : "Now playing on TV";
      if (!$("npName").textContent) $("npName").textContent = status.name;
    } else if (state.playing) {
      state.playing = false;
      state.paused = false;
      $("nowplaying").hidden = true;
      $("npName").textContent = "";
      $("npLabel").textContent = "Now playing on TV";
    }
  } catch (error) {
    $("dot").className = "dot bad";
    $("who").textContent = "remote server unreachable";
  }
}

$("searchForm").addEventListener("submit", async (event) => {
  event.preventDefault();
  const term = $("search").value.trim();
  $("heading").textContent = term ? "Results for “" + term + "”" : "Library";
  await loadItems(term || undefined);
});

$("dPlay").addEventListener("click", () => {
  if (state.selected) play(state.selected);
});

$("detailClose").addEventListener("click", () => { $("detail").hidden = true; });

$("npStop").addEventListener("click", () => transport("stop"));

for (const button of document.querySelectorAll("[data-key]")) {
  button.addEventListener("click", async () => {
    const key = button.dataset.key;
    button.classList.add("active");
    setTimeout(() => button.classList.remove("active"), 120);
    try {
      await post("/api/key", { key });
    } catch (error) {
      toast("Key failed: " + error.message);
    }
  });
}

const SEEK = { back: -15, rew: -60, fwd: 60 };

async function transport(action) {
  if (!state.playing && action !== "stop") {
    toast("Nothing is playing");
    return;
  }
  try {
    if (action in SEEK) {
      toast("Seeking…");
      await post("/api/seek", { delta: SEEK[action] });
      return;
    }
    const result = await post("/api/transport", { action });
    if (action === "stop") {
      state.playing = false;
      state.paused = false;
      $("nowplaying").hidden = true;
      $("npName").textContent = "";
      toast("Stopped");
      return;
    }
    state.paused = !!result.paused;
    toast(state.paused ? "Paused" : "Resumed");
    await refreshStatus();
  } catch (error) {
    toast("Transport failed: " + error.message);
  }
}

for (const button of document.querySelectorAll("[data-transport]")) {
  button.addEventListener("click", () => {
    button.classList.add("active");
    setTimeout(() => button.classList.remove("active"), 120);
    transport(button.dataset.transport);
  });
}

setInterval(() => {
  if (!state.playing) return;
  if (!state.paused) state.elapsed += 1;
  $("npTime").textContent = clock(state.elapsed);
}, 1000);

setInterval(refreshStatus, 5000);
refreshStatus();
loadLibraries().catch((error) => toast("Could not load libraries: " + error.message));
