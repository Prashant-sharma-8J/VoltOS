"use strict";

const byId = (id) => document.getElementById(id);
const units = (value, digits = 1) => Number(value).toFixed(digits);
const safe = (value) => String(value).replace(/[&<>"']/g, (character) => (
  { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[character]
));
const empty = (columns, message) => `<tr><td colspan="${columns}" class="empty-cell">${message}</td></tr>`;
const money = new Intl.NumberFormat("en-IN", { style: "currency", currency: "INR" });
const time = new Intl.DateTimeFormat("en-GB", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
const views = { overview: "Station overview", sessions: "Session history" };
const ui = { snapshot: null, history: [], timer: null, actionsInFlight: 0, statusRevision: 0 };

function route() {
  const hash = location.hash.slice(1);
  const view = Object.hasOwn(views, hash) ? hash : "overview";
  for (const name of Object.keys(views)) {
    byId(name).hidden = view !== name;
    const link = document.querySelector(`nav [href="#${name}"]`);
    if (view === name) link.setAttribute("aria-current", "page");
    else link.removeAttribute("aria-current");
  }
  byId("page-title").textContent = views[view];
  byId("overview-actions").hidden = view !== "overview";
  window.scrollTo(0, 0);
  drawChart();
}

async function request(path, data) {
  const options = data ? { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(data) } : {};
  const response = await fetch(path, { ...options, signal: AbortSignal.timeout(5000) });
  const payload = await response.json();
  if (!response.ok || !payload.ok) throw new Error(payload.error || "Station request failed.");
  return payload;
}

function alertUser(message, isError = false) {
  const notice = byId("notice");
  clearTimeout(ui.timer);
  notice.classList.toggle("error", isError);
  notice.textContent = message;
  notice.hidden = false;
  if (!isError) ui.timer = setTimeout(() => { notice.hidden = true; }, 4500);
}

function connection(status) {
  const element = byId("connection");
  if (element.dataset.status === status) return;
  element.dataset.status = status;
  element.className = `connection ${status}`;
  element.innerHTML = status === "live" ? "<i></i> Station live" : "<i></i> Disconnected";
}

function metric(id, value, unit) {
  byId(id).innerHTML = `${safe(value)}${unit ? `<small>${safe(unit)}</small>` : ""}`;
}

function renderDemoButton() {
  const demo = byId("demo");
  const data = ui.snapshot;
  const occupied = data && (data.queue.length > 0 || data.chargers.some((c) => c.vehicle));
  demo.disabled = !data || occupied || ui.actionsInFlight > 0;
  demo.title = occupied ? "Demo traffic can be loaded when the station is empty." : "Queue nine example vehicles.";
}

function render(data) {
  const active = data.chargers.filter((charger) => charger.state === "Charging").length;
  const offline = data.chargers.filter((charger) => charger.state === "Offline").length;
  metric("metric-power", units(data.power), "kW");
  byId("metric-cap").textContent = `of ${units(data.limit)} kW effective`;
  metric("metric-chargers", `${active} / ${data.chargers.length}`);
  byId("metric-health").textContent = offline ? `${offline} offline` : "All chargers online";
  metric("metric-queue", data.queue.length);
  byId("metric-next").textContent = data.queue.length ? `Next · ${data.queue[0].plate}` : "No requests waiting";
  metric("metric-completed", data.completed);
  byId("metric-energy").textContent = `${units(data.energy)} kWh billed`;
  byId("queue-count").textContent = data.queue.length;

  byId("power-mode").textContent = data.peak ? "Peak throttling active" : data.peakMode === "auto" ? "Peak mode scheduled" : "Peak throttling off";
  byId("sim-clock").textContent = `Simulation clock ×${data.speed}`;
  renderDemoButton();

  const rows = byId("charger-rows");
  const focused = rows.contains(document.activeElement) ? document.activeElement.dataset.charger : null;
  rows.innerHTML = data.chargers.map((charger) => {
    const id = String(charger.id).padStart(2, "0");
    const vehicle = charger.vehicle;
    const failed = charger.state === "Offline";
    const energy = vehicle
      ? `<strong class="mono">${safe(vehicle.plate)}</strong><span class="detail">${units(vehicle.delivered, 2)} / ${units(vehicle.target)} kWh</span><progress aria-label="Energy delivered to ${safe(vehicle.plate)}" value="${vehicle.delivered}" max="${vehicle.target}"></progress>`
      : `<span class="detail">${failed ? `Recovery in ${Math.ceil(charger.repairIn)}s` : "Ready for dispatch"}</span>`;
    return `<tr><td><strong class="mono">CH-${id}</strong><span class="detail state ${safe(charger.state)}"><i></i>${safe(charger.state)}</span></td>
      <td>${energy}</td><td class="mono">${units(charger.kw)} kW</td>
      <td><button type="button" aria-label="${failed ? "Restore" : "Simulate fault on"} charger ${charger.id}" class="fault-button ${failed ? "failed" : ""}" data-charger="${charger.id}" data-action="${failed ? "repair" : "fault"}" ${failed && vehicle ? "disabled" : ""}>${failed ? "Restore" : "Test fault"}</button></td></tr>`;
  }).join("");
  if (focused) rows.querySelector(`[data-charger="${focused}"]`)?.focus({ preventScroll: true });

  byId("queue-rows").innerHTML = data.queue.length ? data.queue.map((vehicle) => (
    `<tr><td><strong class="mono">${safe(vehicle.plate)}</strong><span class="detail">${vehicle.soc}% SOC${vehicle.interruptions ? " · requeued" : ""}</span></td>
     <td><span class="priority" data-rank="${vehicle.rank}">${safe(vehicle.priority)}</span></td>
     <td class="mono">${vehicle.eta === null ? "—" : vehicle.eta < 0.05 ? "&lt;0.1 min" : `${units(vehicle.eta)} min`}</td></tr>`
  )).join("") : empty(3, "No vehicles are waiting.");
  byId("events").innerHTML = data.events.slice(0, 9).map((event) => {
    const timestamp = new Date(event.time * 1000);
    return `<li data-type="${safe(event.type)}"><time datetime="${timestamp.toISOString()}">${time.format(timestamp)}</time><span>${safe(event.message)}</span></li>`;
  }).join("");

  metric("report-completed", data.completed);
  metric("report-energy", units(data.energy), "kWh");
  metric("report-revenue", money.format(data.revenue));
  metric("report-elapsed", data.completed ? units(data.elapsedAverage) : "—", "min");
  byId("receipt-rows").innerHTML = data.receipts.length ? data.receipts.map((row) => (
    `<tr><td class="mono">#${row.id}</td><td class="mono">${safe(row.plate)}</td><td class="mono">${units(row.energy, 2)} kWh</td>
    <td class="mono">${units(row.minutes, 2)} min</td><td>${row.interruptions}</td><td class="mono">${money.format(row.cost)}</td></tr>`
  )).join("") : empty(6, "Receipts appear when a charging session completes.");
  if (data.logError) alertUser(data.logError, true);
}

function drawChart() {
  const canvas = byId("power-chart");
  const { width, height } = canvas.getBoundingClientRect();
  if (!width || !height) return;
  const scale = window.devicePixelRatio || 1;
  canvas.width = Math.round(width * scale);
  canvas.height = Math.round(height * scale);
  const ctx = canvas.getContext("2d");
  ctx.scale(scale, scale);
  const compact = width < 390;
  const left = compact ? 49 : 58, right = width - 11, top = 18, bottom = height - 27;
  const now = ui.history.at(-1)?.time ?? 0;
  const elapsed = now - (ui.history[0]?.time ?? now);
  const windowSeconds = elapsed > 30 ? 60 : elapsed > 15 ? 30 : 15;
  const start = now - windowSeconds;
  const highest = Math.max(5, ...ui.history.map((point) => point.limit));
  const tick = Math.ceil(highest / 4 / (highest < 30 ? 1 : 5)) * (highest < 30 ? 1 : 5);
  const ceiling = tick * 4;
  const x = (t) => left + ((t - start) / windowSeconds) * (right - left);
  const y = (kw) => bottom - (kw / ceiling) * (bottom - top);
  ctx.font = `11px ${getComputedStyle(canvas).getPropertyValue("--mono")}`;
  ctx.fillStyle = "#adc0b1";
  ctx.strokeStyle = "#385349";
  ctx.lineWidth = 1;
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (let row = 0; row <= 4; row++) {
    const kw = row * tick;
    ctx.beginPath();
    ctx.moveTo(left, y(kw));
    ctx.lineTo(right, y(kw));
    ctx.stroke();
    ctx.fillText(row === 4 ? `${kw} kW` : kw, left - 6, y(kw));
  }
  const divisions = compact && windowSeconds > 15 ? 2 : windowSeconds === 15 ? 3 : 4;
  for (let index = 0; index <= divisions; index++) {
    const seconds = windowSeconds - index * (windowSeconds / divisions);
    const col = x(now - seconds);
    ctx.beginPath();
    ctx.moveTo(col, top);
    ctx.lineTo(col, bottom);
    ctx.stroke();
    ctx.textAlign = seconds === windowSeconds ? "left" : seconds === 0 ? "right" : "center";
    ctx.fillText(seconds ? `−${seconds}s` : "Now", col, bottom + 16);
  }
  if (!ui.history.length) return;
  ctx.save();
  ctx.beginPath();
  ctx.rect(left, top - 3, right - left + 1, bottom - top + 6);
  ctx.clip();
  for (const key of ["limit", "power"]) {
    ctx.beginPath();
    for (let index = 0; index < ui.history.length; index++) {
      const current = ui.history[index];
      if (!index) ctx.moveTo(x(current.time), y(current[key]));
      else {
        ctx.lineTo(x(current.time), y(ui.history[index - 1][key]));
        ctx.lineTo(x(current.time), y(current[key]));
      }
    }
    ctx.strokeStyle = key === "limit" ? "#a2b5a8" : "#dbef9d";
    ctx.lineWidth = key === "limit" ? 1.5 : 2.5;
    ctx.setLineDash(key === "limit" ? [5, 4] : []);
    ctx.stroke();
    const latest = ui.history.at(-1);
    ctx.fillStyle = ctx.strokeStyle;
    ctx.beginPath();
    ctx.arc(right - 2, y(latest[key]), 3.5, 0, Math.PI * 2);
    ctx.fill();
  }
  ctx.restore();
}

async function loadStatus() {
  const revision = ++ui.statusRevision;
  const data = await request("/api/status");
  if (revision !== ui.statusRevision) return;
  if (ui.snapshot && data.uptime < ui.snapshot.uptime) ui.history = [];
  ui.snapshot = data;
  ui.history.push({ time: data.uptime, limit: data.limit, power: data.power });
  ui.history = ui.history.filter((point) => point.time >= data.uptime - 61);
  connection("live");
  render(data);
  drawChart();
}

async function refresh() {
  try {
    await loadStatus();
  } catch (error) {
    connection("lost");
    if (!ui.snapshot) alertUser(error.message, true);
  } finally {
    setTimeout(refresh, 650);
  }
}

async function action(data, success, button, form) {
  const errorBox = form?.querySelector(".form-error");
  if (errorBox) errorBox.hidden = true;
  button.disabled = true;
  ui.actionsInFlight++;
  renderDemoButton();
  try {
    const result = await request("/api/action", data);
    if (form) form.closest("dialog").close();
    alertUser(typeof success === "function" ? success(result) : success);
    try {
      await loadStatus();
    } catch {
      connection("lost");
    }
    return true;
  } catch (error) {
    if (errorBox) {
      errorBox.textContent = error.message;
      errorBox.hidden = false;
    } else alertUser(error.message, true);
    return false;
  } finally {
    ui.actionsInFlight--;
    button.disabled = false;
    renderDemoButton();
  }
}

function openDialog(dialog) {
  dialog.querySelector(".form-error").hidden = true;
  dialog.showModal();
}
byId("open-arrival").addEventListener("click", () => openDialog(byId("arrival-dialog")));
byId("open-power").addEventListener("click", () => {
  if (!ui.snapshot) return alertUser("Station information is unavailable.", true);
  const form = byId("power-form");
  form.elements.capacity.value = ui.snapshot.capacity;
  form.elements.mode.value = ui.snapshot.peakMode;
  openDialog(byId("power-dialog"));
});
document.querySelectorAll("[data-close]").forEach((button) => {
  button.addEventListener("click", () => button.closest("dialog").close());
});
byId("demo").addEventListener("click", (event) => action({ action: "demo" }, "Nine demo requests queued.", event.currentTarget));
byId("charger-rows").addEventListener("click", (event) => {
  const button = event.target.closest("button[data-charger]");
  if (!button) return;
  const { charger, action: operation } = button.dataset;
  action({ action: operation, charger: Number(charger) },
    operation === "fault" ? `Fault simulated on charger ${charger}.` : `Charger ${charger} restored.`, button);
});
byId("arrival-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  const plate = form.elements.plate.value.trim().toUpperCase();
  const data = { action: "add", plate, soc: form.elements.soc.valueAsNumber,
    service: form.elements.service.value, kwh: form.elements.kwh.valueAsNumber };
  if (await action(data, (result) => `${plate} queued as request #${result.id}.`, event.submitter, form)) form.reset();
});
byId("power-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  action({ action: "settings", capacity: form.elements.capacity.valueAsNumber, mode: form.elements.mode.value },
    "Power settings updated.", event.submitter, form);
});
window.addEventListener("hashchange", route);
new ResizeObserver(drawChart).observe(byId("power-chart"));
route();
renderDemoButton();
refresh();
