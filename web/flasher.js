// Build streamer. Flashing is done natively by the server (arduino-cli --upload),
// so no browser-side serial code lives here anymore.

const noop = () => {};

// Run a server-side compile. Streams the compiler log to onLog(text).
// Returns true on a clean compile (exit 0).
export async function streamBuild({ vehicle = "", onLog = noop } = {}) {
  const res = await fetch("/run", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ cmd: "build", vehicle }),
  });
  if (!res.ok) {
    let msg = "Build request failed (HTTP " + res.status + ")";
    try { const j = await res.json(); if (j.error) msg = j.error; } catch (_) {}
    throw new Error(msg);
  }
  const reader = res.body.getReader();
  const dec = new TextDecoder();
  let all = "";
  while (true) {
    const { value, done } = await reader.read();
    if (done) break;
    const chunk = dec.decode(value, { stream: true });
    all += chunk;
    onLog(chunk);
  }
  return all.includes("--- DONE (exit 0) ---");
}
