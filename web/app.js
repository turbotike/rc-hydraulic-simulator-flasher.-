// DIYGuy999 configurator SPA.
// Renders from /api/schema, writes via /save and friends, flashes via the
// shared WebSerial flasher core.

import { streamBuild } from "/web/flasher.js";

const $ = (id) => document.getElementById(id);
const el = (tag, cls, html) => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (html != null) e.innerHTML = html;
  return e;
};
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
const post = async (url, body) => {
  const res = await fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
  const j = await res.json().catch(() => ({ ok: res.ok }));
  if (!res.ok || j.ok === false) throw new Error(j.error || ("Request failed (" + res.status + ")"));
  return j;
};

const FLASH = "__flash__", FORGE = "__soundforge__", GAMEPAD = "__gamepad__";

const state = {
  schema: null,
  activeTab: null,
  changes: {},   // { file: { name: {kind, enabled|value} } }
};

// ---------- dirty / toast ----------
let gpTouched = false; // Controls tab (gamepad config) has unsaved edits
const isDirty = () => gpTouched || Object.values(state.changes).some((f) => Object.keys(f).length);
const markDirty = () => { $("dirty").textContent = isDirty() ? "● unsaved changes" : ""; };
function recordChange(file, name, payload) { (state.changes[file] ||= {})[name] = payload; markDirty(); }
let toastTimer;
function toast(msg, kind = "") {
  const t = $("toast"); t.textContent = msg; t.className = "show " + kind;
  clearTimeout(toastTimer); toastTimer = setTimeout(() => (t.className = ""), 3200);
}

// ---------- data ----------
async function loadSchema() {
  const res = await fetch("/api/schema");
  const j = await res.json();
  if (!j.ok) throw new Error(j.error || "Failed to load configuration");
  state.schema = j.schema;
  state.changes = {};
  markDirty();
}
function vehicleFile() { return state.schema.currentVehicle ? "vehicles/" + state.schema.currentVehicle : null; }
function allTabs() {
  const tabs = [];
  if (state.schema.vehicleTab) tabs.push(state.schema.vehicleTab);
  tabs.push(...state.schema.tabs);
  return tabs;
}

// ---------- header / tab bar ----------
function renderVehicleSelect() {
  const sel = $("vehicleSel");
  sel.innerHTML = "";
  for (const v of state.schema.vehicles) {
    const o = el("option"); o.value = v; o.textContent = v.replace(/\.h$/, "");
    if (v === state.schema.currentVehicle) o.selected = true;
    sel.appendChild(o);
  }
}
function renderTabBar() {
  const nav = $("tabs"); nav.innerHTML = "";
  const add = (id, label) => {
    const b = el("button", "tab", label); b.dataset.id = id;
    if (id === state.activeTab) b.classList.add("active");
    b.onclick = () => { state.activeTab = id; render(); };
    nav.appendChild(b);
  };
  for (const t of allTabs()) add(t.id || t.file, esc(t.label));
  add(FORGE, "🔊 Sound Technician");
  add(GAMEPAD, "🎮 Controls");
  add(FLASH, "⚡ Flash");
}

// ---------- controls ----------
function effective(file, c) {
  const pending = (state.changes[file] || {})[c.name];
  let enabled = c.enabled, value = c.value;
  if (pending) {
    if ("enabled" in pending) enabled = pending.enabled;
    if ("value" in pending) { value = pending.value; if (c.saveKind === "bool_var") enabled = pending.value === "true"; }
  }
  return { enabled, value };
}
function controlInput(file, c) {
  const eff = effective(file, c);
  const wrap = el("div", "input");
  if (c.control === "toggle") {
    const sw = el("label", "switch"); const inp = el("input"); inp.type = "checkbox"; inp.checked = !!eff.enabled;
    inp.onchange = () => {
      if (c.saveKind === "bool_var") recordChange(file, c.name, { kind: "bool_var", value: inp.checked ? "true" : "false" });
      else if (c.saveKind === "define_val") recordChange(file, c.name, { kind: "define_val", enabled: inp.checked, value: c.value });
      else recordChange(file, c.name, { kind: "flag", enabled: inp.checked });
    };
    sw.appendChild(inp); sw.appendChild(el("span", "slider-ui")); wrap.appendChild(sw);
  } else if (c.control === "select") {
    const sel = el("select");
    for (const o of (c.options || [])) {
      const op = el("option"); op.value = o.value; op.textContent = o.label;
      if (String(eff.value) === String(o.value)) op.selected = true;
      sel.appendChild(op);
    }
    sel.onchange = () => recordChange(file, c.name, { kind: c.saveKind || "select", value: sel.value });
    wrap.appendChild(sel);
  } else if (c.control === "slider") {
    const valLbl = el("span", "val", esc(eff.value) + esc(c.suffix || ""));
    const inp = el("input"); inp.type = "range"; inp.min = c.min; inp.max = c.max; inp.step = c.step; inp.value = eff.value;
    inp.oninput = () => {
      valLbl.textContent = inp.value + (c.suffix || "");
      const p = c.saveKind === "define_val" ? { kind: "define_val", enabled: c.enabled !== false, value: inp.value } : { kind: c.saveKind, value: inp.value };
      recordChange(file, c.name, p);
    };
    wrap.appendChild(inp); wrap.appendChild(valLbl);
  } else {
    const inp = el("input"); inp.type = c.control === "number" ? "number" : "text"; inp.value = eff.value ?? ""; inp.style.width = "150px";
    inp.onchange = () => {
      const p = c.saveKind === "define_val" ? { kind: "define_val", enabled: c.enabled !== false, value: inp.value } : { kind: c.saveKind, value: inp.value };
      recordChange(file, c.name, p);
    };
    wrap.appendChild(inp);
  }
  return wrap;
}
function controlRow(file, c) {
  const row = el("div", "ctrl");
  const meta = el("div", "meta");
  meta.appendChild(el("div", "name", esc(c.label)));
  if (c.desc) meta.appendChild(el("div", "desc", esc(c.desc)));
  row.appendChild(meta);
  row.appendChild(controlInput(file, c));
  return row;
}

// ---------- vehicle actions + presets (shown atop Vehicle Tuning) ----------
function vehicleToolbar() {
  const bar = el("div", "toolbar");
  const v = state.schema.currentVehicle;

  // Save your vehicle to a shareable .h file.
  const save = el("button", null, "💾 Save vehicle");
  save.title = "Download this vehicle as a .h file you can share with others";
  save.onclick = () => { window.location = "/download_vehicle?vehicle=" + encodeURIComponent(v); };

  // Load a vehicle .h file someone shared with you.
  const load = el("button", null, "📂 Load vehicle");
  load.title = "Load a vehicle .h file (yours or one someone shared)";
  load.onclick = () => $("importFile").click();

  const reset = el("button", null, "↺ Reset vehicle");
  reset.title = "Restore this vehicle to its original factory settings";
  reset.onclick = async () => {
    if (!confirm("Reset " + v.replace(/\.h$/, "") + " to its original factory settings?")) return;
    try { await post("/reset_vehicle", { vehicle: v }); toast("Reset to factory.", "ok"); await reloadKeepTab(); }
    catch (e) { toast(e.message, "err"); }
  };

  bar.append(save, load, reset);
  return bar;
}

function presetBar() {
  const bar = el("div", "toolbar");
  bar.appendChild(el("span", "chip", "💾 Presets"));
  const sel = el("select"); sel.id = "presetSel";
  const presets = state.schema.presets || [];
  if (!presets.length) { const o = el("option"); o.textContent = "(none saved)"; o.value = ""; sel.appendChild(o); }
  for (const p of presets) { const o = el("option"); o.value = p; o.textContent = p; sel.appendChild(o); }
  bar.appendChild(sel);

  const v = state.schema.currentVehicle;
  const load = el("button", "mini", "Load");
  load.onclick = async () => {
    const name = sel.value; if (!name) return toast("No preset selected.");
    try {
      const j = await post("/preset_load", { vehicle: v, name });
      await post("/save", j.data);
      toast("Loaded preset “" + name + "”.", "ok");
      await reloadKeepTab();
    } catch (e) { toast(e.message, "err"); }
  };
  const save = el("button", "mini", "Save as…");
  save.onclick = async () => {
    const name = prompt("Preset name:"); if (!name) return;
    try { await post("/preset_save", { vehicle: v, name, data: vehicleSnapshot() }); toast("Saved preset “" + name + "”.", "ok"); await reloadKeepTab(); }
    catch (e) { toast(e.message, "err"); }
  };
  const del = el("button", "mini", "Delete");
  del.onclick = async () => {
    const name = sel.value; if (!name) return;
    if (!confirm("Delete preset “" + name + "”?")) return;
    try { await post("/preset_delete", { vehicle: v, name }); toast("Deleted.", "ok"); await reloadKeepTab(); }
    catch (e) { toast(e.message, "err"); }
  };
  bar.append(load, save, del);
  return bar;
}

// A preset captures the active vehicle's tuning + sound choices.
function vehicleSnapshot() {
  const vf = vehicleFile(); if (!vf) return {};
  const fields = {};
  for (const c of (state.schema.vehicleTab?.controls || [])) {
    if (c.saveKind === "flag") fields[c.name] = { kind: "flag", enabled: !!c.enabled };
    else if (c.saveKind === "bool_var") fields[c.name] = { kind: "bool_var", value: c.enabled ? "true" : "false" };
    else if (c.saveKind === "define_val") fields[c.name] = { kind: "define_val", enabled: c.enabled !== false, value: c.value };
    else fields[c.name] = { kind: "text_var", value: c.value };
  }
  for (const g of (state.schema.soundChoices || [])) {
    if (g.selected) fields["__sound__" + g.key] = { kind: "sound_choice", value: g.selected };
  }
  return { [vf]: fields };
}

async function reloadKeepTab() {
  const keep = state.activeTab;
  await loadSchema();
  renderVehicleSelect();
  state.activeTab = keep;
  render();
}

// ---------- panes ----------
function renderSettingsPane(tab) {
  const pane = el("div", "tabpane");
  pane.appendChild(el("h2", "pane-title", esc(tab.label)));
  pane.appendChild(el("p", "pane-sub", esc(tab.file)));
  if (state.schema.vehicleTab && tab.file === state.schema.vehicleTab.file) {
    pane.appendChild(vehicleToolbar());
  }
  if (!tab.controls.length) { pane.appendChild(el("div", "empty", "No adjustable settings here.")); return pane; }
  for (const c of tab.controls) pane.appendChild(controlRow(tab.file, c));
  return pane;
}

// ---- Sound Forge ----
let audioCtx;
async function previewSound(file) {
  try {
    const res = await fetch("/sound_pcm/" + encodeURIComponent(file));
    const j = await res.json();
    if (!j.ok) return toast(j.error || "Preview failed", "err");
    audioCtx ||= new (window.AudioContext || window.webkitAudioContext)();
    const buf = audioCtx.createBuffer(1, j.samples.length, j.sampleRate || 22050);
    const ch = buf.getChannelData(0);
    for (let i = 0; i < j.samples.length; i++) ch[i] = j.samples[i] / 128;
    const src = audioCtx.createBufferSource(); src.buffer = buf; src.connect(audioCtx.destination); src.start();
  } catch (e) { toast("Preview failed: " + e.message, "err"); }
}

// ---- Engine demo (browser preview of the whole sound pack) ----
const soundBufCache = {};
async function loadSoundBuffer(file) {
  if (soundBufCache[file]) return soundBufCache[file];
  const j = await (await fetch("/sound_pcm/" + encodeURIComponent(file))).json();
  if (!j.ok || !j.samples) throw new Error(j.error || "Couldn't load " + file);
  audioCtx ||= new (window.AudioContext || window.webkitAudioContext)();
  const buf = audioCtx.createBuffer(1, j.samples.length, j.sampleRate || 22050);
  const ch = buf.getChannelData(0);
  for (let i = 0; i < j.samples.length; i++) ch[i] = j.samples[i] / 128;
  soundBufCache[file] = buf; return buf;
}
function slotFile(slot) { // the currently-selected sound file for a slot
  const s = state.schema.sounds || {};
  if (s[slot]) return s[slot];
  const g = (state.schema.soundChoices || []).find((x) => x.key === slot);
  return g ? g.selected : null;
}
// Levels-aware mixing through a compressor bus (so it reacts to the Levels tab without clipping).
let demo = null, demoBus = null;
function demoAudioBus() {
  if (!audioCtx) return null;
  if (!demoBus) {
    const comp = audioCtx.createDynamicsCompressor();
    comp.threshold.value = -14; comp.knee.value = 14; comp.ratio.value = 6; comp.attack.value = 0.005; comp.release.value = 0.2;
    const g = audioCtx.createGain(); g.gain.value = 0.95;
    g.connect(comp); comp.connect(audioCtx.destination); demoBus = g;
  }
  return demoBus;
}
function demoMaster() { const L = state.schema.levels || {}; return Math.min(2.5, (L.masterVolume || 100) / 100) * 0.3; }
function demoLvl(key, d) { const L = state.schema.levels || {}; const v = L[key] != null ? L[key] : d; return Math.max(0, v / 100); }

async function demoStart(fadeInMs) {
  audioCtx ||= new (window.AudioContext || window.webkitAudioContext)();
  try { await audioCtx.resume(); } catch (_) {}
  demoStop();
  const bus = demoAudioBus();
  const idleF = slotFile("idleSound"), revF = slotFile("revSound"), startF = slotFile("startSound"), turboF = slotFile("turboSound");
  if (!idleF) { toast("No idle sound set.", "err"); return; }
  let crankDur = 0, crankStartAt = audioCtx.currentTime, crankGain = null;
  if (startF) { try { const b = await loadSoundBuffer(startF); crankDur = b.duration; const s = audioCtx.createBufferSource(); s.buffer = b; crankGain = audioCtx.createGain(); crankGain.gain.value = demoLvl("startVolumePercentage", 140) * demoMaster() * 1.4; s.connect(crankGain); crankGain.connect(bus); crankStartAt = audioCtx.currentTime; s.start(); } catch (_) { crankGain = null; } }
  const idleBuf = await loadSoundBuffer(idleF);
  const revBuf = revF ? await loadSoundBuffer(revF).catch(() => idleBuf) : idleBuf;
  const idleSrc = audioCtx.createBufferSource(); idleSrc.buffer = idleBuf; idleSrc.loop = true;
  const revSrc = audioCtx.createBufferSource(); revSrc.buffer = revBuf; revSrc.loop = true;
  const idleGain = audioCtx.createGain(), revGain = audioCtx.createGain();
  idleGain.connect(bus); revGain.connect(bus);
  idleSrc.connect(idleGain); revSrc.connect(revGain);
  idleSrc.start(); revSrc.start();
  demo = { idleSrc, revSrc, idleGain, revGain, throttle: 0, bog: false };
  if (turboF) { try {
    const tb = await loadSoundBuffer(turboF);
    const turboSrc = audioCtx.createBufferSource(); turboSrc.buffer = tb; turboSrc.loop = true;
    const turboGain = audioCtx.createGain(); turboGain.gain.value = 0; turboGain.connect(bus); turboSrc.connect(turboGain);
    turboSrc.start(); demo.turboSrc = turboSrc; demo.turboGain = turboGain;
  } catch (_) {} }
  if (fadeInMs) { // true crossfade: crank fades down over its tail while the idle loop comes up
    idleGain.gain.value = 0; revGain.gain.value = 0;
    const xf = Math.min(0.9, Math.max(0.35, crankDur * 0.45));   // crossfade length
    const startAt = crankStartAt + Math.max(0.15, crankDur - xf);
    const idleTarget = demoLvl("idleVolumePercentage", 100) * demoMaster();
    idleGain.gain.setValueAtTime(0, startAt);
    idleGain.gain.linearRampToValueAtTime(idleTarget, startAt + xf);
    if (crankGain) { crankGain.gain.setValueAtTime(crankGain.gain.value, startAt); crankGain.gain.linearRampToValueAtTime(0, startAt + xf); }
  } else { demoThrottle(0); }
}
// A looping effect (track rattle / pump / relief) with a smooth fade in/out.
function demoLoop(nodeRef, slot, vol, rate) {
  if (nodeRef.n || !audioCtx) return;
  const f = slotFile(slot); if (!f) return;
  loadSoundBuffer(f).then((b) => {
    if (nodeRef.n) return;
    const src = audioCtx.createBufferSource(); src.buffer = b; src.loop = true;
    if (rate) src.playbackRate.value = rate;
    const g = audioCtx.createGain(); g.gain.value = 0; g.connect(demoAudioBus()); src.connect(g);
    src.start(); nodeRef.n = { src, g };
    g.gain.setTargetAtTime(vol, audioCtx.currentTime, 0.18);
  }).catch(() => {});
}
function demoLoopStop(nodeRef) {
  if (!nodeRef.n) return;
  const n = nodeRef.n; nodeRef.n = null;
  try { n.g.gain.setTargetAtTime(0, audioCtx.currentTime, 0.12); setTimeout(() => { try { n.src.stop(); } catch (_) {} }, 300); } catch (_) {}
}
const demoTrackRef = { n: null };

// Hydrostatic drive whine — a hydraulic pump track looped and pitched UP a bunch to sing like a
// hydrostatic drive; the pitch rises with the swashplate. Level from the Levels tab.
const DEMO_WHINE_FILE = "cdcWhine.h"; // real hydrostatic whine recording (from cdc.wav)
const demoWhine = { src: null, filt: null, g: null };
let demoSwashLast = 0;
function demoStartWhine() {
  if (demoWhine.g || !audioCtx) return;
  const filt = audioCtx.createBiquadFilter(); filt.type = "lowpass"; filt.frequency.value = 3500; filt.Q.value = 1;
  const g = audioCtx.createGain(); g.gain.value = 0;
  filt.connect(g); g.connect(demoAudioBus());
  demoWhine.filt = filt; demoWhine.g = g;
  loadSoundBuffer(DEMO_WHINE_FILE).then((b) => {
    if (!demoWhine.g) return; // stopped before it loaded
    const src = audioCtx.createBufferSource(); src.buffer = b; src.loop = true;
    src.playbackRate.value = 0.8; src.connect(filt); src.start();
    demoWhine.src = src; demoSwash(demoSwashLast || 0.15);
  }).catch(() => {});
}
function demoStopWhine() {
  const w = demoWhine; if (!w.g) return;
  const src = w.src;
  try { w.g.gain.setTargetAtTime(0, audioCtx.currentTime, 0.15); } catch (_) {}
  setTimeout(() => { try { if (src) src.stop(); } catch (_) {} }, 400);
  w.src = w.filt = w.g = null;
}
// Whine rides the SWASHPLATE (pump displacement / drive command), NOT the track speed — so under
// load, when the tracks droop and slow, the whine holds up because the pump is still stroked hard.
function demoSwash(amount) {
  const a = Math.max(0, Math.min(1, amount));
  demoSwashLast = a;
  const w = demoWhine; if (!w.g) return;
  const now = audioCtx.currentTime;
  const lvl = demoLvl("hydrostaticWhineVolumePercentage", 120);
  const rate = 0.7 + a * 1.0;                                  // real whine: ~0.7x low stroke → ~1.7x full stroke
  // Slow glide (~0.5s) = the pump winding up as the swashplate strokes.
  if (w.src) { try { w.src.playbackRate.setTargetAtTime(rate, now, 0.5); } catch (_) {} }
  try { w.filt.frequency.setTargetAtTime(2500 + a * 4000, now, 0.5); } catch (_) {}
  try { w.g.gain.setTargetAtTime(demoMaster() * (0.06 + 0.34 * a) * lvl, now, 0.25); } catch (_) {}
}

function demoStartTracks() { demoStartWhine(); demoLoop(demoTrackRef, "trackRattleSound", demoLvl("trackRattleVolumePercentage", 100) * demoMaster(), 0.4); }
function demoStopTracks() { demoStopWhine(); demoLoopStop(demoTrackRef); }
// Rattle pace rides the actual track speed (0 = crawling, 1 = full pace) — droops under load.
function demoTrackRate(speed) {
  const s = Math.max(0, Math.min(1, speed));
  if (!demoTrackRef.n) return;
  const r = 0.38 + s * 0.22; // ~0.38 crawling → ~0.60 full pace (tamed top end)
  const g = demoLvl("trackRattleVolumePercentage", 100) * demoMaster() * (0.5 + 0.5 * s);
  try { demoTrackRef.n.src.playbackRate.setTargetAtTime(r, audioCtx.currentTime, 0.2); } catch (_) {}
  try { demoTrackRef.n.g.gain.setTargetAtTime(g, audioCtx.currentTime, 0.2); } catch (_) {}
}
// Implement pump whine — same physics as the HST whine: on this machine the hydraulic pump is
// basically the same as the hydrostatic one, so the whine comes from the PUMP STROKE (implement flow
// demand), pitching and swelling with how hard it strokes. Silent at zero stroke → quiet while just
// traveling (no implement flow), loud when a function is moving.
const demoPump = { src: null, g: null, stroke: 0 };
function demoStartPump() {
  if (demoPump.g || !audioCtx) return;
  const f = slotFile("hydraulicPumpSound"); if (!f) return;
  const g = audioCtx.createGain(); g.gain.value = 0; g.connect(demoAudioBus());
  demoPump.g = g;
  loadSoundBuffer(f).then((b) => {
    if (!demoPump.g) return; // stopped before it loaded
    const src = audioCtx.createBufferSource(); src.buffer = b; src.loop = true;
    src.connect(g); src.start(); // NATIVE pitch — the hydraulic sound itself is untouched
    demoPump.src = src; demoPumpStroke(demoPump.stroke);
  }).catch(() => {});
}
function demoStopPump() {
  const w = demoPump; if (!w.g) return;
  const src = w.src; w.stroke = 0;
  try { w.g.gain.setTargetAtTime(0, audioCtx.currentTime, 0.15); } catch (_) {}
  setTimeout(() => { try { if (src) src.stop(); } catch (_) {} }, 400);
  w.src = w.g = null;
}
// Pump stroke 0..1 = implement flow demand: only the VOLUME swells with the stroke (silent at zero).
// The hydraulic sound itself is left at native pitch — not changed.
function demoPumpStroke(amount) {
  const a = Math.max(0, Math.min(1, amount));
  demoPump.stroke = a;
  const w = demoPump; if (!w.g) return;
  const lvl = demoLvl("hydraulicPumpVolumePercentage", 100);
  try { w.g.gain.setTargetAtTime(demoMaster() * lvl * a, audioCtx.currentTime, 0.2); } catch (_) {}
}
function demoThrottle(t) { // crossfade idle→rev, spool the turbo, pitch up — all scaled by the Levels tab
  if (!demo) return;
  demo.throttle = t;
  const now = audioCtx.currentTime, m = demoMaster();
  demo.idleGain.gain.setTargetAtTime(demoLvl("idleVolumePercentage", 100) * m * Math.max(0, 1 - t * 0.85), now, 0.08);
  demo.revGain.gain.setTargetAtTime(demoLvl("revVolumePercentage", 110) * m * t, now, 0.08);
  if (demo.turboGain) demo.turboGain.gain.setTargetAtTime(demoLvl("turboVolumePercentage", 90) * m * Math.pow(t, 1.4), now, 0.12);
  // Native pitch at idle (matches the preview); revs up with throttle; sags when bogged.
  const rate = (demo.bog ? 0.8 : 1.0) + t * 0.4;
  demo.idleSrc.playbackRate.setTargetAtTime(rate, now, demo.bog ? 0.05 : 0.1);
  demo.revSrc.playbackRate.setTargetAtTime(rate, now, demo.bog ? 0.05 : 0.1);
  if (demo.turboSrc) demo.turboSrc.playbackRate.setTargetAtTime(1.0 + t * 0.4, now, 0.12);
}
function demoBog(on) { if (demo) { demo.bog = on; demoThrottle(demo.throttle); } }
function demoStop() {
  demoStopPump(); demoStopTracks();
  if (demo) { try { demo.idleSrc.stop(); demo.revSrc.stop(); if (demo.turboSrc) demo.turboSrc.stop(); } catch (_) {} demo = null; }
}
// Automatic diesel wind-down: rpm/pitch sag to a stall and the volume tails off, then everything
// stops. Returns a promise so the caller can wait for the shutdown to finish.
function demoShutdown() {
  return new Promise((resolve) => {
    if (!demo || !audioCtx) { demoStop(); resolve(); return; }
    const now = audioCtx.currentTime, dn = 1.2; // wind-down time
    demoStopPump(); demoStopTracks();
    [demo.idleSrc, demo.revSrc, demo.turboSrc].forEach((s) => {
      if (s) try { s.playbackRate.setValueAtTime(s.playbackRate.value, now); s.playbackRate.exponentialRampToValueAtTime(0.18, now + dn); } catch (_) {}
    });
    [demo.idleGain, demo.revGain, demo.turboGain].forEach((g) => {
      if (g) try { g.gain.setTargetAtTime(0, now + dn * 0.45, 0.3); } catch (_) {}
    });
    setTimeout(() => { demoStop(); resolve(); }, (dn + 0.35) * 1000);
  });
}

// Scripted auto-demo (your walkaround): crank → crossfade to idle → throttle up → hold →
// lower the blade → drive forward → stop → back to idle → off.
let demoAutoRunning = false;
async function demoAuto(onThrottle, onStage) {
  if (demoAutoRunning) return;
  demoAutoRunning = true;
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const stage = (s) => { if (onStage) onStage(s); };
  const alive = () => demoAutoRunning && demo;
  const glide = async (from, to, ms) => {
    const steps = Math.max(1, Math.round(ms / 60));
    for (let i = 1; i <= steps && alive(); i++) {
      const t = from + (to - from) * i / steps;
      demoThrottle(t); if (onThrottle) onThrottle(t);
      await sleep(60);
    }
  };
  // ramp the track-rattle pace (0..1) over ms; re-applies so it catches once the loop loads
  const trackTo = async (from, to, ms) => {
    const steps = Math.max(1, Math.round(ms / 80));
    for (let i = 1; i <= steps && alive(); i++) { demoTrackRate(from + (to - from) * i / steps); await sleep(80); }
  };
  try {
    stage("🔑 Starting up…");          await demoStart(1500); if (!demo) return; // crank crossfades into idle
    await sleep(4500);                                                            // hold idle a good while
    if (!alive()) return;
    stage("🔺 Throttle up");           await glide(0, 0.85, 1300);                // idle up
    await sleep(1200);                                                            // hold
    if (!alive()) return;
    stage("🔧 Lowering the blade");    demoStartPump(); demoPumpStroke(0.75);    // pump strokes to feed the blade valve
    await sleep(1300); demoPumpStroke(0.2); await sleep(500);                     // eases as the blade settles
    if (!alive()) return;
    stage("🚜 Driving forward");       demoStopPump(); demoStartTracks(); demoSwash(0.9); await glide(0.85, 1.0, 700); // no implement flow → pump quiet
    await trackTo(0.3, 1.0, 900); demoSwash(1.0);                                 // swash strokes up, tracks pick up
    if (!alive()) return;
    // Push into the pile — swash stays stroked FULL (whine holds) but the tracks droop and slow.
    stage("💥 Digging in — she's bogging!");
    demoStartPump(); demoPumpStroke(1.0); demoBog(true); demoSwash(1.0);          // pump maxed against the cut, engine bogs
    await trackTo(1.0, 0.3, 1300);                                                // tracks lug down; whine holds up
    await sleep(1300);
    if (!alive()) return;
    stage("😮‍💨 Pulling out");          demoBog(false); demoPumpStroke(0.6); demoSwash(1.0); // blade raising out of the cut
    await trackTo(0.3, 0.95, 1100);                                               // catches its breath, rolls on
    stage("🛑 Stop");                  demoSwash(0); demoStopPump(); await trackTo(0.95, 0.2, 500); demoStopTracks(); await sleep(700); // blade's up, pump off
    stage("🔻 Back to idle");          await glide(1.0, 0, 1500); if (onThrottle) onThrottle(0);
    await sleep(1400);
    stage("🔌 Shutting down");         await demoShutdown();
    await sleep(200); stage("");
  } finally { demoAutoRunning = false; }
}

function renderForgePane() {
  const pane = el("div", "tabpane");
  pane.appendChild(el("h2", "pane-title", "🔊 Sound Technician"));
  pane.appendChild(el("p", "pane-sub", "Master volume and engine sound selection for " + esc(state.schema.currentVehicle || "—")));

  // Master volume + pot override
  const vcard = el("div", "card");
  vcard.innerHTML = `
    <div class="row">
      <strong style="min-width:130px">Master Volume</strong>
      <input type="range" id="masterVol" min="0" max="300" step="5" value="100">
      <span class="val" id="masterVolVal">100%</span>
    </div>`;
  pane.appendChild(vcard);

  // Engine demo — hear the pack without flashing
  const demoCard = el("div", "card");
  demoCard.innerHTML = `
    <div class="sound-cat">🎧 Demo — hear your sound pack</div>
    <p class="pane-sub">Hear the whole thing in the browser, no flashing needed. Uses the sounds you've got selected.</p>
    <div class="row" style="gap:12px;flex-wrap:wrap;align-items:center">
      <button id="demoAutoBtn" class="primary">🎬 Auto demo</button>
      <span id="demoStage" class="pane-sub" style="margin:0;font-style:normal"></span>
    </div>
    <p class="pane-sub" style="margin:10px 0 0">Auto demo: cranks up → idles → throttles up → lowers the blade → drives forward → <b>digs in and bogs down</b> → pulls out → back to idle → shuts off. Reacts to your Levels.</p>
    <hr style="border:0;border-top:1px solid var(--line);margin:16px 0">
    <div class="row" style="gap:12px;flex-wrap:wrap">
      <button id="demoStartBtn">▶ Start engine</button>
      <button id="demoStopBtn">⏹ Stop</button>
    </div>
    <div class="row" style="margin-top:14px">
      <strong style="min-width:90px">Throttle</strong>
      <input type="range" id="demoThr" min="0" max="100" step="1" value="0" disabled>
      <span class="val" id="demoThrVal">idle</span>
    </div>`;
  pane.appendChild(demoCard);

  // Sound choosers
  const choices = state.schema.soundChoices || [];
  if (choices.length) {
    const vf = vehicleFile();
    const card = el("div", "card");
    card.appendChild(el("div", "sound-cat", "Engine &amp; effect sounds"));
    for (const g of choices) {
      const row = el("div", "ctrl");
      const meta = el("div", "meta");
      meta.appendChild(el("div", "name", esc(g.title)));
      row.appendChild(meta);
      const input = el("div", "input");
      const sel = el("select");
      for (const o of g.options) { const op = el("option"); op.value = o.file; op.textContent = o.label; if (o.file === g.selected) op.selected = true; sel.appendChild(op); }
      const pending = (state.changes[vf] || {})["__sound__" + g.key];
      if (pending) sel.value = pending.value;
      sel.onchange = () => recordChange(vf, "__sound__" + g.key, { kind: "sound_choice", value: sel.value });
      const play = el("button", "mini", "▶");
      play.title = "Preview selected"; play.onclick = () => previewSound(sel.value);
      const add = el("button", "mini", "＋ Change");
      add.title = "Pick from the library or upload your own WAV";
      add.onclick = () => openSoundModal(g);
      input.append(sel, play, add);
      // Delete button for custom sounds
      const selOpt = g.options.find((o) => o.file === sel.value);
      if (selOpt && /custom/i.test(selOpt.label)) {
        const del = el("button", "mini", "🗑");
        del.title = "Delete this custom sound";
        del.onclick = async () => {
          if (!confirm("Delete custom sound " + sel.value + "?")) return;
          try { await post("/delete_sound", { filename: sel.value }); toast("Deleted.", "ok"); await reloadKeepTab(); }
          catch (e) { toast(e.message, "err"); }
        };
        input.append(del);
      }
      row.appendChild(input);
      card.appendChild(row);
    }
    pane.appendChild(card);
  }
  return pane;
}

// ---- Sound library modal (browse / preview / install / upload WAV) ----
let allSoundsCache = null;
let pendingWavGroup = null;
async function getAllSounds() {
  if (allSoundsCache) return allSoundsCache;
  try { const j = await (await fetch("/all_sounds")).json(); allSoundsCache = j.ok ? j.sounds : []; }
  catch (_) { allSoundsCache = []; }
  return allSoundsCache;
}
function closeModal() { $("modal").innerHTML = ""; }

async function openSoundModal(group) {
  const bg = el("div", "modal-bg");
  bg.onclick = (e) => { if (e.target === bg) closeModal(); };
  const cat = group.category || "";   // library category for this slot ("" = none)
  const m = el("div", "modal");
  m.innerHTML = `
    <div class="modal-head"><h3>Sound for: ${esc(group.title)}</h3><button class="modal-x">✕</button></div>
    <div class="modal-body">
      <input class="search" id="sndSearch" type="text" placeholder="Search…">
      <div class="filter-row">
        <span id="sndCount" class="muted-sm"></span>
        <div class="spacer"></div>
        <label class="opt${cat ? "" : " hidden"}"><input type="checkbox" id="showAll"> Show all sounds</label>
      </div>
      <div class="snd-list" id="sndList"><div class="empty">Loading…</div></div>
      <p class="hint-row">Or add your own — a WAV is converted automatically (mono, 22050 Hz works best).</p>
    </div>
    <div class="modal-foot">
      <button id="uploadWav" class="primary">⬆ Upload WAV</button>
      <div class="spacer"></div>
      <button class="modal-close">Close</button>
    </div>`;
  bg.appendChild(m);
  $("modal").innerHTML = ""; $("modal").appendChild(bg);
  m.querySelector(".modal-x").onclick = closeModal;
  m.querySelector(".modal-close").onclick = closeModal;
  $("uploadWav").onclick = () => { pendingWavGroup = group; $("wavFile").click(); };

  const list = $("sndList");
  const all = await getAllSounds();
  const search = $("sndSearch");
  const showAll = $("showAll");
  const count = $("sndCount");

  const renderList = () => {
    const q = search.value.trim().toLowerCase();
    let items = all;
    let scoped = false; // true when narrowed to this slot's category
    // Default: only sounds for this slot's category. "Show all" or a search overrides.
    if (cat && !showAll.checked && !q) {
      const inCat = all.filter((s) => (s.category || "") === cat);
      if (inCat.length) { items = inCat; scoped = true; } // else fall back to all
    }
    if (q) items = items.filter((s) => s.label.toLowerCase().includes(q) || (s.category || "").includes(q));
    count.textContent = items.length + (items.length === 1 ? " sound" : " sounds")
      + (scoped ? " for this slot" : (cat && !q && !showAll.checked ? " (none tagged for this slot — showing all)" : ""));
    list.innerHTML = "";
    if (!items.length) { list.appendChild(el("div", "empty", "No matching sounds. Try “Show all”.")); return; }
    for (const s of items.slice(0, 150)) {
      const row = el("div", "snd");
      row.appendChild(el("div", "nm", esc(s.label)));
      if (s.category && s.category !== "other") row.appendChild(el("span", "tag", esc(s.category)));
      const play = el("button", "mini", "▶"); play.onclick = () => previewSound(s.file);
      const use = el("button", "mini primary", "Use"); use.onclick = () => useLibrarySound(s.file, group);
      const del = el("button", "mini", "🗑"); del.title = "Delete this sound file";
      del.onclick = async () => {
        if (!confirm("Delete " + s.label + " permanently?")) return;
        try {
          await post("/delete_sound", { filename: s.file });
          const i = all.indexOf(s); if (i >= 0) all.splice(i, 1);
          allSoundsCache = null; // invalidate the shared cache
          toast("Deleted " + s.label, "ok"); renderList();
        } catch (e) { toast(e.message, "err"); }
      };
      row.append(play, use, del);
      list.appendChild(row);
    }
    if (items.length > 150) list.appendChild(el("div", "hint-row", "Showing first 150 — type to narrow it down."));
  };
  search.oninput = renderList;
  if (showAll) showAll.onchange = renderList;
  renderList();
}

async function useLibrarySound(file, group) {
  // The file already exists in the library — just assign it to this slot and save.
  try {
    await post("/save", { "config.h": { ["__sound__" + group.key]: { kind: "sound_choice", value: file } } });
    closeModal(); toast(group.title + " → " + file.replace(/\.h$/, ""), "ok");
    await reloadKeepTab();
  } catch (e) { toast(e.message, "err"); }
}

// bitluni-style WAV → C header, named for the target slot's variable prefix.
function wavToHeader(audioBuffer, varPrefix) {
  let buffer = Float32Array.from(audioBuffer.getChannelData(0));
  for (let c = 1; c < audioBuffer.numberOfChannels; c++) {
    const cb = audioBuffer.getChannelData(c);
    for (let i = 0; i < buffer.length; i++) buffer[i] += cb[i];
  }
  const target = 22050;
  let sampleRate = audioBuffer.sampleRate;
  const scale = audioBuffer.sampleRate / target;
  if (scale > 1.001 || scale < 0.999) {
    const len = Math.floor((buffer.length - 1) / scale);
    const b = new Float32Array(len);
    for (let i = 0; i < len; i++) b[i] = buffer[Math.floor(i * scale)];
    buffer = b; sampleRate = target;
  }
  const p = varPrefix || "";
  const arr = p ? p + "Samples" : "samples";
  const rate = p ? p + "SampleRate" : "sampleRate";
  const count = p ? p + "SampleCount" : "sampleCount";
  let max = 0; for (let i = 0; i < buffer.length; i++) max = Math.max(Math.abs(buffer[i]), max); if (!max) max = 1;
  const out = new Array(buffer.length);
  for (let i = 0; i < buffer.length; i++) { let o = Math.round(buffer[i] / max * 127); out[i] = o > 127 ? 127 : o < -128 ? -128 : o; }
  return "const unsigned int " + rate + " = " + sampleRate + ";\r\n" +
    "const unsigned int " + count + " = " + buffer.length + ";\r\n" +
    "const signed char " + arr + "[] = {\r\n" + out.join(", ") + "\r\n};\r\n";
}

async function handleWavFile(file) {
  const group = pendingWavGroup; pendingWavGroup = null;
  if (!group) return;
  try {
    toast("Converting WAV…");
    audioCtx ||= new (window.AudioContext || window.webkitAudioContext)();
    const audio = await audioCtx.decodeAudioData(await file.arrayBuffer());
    const base = file.name.replace(/\.[^.]+$/, "").replace(/[^A-Za-z0-9_]/g, "_") || "customSound";
    // Unique array name per file (from the filename) so uploads never collide with another slot.
    const prefix = base.replace(/[^A-Za-z0-9]/g, "") || "customSound";
    const text = wavToHeader(audio, prefix.charAt(0).toLowerCase() + prefix.slice(1));
    await post("/install_header", { filename: base + ".h", text, category: group.key });
    await post("/save", { "config.h": { ["__sound__" + group.key]: { kind: "sound_choice", value: base + ".h" } } });
    closeModal(); toast("Added " + base + " → " + group.title, "ok");
    await reloadKeepTab();
  } catch (e) { toast("WAV import failed: " + e.message, "err"); }
}

function wireForgePane() {
  // Master volume
  fetch("/get_volume").then((r) => r.json()).then((j) => {
    if (!j.ok) return;
    const vol = $("masterVol"), val = $("masterVolVal");
    if (vol) { vol.value = j.volume; val.textContent = j.volume + "%"; }
  }).catch(() => {});
  let volTimer;
  $("masterVol").oninput = (e) => {
    $("masterVolVal").textContent = e.target.value + "%";
    clearTimeout(volTimer);
    volTimer = setTimeout(() => post("/set_volume", { volume: parseInt(e.target.value, 10) }).then(() => toast("Volume saved.", "ok")).catch((err) => toast(err.message, "err")), 500);
  };

  // --- Engine demo wiring ---
  demoStop(); // stop any prior demo when this pane re-renders
  const thr = $("demoThr"), thrVal = $("demoThrVal"), stageEl = $("demoStage");
  const setThr = (t) => { const p = Math.round(t * 100); thr.value = p; thrVal.textContent = p === 0 ? "idle" : p + "%"; };
  $("demoStartBtn").onclick = async () => { await demoStart(1200); if (demo) { thr.disabled = false; setThr(0); toast("Engine running — work the throttle.", "ok"); } };
  $("demoStopBtn").onclick = () => { demoStop(); thr.disabled = true; setThr(0); };
  thr.oninput = () => { const t = thr.value / 100; demoThrottle(t); thrVal.textContent = thr.value == 0 ? "idle" : thr.value + "%"; };
  $("demoAutoBtn").onclick = () => {
    thr.disabled = true;
    demoAuto(setThr, (s) => { if (stageEl) stageEl.textContent = s; });
  };
}

function renderFlashPane() {
  const pane = el("div", "tabpane");
  pane.innerHTML = `
    <h2 class="pane-title">⚡ Flash your board</h2>
    <ol class="steps">
      <li><span class="warn">Disconnect the battery</span> from the controller (GPIO12 must be free).</li>
      <li>Plug the ESP32 into USB with a <em>data</em> cable.</li>
      <li><strong>Detect board</strong>, pick your port, then <strong>Flash</strong>.</li>
    </ol>

    <div class="card">
      <div class="row">
        <button id="detectBtn">🔍 Detect board</button>
        <select id="nativePort" style="min-width:210px"><option value="">— click Detect board —</option></select>
        <button id="nativeFlash" class="primary">🔌 Flash</button>
        <div class="spacer"></div>
        <button id="doBuild" title="Just compile — check for errors without uploading">Build only</button>
      </div>
    </div>

    <div id="status" class="status">Ready. Disconnect the battery, plug in USB, then Detect board.</div>
    <div class="progress"><div id="bar"></div></div>
    <details class="logwrap"><summary>Show details (for troubleshooting)</summary>
      <div class="card"><pre id="log" class="log">Log output appears here…</pre></div>
    </details>`;
  return pane;
}

// ---- Controls (RC transmitter vs game controller) ----
// Hydraulic mode (Machine tab, pumpFromRpm) forces an RC receiver — a Bluetooth pad can't run the
// hydraulic valves. Read the toggle's live value (including an unsaved change) from the schema.
function isHydraulicMode() {
  for (const tab of (state.schema.tabs || [])) {
    for (const c of (tab.controls || [])) {
      if (c.name === "pumpFromRpm") return !!effective(tab.file, c).enabled;
    }
  }
  return false;
}
function renderGamepadPane() {
  const pane = el("div", "tabpane");
  pane.innerHTML = `
    <h2 class="pane-title">🎮 Controls</h2>
    <p class="pane-sub">Choose how you drive the machine — your RC transmitter, or a Bluetooth game
      controller. Set it and flash; this is a set-and-go build with nothing to tune on the machine.</p>
    <div id="gpRoot"><div class="empty">Loading…</div></div>`;
  return pane;
}

// local working copy of the controls config (mirrors the server, saved on demand)
let gpCfg = null;

function wireGamepadPane() {
  const root = document.getElementById("gpRoot");
  fetch("/gamepad_config").then((r) => r.json()).then((j) => {
    if (!j.ok) { root.innerHTML = `<div class="empty">Couldn't load controls: ${esc(j.error || "")}</div>`; return; }
    gpCfg = j.config;
    gpTouched = false;                       // freshly loaded — nothing to save yet
    // Any input change in the Controls tab marks it dirty (saved by the top Save button).
    root.addEventListener("change", () => { gpTouched = true; markDirty(); });
    buildGamepadUI(root);
  }).catch((e) => { root.innerHTML = `<div class="empty">Couldn't load controls: ${esc(e.message)}</div>`; });
}

function buildGamepadUI(root) {
  const c = gpCfg;
  root.innerHTML = "";

  const hydraulic = isHydraulicMode();
  if (hydraulic && c.mode === "gamepad") { c.mode = "webui"; gpTouched = true; markDirty(); } // real hydraulic build is RC only

  // --- Mode picker: two big, obvious cards (gamepad hidden in hydraulic mode) ---
  const modeWrap = el("div", "gpmodes");
  const mk = (id, icon, title, sub) => {
    const card = el("div", "gpmode" + (c.mode === id ? " sel" : ""));
    card.innerHTML = `<div class="gpmode-ic">${icon}</div><div class="gpmode-t">${title}</div><div class="gpmode-s">${sub}</div>`;
    card.onclick = () => { c.mode = id; buildGamepadUI(root); };
    return card;
  };
  modeWrap.append(mk("webui", "🎚️", "RC transmitter", "Drive with your normal RC radio. Pick your protocol below."));
  if (!hydraulic) modeWrap.append(mk("gamepad", "🎮", "Game controller", "Drive with a PS4 / PS5 / Xbox pad over Bluetooth."));
  root.appendChild(modeWrap);

  if (hydraulic) root.appendChild(el("div", "gpbadge",
    "🔧 Hydraulic mode is on (Machine tab) — this build runs on the RC receiver, so gamepad is disabled. Turn Hydraulic mode off to use a controller."));

  const gpOnly = c.mode === "gamepad";
  if (gpOnly) {
    root.appendChild(el("div", "gpbadge", "🎮 Pair with SHARE+PS. Generic/clone controllers can be slow to connect — a genuine DS4/DualSense pairs fastest. Hold BOOT ~3s to re-pair a new one."));
  } else {
    // RC transmitter → pick which bus your receiver uses (moved off the Machine tab).
    const rcCard = el("div", "card");
    rcCard.appendChild(el("div", "sound-cat", "RC protocol"));
    rcCard.appendChild(el("p", "pane-sub", "How your RC receiver sends its channels to the board."));
    const row = el("div", "ctrl");
    const meta = el("div", "meta"); meta.appendChild(el("div", "name", "Receiver protocol"));
    row.appendChild(meta);
    const inp = el("div", "input");
    const selEl = el("select");
    [["IBUS_COMMUNICATION", "IBUS (FlySky)"], ["SBUS_COMMUNICATION", "SBUS (Futaba / FrSky)"],
     ["PWM_COMMUNICATION", "PWM (separate channel wires)"]].forEach(([v, l]) => {
      const o = el("option"); o.value = v; o.textContent = l; selEl.appendChild(o);
    });
    selEl.value = c.rcProtocol || "IBUS_COMMUNICATION";
    selEl.onchange = () => { c.rcProtocol = selEl.value; };
    inp.appendChild(selEl); row.appendChild(inp); rcCard.appendChild(row);
    root.appendChild(rcCard);
  }

  // --- Reverse output direction (both modes) — for motors/actuators wired backwards ---
  {
    const rc = el("div", "card");
    rc.appendChild(el("div", "sound-cat", "Reverse output direction"));
    rc.appendChild(el("p", "pane-sub", "Motors and actuators get wired whichever way they land. If an output runs the wrong way on the bench, flip it here — it mirrors that channel around center."));
    c.outputReversed = c.outputReversed || [false, false, false, false, false, false];
    const revRow = (idx, label) => {
      const row = el("div", "ctrl");
      const meta = el("div", "meta"); meta.appendChild(el("div", "name", esc(label)));
      row.appendChild(meta);
      const input = el("div", "input");
      const sw = el("label", "switch"); const inp = el("input"); inp.type = "checkbox";
      inp.checked = !!c.outputReversed[idx];
      inp.onchange = () => { c.outputReversed[idx] = inp.checked; };
      sw.appendChild(inp); sw.appendChild(el("span", "slider-ui")); input.appendChild(sw);
      row.appendChild(input); return row;
    };
    const drv = c.driveOutputs || [];
    if (drv[0]) rc.appendChild(revRow(0, drv[0][1] + " (" + drv[0][0] + ")"));
    if (drv[1]) rc.appendChild(revRow(1, drv[1][1] + " (" + drv[1][0] + ")"));
    (c.outputList || []).forEach(([_k, label], i) => rc.appendChild(revRow(2 + i, label)));
    root.appendChild(rc);
  }

  // --- RC mode: read-only reference of what this machine's 6 outputs do ---
  if (!gpOnly) {
    const card = el("div", "card");
    card.appendChild(el("div", "sound-cat", "This machine's 6 outputs"));
    card.appendChild(el("p", "pane-sub", "With an RC transmitter, the ESP32 drives these six outputs (CH1–CH6) for the machine picked on the Machine tab. Drive is handled automatically; the implements follow their sticks."));
    const outRow = (pin, label) => {
      const row = el("div", "ctrl");
      const meta = el("div", "meta"); meta.appendChild(el("div", "name", esc(label)));
      row.appendChild(meta);
      const input = el("div", "input");
      const tag = el("span", "val"); tag.textContent = pin; input.appendChild(tag);
      row.appendChild(input);
      return row;
    };
    for (const [pin, label] of (c.driveOutputs || [])) card.appendChild(outRow(pin, label));
    for (const [key, label] of (c.outputList || [])) {
      const m = String(label).match(/^(.*?)\s*\(([^)]+)\)\s*$/);
      card.appendChild(outRow(m ? m[2] : "", m ? m[1] : label));
    }
    root.appendChild(card);
    card.appendChild(el("p", "hint-row", "Want to drive it with a PS4/PS5/Xbox pad instead? Pick “Game controller” above to map every implement to a stick, trigger, or button."));
  }

  // --- Button map (gamepad only) ---
  if (gpOnly) {
    const card = el("div", "card");
    card.appendChild(el("div", "sound-cat", "Button map"));
    card.appendChild(el("p", "pane-sub", "Pick which controller button triggers each function."));
    for (const [name, label] of c.functions) {
      const row = el("div", "ctrl");
      const meta = el("div", "meta"); meta.appendChild(el("div", "name", esc(label)));
      row.appendChild(meta);
      const input = el("div", "input");
      const sel = el("select");
      for (const [mask, blabel] of c.buttonChoices) {
        const o = el("option"); o.value = mask; o.textContent = blabel;
        if (parseInt(mask, 16) === parseInt(c.buttons[name], 16)) o.selected = true;
        sel.appendChild(o);
      }
      sel.onchange = () => { c.buttons[name] = sel.value; };
      input.appendChild(sel); row.appendChild(input);
      card.appendChild(row);
    }
    root.appendChild(card);
  }

  // --- Drive options (gamepad only) ---
  if (gpOnly) {
    const card = el("div", "card");
    card.appendChild(el("div", "sound-cat", "Drive feel"));
    const toggle = (label, key, hint) => {
      const row = el("div", "ctrl");
      const meta = el("div", "meta"); meta.appendChild(el("div", "name", esc(label)));
      if (hint) meta.appendChild(el("div", "desc", esc(hint)));
      row.appendChild(meta);
      const input = el("div", "input");
      const sw = el("label", "switch"); const inp = el("input"); inp.type = "checkbox"; inp.checked = !!c[key];
      inp.onchange = () => { c[key] = inp.checked; };
      sw.appendChild(inp); sw.appendChild(el("span", "slider-ui")); input.appendChild(sw);
      row.appendChild(input); return row;
    };
    // (The old "Tank / dual-track mix" toggle was removed — the drive mode is set once on the
    //  Machine tab ("Dozer drive"); the firmware always mixes off that, so this was a dead duplicate.)
    card.appendChild(toggle("Engine-feel rumble", "rumble",
      "Feel the engine through the controller — idle purr, load-follow, and a jolt when it bogs under load. Turn off to save controller battery. (PS4/PS5/Xbox.)"));

    // steering source
    const srow = el("div", "ctrl");
    const smeta = el("div", "meta"); smeta.appendChild(el("div", "name", "Steering stick"));
    srow.appendChild(smeta);
    const sin = el("div", "input"); const ssel = el("select");
    [["1", "Right stick (left/right)"], ["0", "Left stick (left/right)"]].forEach(([v, t]) => {
      const o = el("option"); o.value = v; o.textContent = t; if (String(c.steerSource) === v) o.selected = true; ssel.appendChild(o);
    });
    ssel.onchange = () => { c.steerSource = parseInt(ssel.value, 10); };
    sin.appendChild(ssel); srow.appendChild(sin); card.appendChild(srow);

    card.appendChild(toggle("Invert steering", "steerInvert", "Flip left/right if it steers the wrong way."));
    card.appendChild(toggle("Invert throttle", "throttleInvert", "Swap forward/reverse."));
    root.appendChild(card);

    // --- Output mapping matrix: CH2 / CH3 / CH4 / AUX -> any control ---
    const omcard = el("div", "card");
    omcard.appendChild(el("div", "sound-cat", "Output mapping"));
    omcard.appendChild(el("p", "pane-sub", "Assign each of this machine's implements to any control — a stick, a trigger, the bumpers/D-pad, or a button. Drive and steering are handled automatically. Implement names follow the machine you picked on the Machine tab (save it first if you just changed it)."));
    omcard.appendChild(el("p", "hint-row", "Pick a control for each implement — a stick, trigger, bumpers, or D-pad. Header shown in ( ) is the board output to wire it to. Tilt and angle are unassigned by default; assign them to bring their headers to life."));
    c.outputs = c.outputs || {};
    for (const [key, label] of (c.outputList || [])) {
      // Fixed full-range endpoints: center = stop, ends = full drive; the actuator's limit switch stops it.
      const o = c.outputs[key] || (c.outputs[key] = { src: 0, btn: "0x0000", min: 1000, center: 1500, max: 2000 });
      const block = el("div", "gpmap");
      block.appendChild(el("div", "gpmap-h", esc(label)));

      // control source (+ button picker if a button source)
      const srow = el("div", "ctrl");
      srow.appendChild((() => { const m = el("div", "meta"); m.appendChild(el("div", "name", "Control")); return m; })());
      const sin = el("div", "input"); const ssel = el("select");
      for (const [id, slabel] of (c.sourceChoices || [])) {
        const op = el("option"); op.value = id; op.textContent = slabel; if (Number(o.src) === Number(id)) op.selected = true; ssel.appendChild(op);
      }
      ssel.onchange = () => { o.src = parseInt(ssel.value, 10); buildGamepadUI(root); };
      sin.appendChild(ssel);
      srow.appendChild(sin); block.appendChild(srow);
      omcard.appendChild(block);
    }
    root.appendChild(omcard);
  }

  // (Servo endpoints removed — this rig drives motors + hydraulic actuators, not travel-limited servos.)

  // --- Save note (one Save button up top saves everything — no separate save here) ---
  root.appendChild(el("p", "pane-sub", "Use the Save button at the top to save these controls along with the rest of your settings, then Flash. " + (
    c.mode === "gamepad"
      ? "Game-controller builds use the Bluepad32 ESP32 core (downloaded once on the first controller flash)."
      : "Standard RC build — set-and-go, nothing to tune on the machine.")));
}

function render() {
  if (!state.activeTab) { const t0 = allTabs()[0]; state.activeTab = t0 ? (t0.id || t0.file) : FLASH; }
  if (state.activeTab !== FORGE) { demoAutoRunning = false; demoStop(); } // silence the demo when leaving Sound
  renderTabBar();
  const content = $("content"); content.innerHTML = "";
  if (state.activeTab === FLASH) { content.appendChild(renderFlashPane()); wireFlashPane(); }
  else if (state.activeTab === GAMEPAD) { content.appendChild(renderGamepadPane()); wireGamepadPane(); }
  else if (state.activeTab === FORGE) { content.appendChild(renderForgePane()); wireForgePane(); }
  else {
    const tab = allTabs().find((t) => (t.id || t.file) === state.activeTab);
    content.appendChild(tab ? renderSettingsPane(tab) : el("div", "empty", "Tab not found."));
  }
}

// ---------- save (one button saves EVERYTHING: schema tabs + Controls) ----------
async function save() {
  if (!isDirty()) { toast("Nothing to save."); return true; }
  $("saveBtn").disabled = true;
  try {
    // 1) Controls (gamepad) config, if the Controls tab was edited.
    if (gpTouched && gpCfg) { await post("/gamepad_config", gpCfg); gpTouched = false; }
    // 2) Machine / Levels / Sound changes.
    const payload = {};
    for (const [file, fields] of Object.entries(state.changes)) {
      if (!Object.keys(fields).length) continue;
      payload[file] = { ...fields };
      if (file.startsWith("vehicles/")) payload[file].__vehicle__ = state.schema.currentVehicle;
    }
    if (Object.keys(payload).length) await post("/save", payload);
    toast("✓ All settings saved.", "ok");
    await reloadKeepTab();
    return true;
  } catch (err) { toast("Save failed: " + err.message, "err"); return false; }
  finally { $("saveBtn").disabled = false; }
}

// ---------- vehicle change / import ----------
async function changeVehicle(vehicle) {
  if (isDirty() && !confirm("You have unsaved changes. Switch vehicle and discard them?")) {
    $("vehicleSel").value = state.schema.currentVehicle; return;
  }
  try {
    await post("/set_vehicle", { vehicle });
    await loadSchema();
    renderVehicleSelect();
    state.activeTab = state.schema.vehicleTab ? state.schema.vehicleTab.file : null;
    render();
    toast("Vehicle: " + vehicle.replace(/\.h$/, ""), "ok");
  } catch (err) { toast(err.message, "err"); }
}
async function importVehicle(file) {
  try {
    const content = await file.text();
    const j = await post("/import_vehicle", { filename: file.name, content });
    await loadSchema(); renderVehicleSelect();
    state.activeTab = state.schema.vehicleTab ? state.schema.vehicleTab.file : null;
    render();
    toast("Imported " + j.vehicle, "ok");
  } catch (e) { toast("Import failed: " + e.message, "err"); }
}

// ---------- flash pane wiring ----------
function wireFlashPane() {
  const logEl = $("log"), statusEl = $("status"), barEl = $("bar");
  const log = (t) => { logEl.textContent += t; logEl.scrollTop = logEl.scrollHeight; };
  const setStatus = (t, k = "") => { statusEl.textContent = t; statusEl.className = "status " + k; };
  const setProgress = (p) => { barEl.style.width = Math.max(0, Math.min(100, p)) + "%"; };
  const resetLog = () => { logEl.textContent = ""; };
  const busy = (on) => {
    // Lock everything that could interfere mid-flash — including the port
    // dropdown and the header Flash button — so nothing gets changed underneath.
    for (const id of ["doBuild", "saveBtn", "detectBtn", "nativeFlash", "nativePort", "flashBtnTop"]) {
      const b = $(id); if (b) b.disabled = on;
    }
  };
  const setIndeterminate = (on) => {
    if (on) { barEl.classList.add("indeterminate"); barEl.style.width = ""; }
    else { barEl.classList.remove("indeterminate"); }
  };
  // auto-open the collapsed details panel (so errors are never hidden)
  const showDetails = () => { const d = document.querySelector(".logwrap"); if (d) d.open = true; };

  async function doBuild() {
    if (isDirty() && !(await save())) return false;
    busy(true); resetLog(); setStatus("🔧 Compiling firmware…", "work"); setProgress(0); setIndeterminate(true); log("Compiling…\n");
    try {
      const ok = await streamBuild({ vehicle: "", onLog: log });
      setIndeterminate(false); setProgress(ok ? 100 : 0);
      setStatus(ok ? "✓ Build OK — ready to flash." : "Build failed — check the details below.", ok ? "ok" : "err");
      if (!ok) showDetails();
      return ok;
    } catch (err) { setIndeterminate(false); log("ERROR: " + err.message + "\n"); setStatus("Build failed: " + err.message, "err"); showDetails(); return false; }
    finally { busy(false); setIndeterminate(false); }
  }
  $("doBuild").onclick = doBuild;

  // --- Native flash via USB cable (arduino-cli uploader — the reliable path) ---
  $("detectBtn").onclick = async () => {
    const sel = $("nativePort");
    sel.innerHTML = "<option value=''>Detecting…</option>";
    try {
      const j = await (await fetch("/native_ports")).json();
      sel.innerHTML = "";
      const ports = (j.ports || []);
      if (!ports.length) { sel.innerHTML = "<option value=''>No serial ports found — check USB/driver</option>"; setStatus("No board detected. Check the USB cable/driver.", "err"); return; }
      for (const p of ports) {
        const o = el("option"); o.value = p.address;
        o.textContent = p.address + (p.likely ? "  ✅ (board)" : "");
        sel.appendChild(o);
      }
      setStatus("Found " + ports.length + " port(s). Pick your board, then Flash via cable.", "ok");
    } catch (e) { sel.innerHTML = "<option value=''>Detect failed</option>"; setStatus("Detect failed: " + e.message, "err"); }
  };
  $("nativeFlash").onclick = async () => {
    const port = $("nativePort").value;
    if (!port) { setStatus("Click Detect board and pick your board first.", "err"); return; }
    if (isDirty() && !(await save())) return;
    busy(true); resetLog();
    setStatus("🔧 Compiling firmware… first flash can take a few minutes", "work");
    setProgress(0); setIndeterminate(true);
    log("Flashing " + port + " via USB cable…\n");
    let phase = "compile";
    try {
      const res = await fetch("/run", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ cmd: "flash", port, vehicle: "" }) });
      if (!res.ok) { let m = "HTTP " + res.status; try { const e = await res.json(); if (e.error) m = e.error; } catch (_) {} throw new Error(m); }
      const reader = res.body.getReader(), dec = new TextDecoder(); let all = "";
      while (true) {
        const { value, done } = await reader.read(); if (done) break;
        const c = dec.decode(value, { stream: true }); all += c; log(c);
        // Compile → Upload transition (esptool starts talking to the board)
        if (phase === "compile" && /esptool|Connecting\.|Chip is|Writing at|Uploading stub/.test(c)) {
          phase = "upload"; setIndeterminate(false);
          setStatus("⬆ Uploading to your board — keep it plugged in…", "work");
        }
        // Upload % — esptool prints "(NN %)" with a space; the "(24%)" sketch-size
        // line has no space, so this won't be fooled by it.
        const pcts = c.match(/\((\d+)\s+%\)/g);
        if (pcts && phase === "upload") {
          const n = parseInt(pcts[pcts.length - 1].match(/\d+/)[0], 10);
          if (!isNaN(n)) setProgress(n);
        }
      }
      setIndeterminate(false);
      if (all.includes("--- DONE (exit 0) ---")) { setProgress(100); setStatus("✓ Flashed! Reconnect the battery. 🎉", "ok"); }
      else { setStatus("Flash failed — check the details below. (Battery disconnected? Right port? USB driver installed?)", "err"); showDetails(); }
    } catch (err) { log("ERROR: " + ((err && err.message) || err) + "\n"); setStatus("Flash failed: " + ((err && err.message) || err), "err"); showDetails(); }
    finally { busy(false); setIndeterminate(false); }
  };

}

// ---------- boot ----------
$("vehicleSel").onchange = (e) => changeVehicle(e.target.value);
$("saveBtn").onclick = save;
// The header "Flash" button is one-click: jump to the Flash tab, auto-detect the
// board, and start flashing it. (Previously it only switched tabs, so clicking the
// big obvious "Flash" button appeared to do nothing — "stuck", log never moved.)
$("flashBtnTop").onclick = async () => {
  if (isDirty()) { if (!(await save())) return; } // save everything (schema + controls) before flashing
  state.activeTab = FLASH; render();
  const sel = $("nativePort"), flashBtn = $("nativeFlash"), statusEl = $("status");
  if (!sel || !flashBtn || !statusEl) return;
  statusEl.textContent = "🔍 Looking for your board…"; statusEl.className = "status work";
  try {
    const j = await (await fetch("/native_ports")).json();
    const ports = j.ports || [];
    sel.innerHTML = "";
    for (const p of ports) {
      const o = el("option"); o.value = p.address;
      o.textContent = p.address + (p.likely ? "  ✅ (board)" : "");
      sel.appendChild(o);
    }
    const pick = ports.find((p) => p.likely) || (ports.length === 1 ? ports[0] : null);
    if (pick) { sel.value = pick.address; flashBtn.click(); }        // -> resets log + streams
    else if (ports.length) { statusEl.textContent = "Several ports found — pick your board below, then Flash."; statusEl.className = "status"; }
    else { statusEl.textContent = "No board found. Plug the ESP32 in with a USB data cable, then click Flash again."; statusEl.className = "status err"; }
  } catch (e) {
    statusEl.textContent = "Couldn't detect the board: " + ((e && e.message) || e); statusEl.className = "status err";
  }
};
$("quitBtn").onclick = async () => {
  if (isDirty() && !confirm("You have unsaved changes. Quit anyway?")) return;
  try { await fetch("/quit", { method: "POST" }); } catch (_) {}
  document.title = "Closed";
  document.body.innerHTML =
    "<div style='min-height:100vh;display:flex;align-items:center;justify-content:center;" +
    "flex-direction:column;gap:14px;text-align:center;font-family:system-ui,sans-serif;" +
    "background:#000;color:#ffcb05'>" +
    "<div style='font-size:42px'>⏻</div>" +
    "<div style='font-size:22px;font-weight:700'>Configurator closed.</div>" +
    "<div style='color:#9aa'>You can close this tab. Re-open the app anytime to start again.</div>" +
    "</div>";
};
$("importFile").onchange = (e) => { if (e.target.files[0]) importVehicle(e.target.files[0]); e.target.value = ""; };
$("wavFile").onchange = (e) => { if (e.target.files[0]) handleWavFile(e.target.files[0]); e.target.value = ""; };
window.addEventListener("beforeunload", (e) => { if (isDirty()) { e.preventDefault(); e.returnValue = ""; } });

// Heartbeat: lets the server auto-close itself when this tab goes away, so no
// stray Python process is left running. A refresh only pauses it for a moment.
const ping = () => fetch("/ping").catch(() => {});
ping();
setInterval(ping, 3000);
// ping the moment the tab regains focus, so a throttled background tab
// re-checks in immediately instead of waiting for the next interval
document.addEventListener("visibilitychange", () => { if (!document.hidden) ping(); });

(async function init() {
  try { await loadSchema(); renderVehicleSelect(); render(); }
  catch (err) { $("content").innerHTML = `<div class="empty">Failed to load: ${esc(err.message)}<br><br>Is the server running? Try refreshing.</div>`; }
})();
