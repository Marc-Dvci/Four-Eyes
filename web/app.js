// The car page: draws the car from the vehicle signals and lists every verdict as the gate records it.
"use strict";

const DOORS = [
  { name: "front left", x: 44, y: 205, side: -1 },
  { name: "front right", x: 256, y: 205, side: 1 },
  { name: "rear left", x: 44, y: 290, side: -1 },
  { name: "rear right", x: 256, y: 290, side: 1 },
];
const SHORT = {
  "Vehicle.Cabin.Door.Row1.DriverSide": "front left door",
  "Vehicle.Cabin.Door.Row1.PassengerSide": "front right door",
  "Vehicle.Cabin.Door.Row2.DriverSide": "rear left door",
  "Vehicle.Cabin.Door.Row2.PassengerSide": "rear right door",
};
const GEARS = { 126: "P", "-1": "R", 0: "N", 127: "D" };
const svgNS = "http://www.w3.org/2000/svg";
const $ = (id) => document.getElementById(id);

function el(tag, attrs, parent) {
  const e = document.createElementNS(svgNS, tag);
  for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
  parent.appendChild(e);
  return e;
}

const doorEls = DOORS.map((d, i) => {
  const g = el("g", {}, $("doors"));
  const x0 = d.side < 0 ? d.x : d.x - 14;
  const door = el("rect", { class: "door", x: x0, y: d.y, width: 14, height: 76, rx: 4 }, g);
  const trackX = d.side < 0 ? d.x + 20 : d.x - 30;
  el("rect", { class: "win-track", x: trackX, y: d.y + 4, width: 10, height: 68, rx: 3 }, g);
  const win = el("rect", { class: "win", x: trackX, y: d.y + 4, width: 10, height: 68, rx: 3 }, g);
  const lock = el("text", { class: "lock", x: d.side < 0 ? d.x - 4 : d.x + 4, y: d.y + 44,
    "text-anchor": d.side < 0 ? "end" : "start" }, g);
  lock.textContent = "locked";
  return { door, win, lock, top: d.y + 4, i };
});

function render(s) {
  $("speed").textContent = Number(s.speed_kmh).toFixed(0);
  $("gear").textContent = GEARS[s.gear] ?? s.gear;
  $("brake").textContent = s.parking_brake ? "on" : "off";
  $("hvac").textContent = s.hvac;
  doorEls.forEach((d) => {
    const locked = (s.locks >> d.i) & 1;
    d.door.classList.toggle("unlocked", !locked);
    d.lock.classList.toggle("unlocked", !locked);
    d.lock.textContent = locked ? "locked" : "unlocked";
    const open = s.windows[d.i] || 0; // percent open: glass shrinks from the top
    const h = 68 * (1 - open / 100);
    d.win.setAttribute("height", h.toFixed(1));
    d.win.setAttribute("y", (d.top + 68 - h).toFixed(1));
  });
  $("trunk").classList.toggle("open", !!s.trunk_open);
}

function describe(signal, value) {
  for (const [p, name] of Object.entries(SHORT)) {
    if (signal === p + ".IsLocked") return (value ? "lock " : "unlock ") + name;
    if (signal === p + ".Window.Position") return `${name.replace(" door", "")} window to ${value} %`;
  }
  if (signal === "Vehicle.Body.Trunk.Rear.IsOpen") return value ? "open the trunk" : "close the trunk";
  if (signal.endsWith("Driver.Temperature")) return `cabin to ${value} °C`;
  if (signal === "unknown") return "change an unlisted signal";
  return `set ${signal} to ${value}`;
}

const REASON = {
  unlock_moving: "the car is not parked",
  trunk_moving: "the car is not in Park",
  window_speed: "above 100 km/h a window opens 30 % at most",
  from_message: "the request carries outside text, so it may not move anything",
  out_of_range: "value out of range",
  not_allowed: "not a signal the agent may change",
  unknown_signal: "this car has no such signal",
  rate: "the agent is over its rate limit",
  turn_budget: "too many actions for one request",
  state_stale: "vehicle state unknown",
  no_turn: "no open request from the driver",
};

function flash(signal, cls) {
  let target = signal === "bus" ? $("carbody") : $("trunk");
  Object.keys(SHORT).forEach((p, i) => { if (signal.startsWith(p)) target = doorEls[i].door; });
  target.classList.remove(cls);
  void target.getBBox();
  target.classList.add(cls);
}

function add(cls, who, text, meta) {
  const li = document.createElement("li");
  li.className = cls;
  const w = document.createElement("span");
  w.className = "who";
  w.textContent = who;
  li.append(w, document.createTextNode(" " + text));
  if (meta) {
    const m = document.createElement("div");
    m.className = "meta";
    m.textContent = meta;
    li.append(m);
  }
  $("feed").prepend(li);
}

function visible(text) {
  // shows hidden characters instead of hiding them
  return [...text].map((ch) => {
    const c = ch.codePointAt(0);
    if ((c >= 0xe0000 && c <= 0xe007f)) return String.fromCodePoint(c - 0xe0000);
    if ((c >= 0x200b && c <= 0x200f) || (c >= 0x202a && c <= 0x202e) || (c >= 0x2060 && c <= 0x2069) || c === 0xfeff)
      return "·";
    return ch;
  }).join("");
}

function onRecord(e) {
  const meta = `record #${e.seq} · ${e.hash.slice(0, 16)}…`;
  if (e.kind === "kernel") {
    const refused = e.verdict === "refused";
    add(refused ? "kernel" : "allowed", refused ? "Refused by the kernel:" : "NOT refused by the kernel:",
      `the agent tried to ${e.op === "open" ? "open" : e.op === "write_unlock" ? "write an unlock onto" : "take over"} the CAN bus ${e.path} (${e.error}).`, meta);
    if (refused) flash("bus", "flash-kernel");
  } else if (e.kind === "turn") {
    const inv = e.invisible ? ` It hides ${e.invisible} invisible characters: “${visible(e.text)}”` : "";
    add("req", e.origin === "driver" ? "Driver:" : "Incoming message:", `“${e.text.replace(/[\u{E0000}-\u{E007F}\u200b-\u200f\u202a-\u202e\u2060-\u2069\ufeff]/gu, "")}”${inv}`, meta);
  } else if (e.kind === "proposal") {
    const what = describe(e.signal, e.value);
    if (e.verdict === "allowed") add("allowed", "Allowed:", `the agent proposed to ${what}.`, meta);
    else {
      add("gate", "Refused by the gate:", `the agent proposed to ${what} at ${Number(e.speed_kmh).toFixed(0)} km/h, but ${REASON[e.reason] || e.reason}.`, meta);
      flash(e.signal, "flash-gate");
    }
  } else if (e.kind === "done" && e.actions === 0) {
    add("req", "Agent:", e.reply ? `“${e.reply}” (proposed no action)` : "proposed no action.", meta);
  }
}

async function main() {
  const h = await (await fetch("api/health")).json();
  $("mode").textContent = h.mode === "live"
    ? "Live: the agent and the gate are running in QNX 8.0 (QEMU)."
    : "Demo: the gate's C core decides on this host; the model's answers and the kernel refusals are replayed from a QNX run.";
  render(await (await fetch("api/state")).json());
  (await (await fetch("api/record")).json()).forEach(onRecord);
  const cases = await (await fetch("api/cases")).json();
  const pick = ["h01", "s01", "u01", "i01", "s03", "h05", "s02", "r01", "h04", "n01", "u04", "h02"];
  pick.map((id) => cases.find((c) => c.id === id)).filter(Boolean).forEach((c) => {
    const b = document.createElement("button");
    b.type = "button";
    b.dataset.kind = c.kind;
    const label = c.text.replace(/[\u{E0000}-\u{E007F}\u200b-\u200f\u202a-\u202e\u2060-\u2069\ufeff]/gu, "");
    b.textContent = `${label.length > 46 ? label.slice(0, 44) + "…" : label} · ${c.vehicle.speed_kmh} km/h`;
    b.title = `${c.kind}, from the ${c.origin}`;
    b.addEventListener("click", () => fetch("api/request", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ case: c.id }) }));
    $("presets").append(b);
  });
  const ev = new EventSource("api/events");
  ev.addEventListener("state", (m) => render(JSON.parse(m.data)));
  ev.addEventListener("record", (m) => onRecord(JSON.parse(m.data)));
}

main();
