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
const isDirty = () => Object.values(state.changes).some((f) => Object.keys(f).length);
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
  for (const t of allTabs()) add(t.file, esc(t.label));
  add(FORGE, "🔊 Sound Forge");
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

function renderForgePane() {
  const pane = el("div", "tabpane");
  pane.appendChild(el("h2", "pane-title", "🔊 Sound Forge"));
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
      row.append(play, use);
      list.appendChild(row);
    }
    if (items.length > 150) list.appendChild(el("div", "hint-row", "Showing first 150 — type to narrow it down."));
  };
  search.oninput = renderList;
  if (showAll) showAll.onchange = renderList;
  renderList();
}

async function useLibrarySound(file, group) {
  try {
    const j = await (await fetch("/sound_text/" + encodeURIComponent(file))).json();
    if (!j.ok) throw new Error(j.error || "Could not read sound");
    await post("/install_header", { filename: file, text: j.text, category: group.key });
    closeModal(); toast("Installed " + file.replace(/\.h$/, ""), "ok");
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
    const text = wavToHeader(audio, group.varPrefix);
    const base = file.name.replace(/\.[^.]+$/, "").replace(/[^A-Za-z0-9_-]/g, "_") || "customSound";
    await post("/install_header", { filename: base + ".h", text, category: group.key });
    closeModal(); toast("Added " + base, "ok");
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
    buildGamepadUI(root);
  }).catch((e) => { root.innerHTML = `<div class="empty">Couldn't load controls: ${esc(e.message)}</div>`; });
}

function buildGamepadUI(root) {
  const c = gpCfg;
  root.innerHTML = "";

  // --- Mode picker: two big, obvious cards ---
  const modeWrap = el("div", "gpmodes");
  const mk = (id, icon, title, sub) => {
    const card = el("div", "gpmode" + (c.mode === id ? " sel" : ""));
    card.innerHTML = `<div class="gpmode-ic">${icon}</div><div class="gpmode-t">${title}</div><div class="gpmode-s">${sub}</div>`;
    card.onclick = () => { c.mode = id; buildGamepadUI(root); };
    return card;
  };
  modeWrap.append(
    mk("webui", "🎚️", "RC transmitter", "Drive with your normal RC radio. Set the protocol on the Machine tab."),
    mk("gamepad", "🎮", "Game controller", "Drive with a PS4 / PS5 / Xbox pad over Bluetooth.")
  );
  root.appendChild(modeWrap);

  const gpOnly = c.mode === "gamepad";
  if (gpOnly) {
    root.appendChild(el("div", "gpbadge", "⚠️ Experimental — controller support needs testing on real hardware."));
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
    card.appendChild(toggle("Tank / dual-track mix", "tankmix",
      "For tracked vehicles (tanks, dozers). Blends throttle + steering into two track signals — LEFT track on CH1, RIGHT track on CH2. Plug an ESC into each; trim them with the CH1/CH2 endpoints below."));
    card.appendChild(toggle("Engine-feel rumble", "rumble",
      "Feel the engine through the controller — idle purr, revs rise with throttle, a bump on each gear shift. Turn off to save controller battery. (PS4/PS5/Xbox.)"));

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
    omcard.appendChild(el("p", "pane-sub", "Assign each spare output to any control — a stick, a trigger, or a button (hold or toggle). Steering (CH1) and throttle (ESC) keep their jobs. Enabling an output takes over that pin from its stock function (e.g. AUX/GPIO32 = 3rd brake light / coupler switch)."));
    omcard.appendChild(el("p", "hint-row", "🎛️ Endpoints shape each output's travel: min / center / max (µs). For a stick or trigger the value tracks the input across that range; for a button, min = released and max = pressed. Blade tilt and angle are unassigned by default — pick a control here to bring GPIO32 / GPIO14 to life."));
    c.outputs = c.outputs || {};
    for (const [key, label] of (c.outputList || [])) {
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
      if (o.src === 8 || o.src === 9) {
        const bsel = el("select"); bsel.style.marginLeft = "8px";
        for (const [mask, blabel] of (c.buttonChoices || [])) {
          const op = el("option"); op.value = mask; op.textContent = blabel; if (parseInt(mask, 16) === parseInt(o.btn, 16)) op.selected = true; bsel.appendChild(op);
        }
        bsel.onchange = () => { o.btn = bsel.value; };
        sin.appendChild(bsel);
      }
      srow.appendChild(sin); block.appendChild(srow);

      // endpoints (only once assigned)
      if (o.src !== 0) {
        const isBtn = (o.src === 8 || o.src === 9);
        const erow = el("div", "ctrl");
        erow.appendChild((() => { const m = el("div", "meta"); m.appendChild(el("div", "name", "Endpoints (µs)")); m.appendChild(el("div", "desc", isBtn ? "Min = released, Max = pressed." : "Min / center / max travel.")); return m; })());
        const ein = el("div", "input gpends");
        for (const [k, tag] of [["min", "Min"], ["center", "Center"], ["max", "Max"]]) {
          if (isBtn && k === "center") continue; // a button only uses min/max
          const box = el("div", "gpend"); box.appendChild(el("span", "gpend-tag", tag));
          const inp = el("input"); inp.type = "number"; inp.min = 500; inp.max = 2500; inp.step = 5;
          inp.value = o[k] != null ? o[k] : (k === "center" ? 1500 : k === "min" ? 1000 : 2000);
          inp.oninput = () => { o[k] = parseInt(inp.value, 10) || 1500; };
          box.appendChild(inp); ein.appendChild(box);
        }
        erow.appendChild(ein); block.appendChild(erow);
      }
      omcard.appendChild(block);
    }
    root.appendChild(omcard);
  }

  // --- Servo endpoints (always shown — apply in every mode) ---
  const scard = el("div", "card");
  scard.appendChild(el("div", "sound-cat", "Servo endpoints" + (c.servoProfile ? " · " + esc(c.servoProfile.replace(/^SERVOS_/, "")) : "")));
  scard.appendChild(el("p", "pane-sub", "Travel limits for each servo channel, in microseconds (1000–2000, 1500 = center)."));
  const ch = (label, keys) => {
    const row = el("div", "ctrl");
    const meta = el("div", "meta"); meta.appendChild(el("div", "name", esc(label)));
    row.appendChild(meta);
    const input = el("div", "input gpends");
    for (const [k, tag] of keys) {
      const box = el("div", "gpend");
      box.appendChild(el("span", "gpend-tag", tag));
      const inp = el("input"); inp.type = "number"; inp.min = 500; inp.max = 2500; inp.step = 5;
      inp.value = (c.servos && c.servos[k] != null) ? c.servos[k] : 1500;
      inp.oninput = () => { (c.servos ||= {})[k] = parseInt(inp.value, 10) || 1500; };
      box.appendChild(inp); input.appendChild(box);
    }
    row.appendChild(input); return row;
  };
  scard.appendChild(ch("CH1 · Steering", [["CH1L", "Left"], ["CH1C", "Center"], ["CH1R", "Right"]]));
  scard.appendChild(ch("CH2 · Gearbox", [["CH2L", "Gear 1"], ["CH2C", "Gear 2"], ["CH2R", "Gear 3"]]));
  scard.appendChild(ch("CH3 · Aux / Beacon", [["CH3L", "Low"], ["CH3C", "Mid"], ["CH3R", "High"]]));
  scard.appendChild(ch("CH4 · Coupler", [["CH4L", "Locked"], ["CH4R", "Unlocked"]]));
  root.appendChild(scard);

  // --- Save ---
  const bar = el("div", "toolbar"); bar.style.marginTop = "18px";
  const saveBtn = el("button", "primary", "💾 Save controls");
  saveBtn.onclick = async () => {
    saveBtn.disabled = true;
    try {
      const j = await post("/gamepad_config", c);
      gpCfg = j.config || c;
      toast(c.mode === "gamepad" ? "Saved. Flash to build the controller firmware." : "Saved. Flash to apply.", "ok");
      buildGamepadUI(root);
    } catch (e) { toast("Save failed: " + e.message, "err"); }
    finally { saveBtn.disabled = false; }
  };
  const flashBtn = el("button", null, "⚡ Save & Flash");
  flashBtn.onclick = async () => {
    flashBtn.disabled = true;
    try { await post("/gamepad_config", c); } catch (e) { toast("Save failed: " + e.message, "err"); flashBtn.disabled = false; return; }
    flashBtn.disabled = false;
    $("flashBtnTop").click();
  };
  bar.append(saveBtn, flashBtn);
  root.appendChild(bar);
  root.appendChild(el("p", "pane-sub",
    c.mode === "gamepad"
      ? "Game-controller builds use the Bluepad32 ESP32 core (downloaded once on the first controller flash)."
      : "Standard RC build — set-and-go, nothing to tune on the machine."));
}

function render() {
  if (!state.activeTab) state.activeTab = (allTabs()[0] || {}).file || FLASH;
  renderTabBar();
  const content = $("content"); content.innerHTML = "";
  if (state.activeTab === FLASH) { content.appendChild(renderFlashPane()); wireFlashPane(); }
  else if (state.activeTab === GAMEPAD) { content.appendChild(renderGamepadPane()); wireGamepadPane(); }
  else if (state.activeTab === FORGE) { content.appendChild(renderForgePane()); wireForgePane(); }
  else {
    const tab = allTabs().find((t) => t.file === state.activeTab);
    content.appendChild(tab ? renderSettingsPane(tab) : el("div", "empty", "Tab not found."));
  }
}

// ---------- save ----------
async function save() {
  if (!isDirty()) { toast("Nothing to save."); return true; }
  const payload = {};
  for (const [file, fields] of Object.entries(state.changes)) {
    if (!Object.keys(fields).length) continue;
    payload[file] = { ...fields };
    if (file.startsWith("vehicles/")) payload[file].__vehicle__ = state.schema.currentVehicle;
  }
  $("saveBtn").disabled = true;
  try {
    await post("/save", payload);
    toast("✓ Settings saved.", "ok");
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
