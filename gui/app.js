const state = {
  token: "",
  status: null,
  devices: null,
  samples: [],
  lastEvent: "",
  requestInFlight: false,
  quitting: false,
  startupStartedAt: 0,
  startupCompletedAt: 0,
  startupFailedAt: 0,
  startupProgressValue: 5
};

const bytecoinPoolHost = "pool.bytecoin.network";
const bytecoinStatsApiUrl = "https://bytecoin.network/api/stats";
let poolStatsApiUrlAutoFilled = false;

const byId = (id) => document.getElementById(id);
const chart = byId("hashrate-chart");
const context = chart.getContext("2d");
const formError = byId("form-error");
const startButton = byId("start-button");
const stopButton = byId("stop-button");
const pauseButton = byId("pause-button");
const quitButton = byId("quit-button");

function isBytecoinPool(endpoint) {
  const value = String(endpoint || "").trim();
  if (!value) return false;
  try {
    const url = new URL(value.includes("://") ? value : `tcp://${value}`);
    return url.hostname.toLowerCase() === bytecoinPoolHost;
  } catch {
    return false;
  }
}

function syncPoolStatsApiUrl() {
  const input = byId("pool-api-url");
  if (isBytecoinPool(byId("pool").value)) {
    if (!input.value.trim() || poolStatsApiUrlAutoFilled) {
      input.value = bytecoinStatsApiUrl;
      poolStatsApiUrlAutoFilled = true;
    }
    return;
  }

  if (poolStatsApiUrlAutoFilled) {
    if (input.value.trim() === bytecoinStatsApiUrl) {
      input.value = "";
    }
    poolStatsApiUrlAutoFilled = false;
  }
}

function formatRate(rate) {
  if (!Number.isFinite(rate) || rate <= 0) return "0.0 H/s";
  if (rate >= 1e6) return `${(rate / 1e6).toFixed(2)} MH/s`;
  if (rate >= 1e3) return `${(rate / 1e3).toFixed(2)} kH/s`;
  return `${rate.toFixed(1)} H/s`;
}

function formatDuration(seconds) {
  const value = Math.max(0, Math.floor(seconds || 0));
  const hours = String(Math.floor(value / 3600)).padStart(2, "0");
  const minutes = String(Math.floor((value % 3600) / 60)).padStart(2, "0");
  const rest = String(value % 60).padStart(2, "0");
  return `${hours}:${minutes}:${rest}`;
}

function formatInteger(value) {
  return Number(value || 0).toLocaleString();
}

function asRecord(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? value
    : {};
}

function toFiniteNumber(value) {
  if (value === null || value === undefined || value === "") return null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function formatCoinAmount(value, units, symbol) {
  const amount = toFiniteNumber(value);
  const denomination = toFiniteNumber(units);
  if (amount === null || denomination === null || denomination <= 0) return "—";
  return `${(amount / denomination).toFixed(8)} ${symbol || "BCN"}`;
}

function formatPoolDate(value) {
  const parsed = toFiniteNumber(value);
  if (parsed === null || parsed <= 0) return "—";
  const milliseconds = parsed < 1e12 ? parsed * 1000 : parsed;
  const date = new Date(milliseconds);
  return Number.isNaN(date.getTime()) ? "—" : date.toLocaleString();
}

function setPoolApiError(message) {
  const error = byId("pool-api-error");
  error.textContent = message;
  error.hidden = !message;
}

function setPoolApiIndicator(label, kind = "") {
  const indicator = byId("pool-api-indicator");
  indicator.textContent = label;
  indicator.className = `pool-api-indicator${kind ? ` ${kind}` : ""}`;
}

function updatePoolUserCard() {
  const wallet = byId("wallet").value.trim();
  const status = state.status || {};
  byId("pool-user-wallet").textContent =
    wallet || "Add a wallet in Mining settings";
  byId("pool-user-wallet").title = wallet;
  byId("pool-user-hashrate").textContent = formatRate(status.hashrate || 0);
  byId("pool-user-uptime").textContent =
    formatDuration(status.uptimeSeconds || 0);
  byId("pool-user-accepted").textContent =
    formatInteger(status.accepted || 0);
  byId("pool-user-share-state").textContent =
    `${formatInteger(status.rejected || 0)} / ${formatInteger(status.pending || 0)}`;
}

function renderPoolPorts(ports) {
  const tableBody = byId("pool-ports");
  tableBody.replaceChildren();
  if (!Array.isArray(ports) || ports.length === 0) {
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 3;
    cell.textContent = "This API did not provide pool port details.";
    row.append(cell);
    tableBody.append(row);
    return;
  }

  for (const port of ports.slice(0, 20)) {
    const data = asRecord(port);
    const row = document.createElement("tr");
    for (const value of [
      data.port ?? "—",
      data.difficulty === undefined ? "—" : formatInteger(data.difficulty),
      data.desc ?? data.description ?? "—"
    ]) {
      const cell = document.createElement("td");
      cell.textContent = String(value);
      row.append(cell);
    }
    tableBody.append(row);
  }
}

function renderPoolStats(data, url) {
  const response = asRecord(data);
  const config = asRecord(response.config);
  const pool = asRecord(response.pool);
  const poolStats = asRecord(pool.stats);
  const network = asRecord(response.network);
  const recognized = [
    pool.hashrate,
    pool.miners,
    pool.totalBlocks,
    network.height,
    network.difficulty
  ].some((value) => value !== undefined && value !== null);
  if (!recognized) {
    throw new Error("The URL returned JSON, but it does not look like a Bytecoin pool stats response.");
  }

  const coin = String(config.symbol || "BCN");
  const units = config.coinUnits || config.denominationUnit || 1;
  const fee = toFiniteNumber(config.fee);
  const blockTime = toFiniteNumber(config.blockTime);
  const lastBlock = pool.lastBlockFound ?? poolStats.lastBlockFound;

  byId("pool-api-source").textContent = `${url.origin}${url.pathname}`;
  byId("pool-api-updated").textContent =
    `Updated ${new Date().toLocaleTimeString()}`;
  byId("pool-hashrate").textContent = formatRate(
    toFiniteNumber(pool.hashrate) ?? 0
  );
  byId("pool-miners").textContent =
    pool.miners === undefined ? "—" : formatInteger(pool.miners);
  byId("pool-round-hashes").textContent =
    pool.roundHashes === undefined ? "—" : formatInteger(pool.roundHashes);
  byId("pool-blocks").textContent =
    pool.totalBlocks === undefined ? "—" : formatInteger(pool.totalBlocks);
  byId("pool-fee").textContent = fee === null ? "—" : `${fee}%`;
  byId("pool-minimum-payout").textContent = formatCoinAmount(
    config.minPaymentThreshold,
    units,
    coin
  );
  byId("network-height").textContent =
    network.height === undefined ? "—" : formatInteger(network.height);
  byId("network-difficulty").textContent =
    network.difficulty === undefined ? "—" : formatInteger(network.difficulty);
  byId("network-reward").textContent = formatCoinAmount(
    network.reward,
    units,
    coin
  );
  byId("network-block-time").textContent =
    blockTime === null ? "—" : blockTime >= 60
      ? `${formatInteger(blockTime / 60)} min`
      : `${formatInteger(blockTime)} sec`;
  byId("pool-payments").textContent =
    pool.totalPayments === undefined ? "—" : formatInteger(pool.totalPayments);
  byId("pool-miners-paid").textContent =
    pool.totalMinersPaid === undefined
      ? "—"
      : formatInteger(pool.totalMinersPaid);
  const lastBlockText = formatPoolDate(lastBlock);
  byId("pool-last-block").textContent =
    `Last pool block: ${lastBlockText}`;
  renderPoolPorts(config.ports);
  updatePoolUserCard();
}

function resetPoolStats(url) {
  byId("pool-api-source").textContent = `${url.origin}${url.pathname}`;
  byId("pool-api-updated").textContent = "Fetching latest data…";
  for (const id of [
    "pool-hashrate",
    "pool-miners",
    "pool-round-hashes",
    "pool-blocks",
    "pool-fee",
    "pool-minimum-payout",
    "network-height",
    "network-difficulty",
    "network-reward",
    "network-block-time",
    "pool-payments",
    "pool-miners-paid"
  ]) {
    byId(id).textContent = "—";
  }
  byId("pool-last-block").textContent = "Last pool block: —";
  byId("pool-ports").replaceChildren();
  updatePoolUserCard();
}

async function loadPoolApi(event) {
  event.preventDefault();
  setPoolApiError("");
  const input = byId("pool-api-url");
  let url;
  try {
    url = new URL(input.value.trim());
  } catch {
    setPoolApiError("Enter a complete API URL, including https:// or http://.");
    return;
  }
  if (!["https:", "http:"].includes(url.protocol) ||
      url.username || url.password) {
    setPoolApiError("Use an HTTP(S) URL without embedded username or password.");
    return;
  }

  byId("pool-api-results").hidden = false;
  resetPoolStats(url);
  setPoolApiIndicator("Loading pool data…", "loading");
  const submit = byId("pool-api-submit");
  submit.disabled = true;
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), 10000);
  try {
    const response = await fetch(url.href, {
      method: "GET",
      headers: { Accept: "application/json" },
      mode: "cors",
      credentials: "omit",
      cache: "no-store",
      redirect: "error",
      signal: controller.signal
    });
    if (!response.ok) {
      throw new Error(`The API returned HTTP ${response.status}.`);
    }
    const contentLength = toFiniteNumber(
      response.headers.get("content-length")
    );
    if (contentLength !== null && contentLength > 1_000_000) {
      throw new Error("The API response is larger than the 1 MB limit.");
    }
    const body = await response.text();
    if (body.length > 1_000_000) {
      throw new Error("The API response is larger than the 1 MB limit.");
    }
    let data;
    try {
      data = JSON.parse(body);
    } catch {
      throw new Error("The API did not return valid JSON.");
    }
    renderPoolStats(data, url);
    setPoolApiIndicator("Pool API connected", "connected");
  } catch (error) {
    const message = error.name === "AbortError"
      ? "The pool API request timed out after 10 seconds."
      : error instanceof TypeError
        ? "The browser could not read this API. Check the URL and confirm that it allows cross-origin (CORS) requests."
        : error.message;
    setPoolApiIndicator("Pool API unavailable", "error");
    setPoolApiError(message);
  } finally {
    window.clearTimeout(timeout);
    submit.disabled = false;
  }
}

function saveSettings() {
  const saved = {
    pool: byId("pool").value.trim(),
    wallet: byId("wallet").value.trim(),
    backend: byId("backend").value,
    threads: byId("threads").value,
    nonceOffset: Number(byId("nonce-offset").value || 39),
    tls: byId("tls").checked
  };
  try {
    localStorage.setItem("bcnminer.settings", JSON.stringify(saved));
  } catch {
    setError("Browser storage is unavailable. Settings will not persist after closing this page.");
  }
}

function loadSettings(serverConfig) {
  try {
    const saved = JSON.parse(localStorage.getItem("bcnminer.settings") || "null");
    const config = serverConfig && typeof serverConfig === "object" ? serverConfig : {};
    const values = { ...config, ...(saved || {}) };
    if (typeof values.pool === "string") byId("pool").value = values.pool;
    if (typeof values.wallet === "string") byId("wallet").value = values.wallet;
    if (typeof config.password === "string") byId("password").value = config.password;
    if (["cpu", "gpu", "both"].includes(values.backend)) byId("backend").value = values.backend;
    if (typeof values.threads === "string") byId("threads").value = values.threads;
    if (Number.isInteger(values.nonceOffset)) byId("nonce-offset").value = String(values.nonceOffset);
    if (typeof values.tls === "boolean") byId("tls").checked = values.tls;
  } catch {
    setError("Saved browser settings could not be read; please review your configuration.");
  }
}

function setError(message) {
  formError.textContent = message;
  formError.hidden = !message;
}

async function requestJson(path, options = {}) {
  const headers = { Accept: "application/json", ...(options.headers || {}) };
  if (options.method && options.method !== "GET") {
    headers["Content-Type"] = "application/json";
    headers["X-BCN-Token"] = state.token;
  }
  const response = await fetch(path, { ...options, headers, cache: "no-store" });
  let payload;
  try {
    payload = await response.json();
  } catch {
    throw new Error(`The miner returned an unreadable response (${response.status}).`);
  }
  if (!response.ok) throw new Error(payload.error || `Request failed (${response.status}).`);
  return payload;
}

function addActivity(message, kind = "normal") {
  if (!message || message === state.lastEvent) return;
  state.lastEvent = message;
  const list = byId("activity-list");
  const empty = list.querySelector(".activity-empty");
  if (empty) empty.remove();
  const row = document.createElement("div");
  row.className = "activity-entry";
  const time = document.createElement("time");
  time.dateTime = new Date().toISOString();
  time.textContent = new Date().toLocaleTimeString([], { hour12: false });
  const dot = document.createElement("span");
  dot.className = `event-dot ${kind}`;
  const text = document.createElement("p");
  text.textContent = message;
  row.append(time, dot, text);
  list.prepend(row);
  while (list.children.length > 40) list.lastElementChild.remove();
}

function setConnection(status) {
  const pill = byId("connection-pill");
  const label = byId("connection-label");
  const connection = String(status.connectionStatus || "IDLE").toUpperCase();
  const running = Boolean(status.running);
  pill.className = "connection-pill";
  if (connection === "CONNECTED") {
    pill.classList.add(status.paused ? "warning" : "mining");
    label.textContent = status.paused ? "Mining paused" : "Pool connected";
  } else if (connection === "CONNECTING" || connection === "RECONNECTING" || connection === "TUNING") {
    pill.classList.add("warning");
    label.textContent = connection === "TUNING" ? "Optimizing hardware" : connection === "RECONNECTING" ? "Reconnecting" : "Connecting";
  } else if (connection === "ERROR") {
    pill.classList.add("error");
    label.textContent = "Miner error";
  } else {
    label.textContent = running ? connection : "Miner idle";
  }
}

function updateDeviceCards(status) {
  const grid = byId("device-grid");
  grid.replaceChildren();
  const running = Boolean(status.running);
  const logicalProcessors = status.maxThreads || state.devices?.logicalProcessors || 0;
  const physicalCores = status.physicalCores || state.devices?.physicalCores || 0;
  const cpuEnabled = running && Number(status.threads) > 0;
  const cpuPaused = Boolean(status.cpuPaused);
  const pauseAll = Boolean(status.pauseAll);
  const cpuLabel = !cpuEnabled
    ? running ? "NOT SELECTED" : "READY"
    : cpuPaused
      ? "PAUSED"
      : status.mining
        ? "MINING"
        : "WAITING";
  const cpuKind = cpuPaused ? "paused" : status.mining && cpuEnabled ? "active" : "";
  const cpu = document.createElement("article");
  cpu.className = "device-card";
  const cpuSymbol = document.createElement("div");
  cpuSymbol.className = "device-symbol cpu-symbol";
  cpuSymbol.textContent = "CPU";
  const cpuInfo = document.createElement("div");
  cpuInfo.className = "device-info";
  const cpuName = document.createElement("strong");
  cpuName.textContent = `${physicalCores} cores · ${logicalProcessors} logical`;
  const cpuDetail = document.createElement("span");
  cpuDetail.textContent = cpuEnabled
    ? `${status.threads || 0} workers configured`
    : "CPU workers are not selected";
  cpuInfo.append(cpuName, cpuDetail);
  const cpuRate = document.createElement("span");
  cpuRate.className = "device-hashrate";
  cpuRate.textContent = formatRate(status.cpuHashrate);
  const cpuControls = document.createElement("div");
  cpuControls.className = "device-controls";
  cpuControls.append(
    createDeviceStatus(cpuLabel, cpuKind),
    createDevicePauseButton("cpu", 0, cpuPaused, cpuEnabled, pauseAll)
  );
  cpu.append(cpuSymbol, cpuInfo, cpuRate, cpuControls);
  grid.append(cpu);

  const gpuDevices = Array.isArray(status.gpus) && status.gpus.length
    ? status.gpus
    : (state.devices?.gpus || []).map((device) => ({
      ...device,
      enabled: false,
      paused: false,
      hashrate: 0
    }));
  const hasSessionGpuStatus = Array.isArray(status.gpus) && status.gpus.length > 0;
  for (const [index, gpu] of gpuDevices.entries()) {
    const card = document.createElement("article");
    card.className = "device-card";
    const symbol = document.createElement("div");
    symbol.className = "device-symbol gpu-symbol";
    symbol.textContent = "GPU";
    const info = document.createElement("div");
    info.className = "device-info";
    const name = document.createElement("strong");
    name.textContent = gpu.name || "OpenCL GPU";
    const detail = document.createElement("span");
    detail.textContent = [gpu.vendor, gpu.processor || gpu.detail].filter(Boolean).join(" · ") || "OpenCL device";
    info.append(name, detail);
    const rate = document.createElement("span");
    rate.className = "device-hashrate";
    rate.textContent = formatRate(gpu.hashrate);
    const paused = Boolean(gpu.paused || status.gpuPaused);
    const enabled = Boolean(gpu.enabled);
    const gpuLabel = paused
      ? "PAUSED"
      : !running
        ? enabled ? "READY" : "DETECTED"
        : enabled
          ? status.mining ? "MINING" : "WAITING"
          : hasSessionGpuStatus && status.gpuBackendEnabled ? "UNAVAILABLE" : "DETECTED";
    const gpuKind = paused
      ? "paused"
      : enabled && status.mining
        ? "active"
        : hasSessionGpuStatus && status.gpuBackendEnabled && running && !enabled
          ? "unavailable"
          : "";
    const controls = document.createElement("div");
    controls.className = "device-controls";
    controls.append(
      createDeviceStatus(gpuLabel, gpuKind),
      createDevicePauseButton("gpu", index, paused, running && enabled, pauseAll)
    );
    card.append(symbol, info, rate, controls);
    grid.append(card);
  }
  const total = 1 + gpuDevices.length;
  byId("device-count").textContent = `${total} device${total === 1 ? "" : "s"} detected`;
}

function createDeviceStatus(label, kind) {
  const badge = document.createElement("span");
  badge.className = `device-state${kind ? ` ${kind}` : ""}`;
  const indicator = document.createElement("i");
  const text = document.createElement("span");
  text.textContent = label;
  badge.append(indicator, text);
  return badge;
}

function createDevicePauseButton(type, index, paused, enabled, pauseAll = false) {
  const button = document.createElement("button");
  button.type = "button";
  button.className = `device-pause${paused ? " resume" : ""}`;
  button.disabled = !enabled || pauseAll;
  button.setAttribute(
    "aria-label",
    `${paused ? "Resume" : "Pause"} ${type === "cpu" ? "CPU workers" : `GPU ${index}`}`
  );
  button.title = pauseAll
    ? "Resume all workers to enable individual device controls."
    : button.getAttribute("aria-label");
  const icon = document.createElement("span");
  icon.setAttribute("aria-hidden", "true");
  icon.textContent = paused ? "▶" : "Ⅱ";
  const label = document.createElement("span");
  label.textContent = paused ? "Resume" : "Pause";
  button.append(icon, label);
  button.addEventListener("click", async () => {
    button.disabled = true;
    const action = type === "cpu"
      ? paused ? "resume-cpu" : "pause-cpu"
      : paused ? "resume-gpu" : "pause-gpu";
    const body = type === "cpu" ? { action } : { action, index };
    try {
      await requestJson("/api/control", {
        method: "POST",
        body: JSON.stringify(body)
      });
      addActivity(
        `${type === "cpu" ? "CPU workers" : `GPU ${index}`} ${paused ? "resumed" : "paused"}.`,
        paused ? "success" : "warn"
      );
      await refreshStatus();
    } catch (error) {
      setError(error.message);
    }
  });
  return button;
}

function updateStatus(status) {
  state.status = status;
  updatePoolUserCard();
  setConnection(status);
  updateStartupProgress(status);
  const running = Boolean(status.running);
  const connected = String(status.connectionStatus).toUpperCase() === "CONNECTED";
  byId("live-indicator").textContent = status.paused ? "PAUSED" : running ? (connected ? "LIVE" : "STARTING") : "IDLE";
  byId("live-indicator").classList.toggle("active", running && !status.paused);
  byId("metric-hashrate").textContent = formatRate(status.hashrate).replace(/ H\/s$| kH\/s$| MH\/s$/, "");
  const rateUnit = formatRate(status.hashrate).match(/(H\/s|kH\/s|MH\/s)$/)?.[0] || "H/s";
  const metricRateUnit = byId("metric-hashrate").nextElementSibling;
  metricRateUnit.textContent = rateUnit;
  byId("metric-hashrate-caption").textContent = status.paused ? "Workers are paused" : running ? "Live pool work in progress" : "Waiting for a mining session";
  byId("metric-accepted").textContent = formatInteger(status.accepted);
  byId("metric-rejected").textContent = `${formatInteger(status.rejected)} rejected`;
  byId("metric-uptime").textContent = formatDuration(status.uptimeSeconds);
  byId("metric-hashes").textContent = `${formatInteger(status.hashes)} hashes computed`;
  byId("metric-workers").textContent = formatInteger(status.threads);
  byId("metric-topology").textContent = `${status.physicalCores || 0} cores · ${status.maxThreads || 0} logical CPUs`;
  byId("chart-current").innerHTML = `${formatRate(status.hashrate).replace(/ (?:H\/s|kH\/s|MH\/s)$/, "")} <small>${rateUnit}</small>`;
  byId("chart-range").textContent = running ? `${status.connectionStatus || "Connecting"} · ${status.pool || ""}` : "Your session will appear here";
  byId("chart-cpu").textContent = formatRate(status.cpuHashrate);
  byId("chart-gpu").textContent = formatRate(status.gpuHashrate);
  byId("chart-difficulty").textContent = status.difficulty ? formatInteger(status.difficulty) : "—";
  startButton.disabled = running || state.requestInFlight;
  stopButton.disabled = !running;
  pauseButton.disabled = !running;
  pauseButton.innerHTML = status.paused ? "<span>▶</span>Resume" : "<span>Ⅱ</span>Pause";
  updateDeviceCards(status);

  const sample = {
    time: Date.now(),
    total: Number(status.hashrate) || 0,
    cpu: Number(status.cpuHashrate) || 0,
    gpu: Number(status.gpuHashrate) || 0
  };
  state.samples.push(sample);
  state.samples = state.samples.filter((point) => point.time > Date.now() - 60000).slice(-61);
  drawChart();
  if (status.event) {
    const kind = /accepted|connected|received/i.test(status.event) ? "success" : /error|failed|rejected/i.test(status.event) ? "error" : /paused|retry|warning/i.test(status.event) ? "warn" : "normal";
    addActivity(status.event, kind);
  }
}

function drawChart() {
  if (!chart || !context) return;
  const rect = chart.getBoundingClientRect();
  if (rect.width < 2 || rect.height < 2) return;
  const ratio = window.devicePixelRatio || 1;
  chart.width = Math.round(rect.width * ratio);
  chart.height = Math.round(rect.height * ratio);
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  const width = rect.width;
  const height = rect.height;
  context.clearRect(0, 0, width, height);
  const left = 2;
  const right = width - 2;
  const top = 7;
  const bottom = height - 7;
  for (let line = 0; line <= 4; line += 1) {
    const y = top + (bottom - top) * line / 4;
    context.beginPath();
    context.strokeStyle = "rgba(156, 174, 189, .13)";
    context.lineWidth = 1;
    context.setLineDash([3, 5]);
    context.moveTo(left, y);
    context.lineTo(right, y);
    context.stroke();
  }
  context.setLineDash([]);
  const maximum = Math.max(100, ...state.samples.map((point) => point.total)) * 1.15;
  const scaleLabels = document.querySelectorAll(".chart-y-labels span");
  [1, .75, .5, .25, 0].forEach((portion, index) => {
    if (scaleLabels[index]) {
      scaleLabels[index].textContent = formatRate(maximum * portion);
    }
  });
  const drawSeries = (key, color, lineWidth) => {
    if (state.samples.length < 2) return;
    const points = state.samples.map((point, index) => ({
      x: left + (right - left) * index / (state.samples.length - 1),
      y: bottom - (bottom - top) * Math.min(point[key], maximum) / maximum
    }));
    context.beginPath();
    context.moveTo(points[0].x, points[0].y);
    for (let index = 1; index < points.length; index += 1) {
      const previous = points[index - 1];
      const current = points[index];
      const middle = (previous.x + current.x) / 2;
      context.quadraticCurveTo(previous.x, previous.y, middle, (previous.y + current.y) / 2);
      if (index === points.length - 1) context.quadraticCurveTo(middle, (previous.y + current.y) / 2, current.x, current.y);
    }
    context.strokeStyle = color;
    context.lineWidth = lineWidth;
    context.lineJoin = "round";
    context.lineCap = "round";
    context.stroke();
    if (key === "total") {
      const fill = context.createLinearGradient(0, top, 0, bottom);
      fill.addColorStop(0, "rgba(241, 58, 139, .18)");
      fill.addColorStop(1, "rgba(241, 58, 139, 0)");
      context.lineTo(points[points.length - 1].x, bottom);
      context.lineTo(points[0].x, bottom);
      context.closePath();
      context.fillStyle = fill;
      context.fill();
    }
  };
  drawSeries("gpu", "#5bd6ae", 1.5);
  drawSeries("cpu", "#70bdf4", 1.5);
  drawSeries("total", "#f13a8b", 2.2);
}

function updateStartupProgress(status) {
  // Use real startup milestones rather than a time-based ETA; tuning and pool responses vary.
  const panel = byId("startup-progress");
  if (!state.startupStartedAt || state.quitting) {
    panel.hidden = true;
    return;
  }

  const now = Date.now();
  const elapsed = Math.floor((now - state.startupStartedAt) / 1000);
  byId("startup-elapsed").textContent = formatDuration(elapsed);
  const track = byId("startup-track");
  const fill = byId("startup-fill");
  panel.classList.remove("complete", "failed");

  if (status.mining) {
    if (!state.startupCompletedAt) state.startupCompletedAt = now;
    byId("startup-phase").textContent = "Mining started";
    byId("startup-step").textContent = "Startup complete";
    byId("startup-detail").textContent =
      `Mining is active after ${formatDuration(elapsed)}.`;
    state.startupProgressValue = 100;
    panel.classList.add("complete");
    if (now - state.startupCompletedAt >= 5000) {
      state.startupStartedAt = 0;
      state.startupCompletedAt = 0;
      panel.hidden = true;
      return;
    }
  } else if (status.connectionStatus === "ERROR") {
    if (!state.startupFailedAt) state.startupFailedAt = now;
    byId("startup-phase").textContent = "Miner could not start";
    byId("startup-step").textContent = "Startup failed";
    byId("startup-detail").textContent =
      status.event || "Check the pool and hardware settings, then try again.";
    panel.classList.add("failed");
    if (now - state.startupFailedAt >= 8000) {
      state.startupStartedAt = 0;
      state.startupFailedAt = 0;
      panel.hidden = true;
      return;
    }
  } else if (!status.running) {
    state.startupStartedAt = 0;
    state.startupCompletedAt = 0;
    state.startupFailedAt = 0;
    panel.hidden = true;
    return;
  } else {
    if (status.connectionStatus === "STOPPING") {
      state.startupStartedAt = 0;
      panel.hidden = true;
      return;
    }
    state.startupCompletedAt = 0;
    state.startupFailedAt = 0;
    const connection = String(status.connectionStatus || "").toUpperCase();
    const event = String(status.event || "");
    let phase = "Preparing mining hardware";
    let step = "Step 2 of 4 · Connect";
    let progress = 42;
    let detail = event || "Initializing selected devices and connecting to the pool.";

    if (connection === "TUNING") {
      phase = "Optimizing CPU configuration";
      step = "Step 1 of 4 · Tune";
      progress = 20;
      detail = event || "Measuring CPU worker configurations.";
    } else if (connection === "RECONNECTING") {
      phase = "Retrying pool connection";
      step = "Step 2 of 4 · Connect";
      progress = 42;
      detail = event || "The pool connection was interrupted; retrying.";
    } else if (connection === "CONNECTED" &&
               /authenticat/i.test(event)) {
      phase = "Authenticating with pool";
      step = "Step 3 of 4 · Authenticate";
      progress = 66;
      detail = event;
    } else if (connection === "CONNECTED") {
      phase = "Waiting for the first pool job";
      step = "Step 4 of 4 · Receive job";
      progress = 86;
      detail = event || "Connected; waiting for the pool to send work.";
    } else if (connection === "CONNECTING") {
      step = "Step 2 of 4 · Connect";
      progress = 42;
    }

    byId("startup-phase").textContent = phase;
    byId("startup-step").textContent = step;
    byId("startup-detail").textContent = detail;
    state.startupProgressValue = progress;
  }

  panel.hidden = false;
  track.setAttribute("aria-valuenow", String(state.startupProgressValue));
  track.setAttribute(
    "aria-valuetext",
    `${byId("startup-step").textContent}; ${byId("startup-phase").textContent}`
  );
  fill.style.width = `${state.startupProgressValue}%`;
}

function initializeHeroParticles() {
  const canvas = byId("hero-particles");
  const particleContext = canvas.getContext("2d");
  const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
  let particles = [];
  let width = 0;
  let height = 0;
  let pixelRatio = 1;
  let animationFrame = 0;
  let lastFrameTime = 0;

  function drawFrame() {
    particleContext.clearRect(0, 0, width, height);
    for (let index = 0; index < particles.length; index += 1) {
      const particle = particles[index];
      for (let otherIndex = index + 1; otherIndex < particles.length; otherIndex += 1) {
        const other = particles[otherIndex];
        const dx = particle.x - other.x;
        const dy = particle.y - other.y;
        const distance = Math.hypot(dx, dy);
        if (distance >= 145) continue;
        const opacity = 0.16 * (1 - distance / 145);
        particleContext.beginPath();
        particleContext.strokeStyle = `rgba(255, 91, 164, ${opacity})`;
        particleContext.lineWidth = 0.7;
        particleContext.moveTo(particle.x, particle.y);
        particleContext.lineTo(other.x, other.y);
        particleContext.stroke();
      }
    }

    for (const particle of particles) {
      particleContext.fillStyle = `rgba(255, 75, 151, ${particle.opacity})`;
      particleContext.fillRect(
        particle.x - particle.size / 2,
        particle.y - particle.size / 2,
        particle.size,
        particle.size
      );
    }
  }

  function animate(timestamp) {
    animationFrame = 0;
    if (document.hidden) return;
    if (timestamp - lastFrameTime >= 33) {
      lastFrameTime = timestamp;
      if (!reducedMotion.matches) {
        for (const particle of particles) {
          particle.x += particle.velocityX;
          particle.y += particle.velocityY;
          if (particle.x < -4) particle.x = width + 4;
          else if (particle.x > width + 4) particle.x = -4;
          if (particle.y < -4) particle.y = height + 4;
          else if (particle.y > height + 4) particle.y = -4;
        }
      }
      drawFrame();
    }
    if (!reducedMotion.matches) {
      animationFrame = window.requestAnimationFrame(animate);
    }
  }

  function startAnimation() {
    if (animationFrame) {
      window.cancelAnimationFrame(animationFrame);
      animationFrame = 0;
    }
    lastFrameTime = 0;
    if (!document.hidden) {
      animationFrame = window.requestAnimationFrame(animate);
    }
  }

  function resizeCanvas() {
    const bounds = canvas.getBoundingClientRect();
    width = bounds.width;
    height = bounds.height;
    pixelRatio = Math.min(window.devicePixelRatio || 1, 1.5);
    canvas.width = Math.round(width * pixelRatio);
    canvas.height = Math.round(height * pixelRatio);
    particleContext.setTransform(pixelRatio, 0, 0, pixelRatio, 0, 0);

    const count = Math.min(140, Math.max(24, Math.round(width * height / 5000)));
    particles = Array.from({ length: count }, () => ({
      x: Math.random() * width,
      y: Math.random() * height,
      velocityX: (Math.random() - 0.5) * 0.24,
      velocityY: (Math.random() - 0.5) * 0.24,
      size: 2 + Math.random() * 2.2,
      opacity: 0.42 + Math.random() * 0.48
    }));
    startAnimation();
  }

  window.addEventListener("resize", resizeCanvas, { passive: true });
  document.addEventListener("visibilitychange", startAnimation);
  reducedMotion.addEventListener?.("change", startAnimation);
  if ("ResizeObserver" in window) {
    new ResizeObserver(resizeCanvas).observe(canvas.parentElement);
  }
  resizeCanvas();
}

async function refreshStatus() {
  if (state.requestInFlight || state.quitting || !state.token) return;
  try {
    const status = await requestJson("/api/status");
    updateStatus(status);
  } catch (error) {
    setConnection({ connectionStatus: "ERROR", running: false });
    updateStartupProgress({
      running: false,
      connectionStatus: "ERROR",
      event: error.message
    });
    setError(error.message);
  }
}

async function loadDevices() {
  try {
    const devices = await requestJson("/api/devices");
    state.devices = devices;
    const threads = byId("threads");
    for (let count = 1; count <= Math.min(devices.logicalProcessors || 1, 64); count += 1) {
      const option = document.createElement("option");
      option.value = String(count);
      option.textContent = `${count} thread${count === 1 ? "" : "s"}`;
      threads.append(option);
    }
    byId("metric-topology").textContent = `${devices.physicalCores} cores · ${devices.logicalProcessors} logical CPUs`;
    if (!devices.gpus?.length) byId("device-count").textContent = "No OpenCL GPUs detected";
    updateDeviceCards(state.status || {
      maxThreads: devices.logicalProcessors,
      physicalCores: devices.physicalCores,
      gpus: []
    });
  } catch (error) {
    setError(`Could not inspect local hardware: ${error.message}`);
  }
}

async function startMining(event) {
  event.preventDefault();
  setError("");
  const settings = {
    pool: byId("pool").value.trim(),
    wallet: byId("wallet").value.trim(),
    password: byId("password").value || "x",
    backend: byId("backend").value,
    threads: byId("threads").value,
    nonceOffset: Number(byId("nonce-offset").value || 39),
    tls: byId("tls").checked
  };
  if (!settings.pool || !settings.wallet) {
    setError("Enter both a pool endpoint and Bytecoin wallet address.");
    return;
  }
  if (settings.backend !== "cpu" && settings.nonceOffset !== 39) {
    setError("GPU mining currently requires the pool nonce offset to be 39.");
    return;
  }
  saveSettings();
  state.startupStartedAt = Date.now();
  state.startupCompletedAt = 0;
  state.startupFailedAt = 0;
  state.startupProgressValue = 5;
  byId("startup-progress").hidden = false;
  byId("startup-progress").classList.remove("complete", "failed");
  byId("startup-phase").textContent = "Preparing mining session";
  byId("startup-step").textContent = "Preparing";
  byId("startup-detail").textContent =
    "Startup time varies by hardware and pool response.";
  byId("startup-track").setAttribute("aria-valuenow", "5");
  byId("startup-fill").style.width = "5%";
  state.requestInFlight = true;
  startButton.disabled = true;
  startButton.textContent = "Preparing miner…";
  try {
    await requestJson("/api/start", {
      method: "POST",
      body: JSON.stringify(settings)
    });
    byId("password").value = "";
    setError("");
    addActivity("Miner start requested; preparing hardware.", "success");
    await refreshStatus();
  } catch (error) {
    state.startupStartedAt = 0;
    byId("startup-progress").hidden = true;
    setError(error.message);
  } finally {
    state.requestInFlight = false;
    startButton.innerHTML = '<span class="button-play">▶</span>Start mining';
    await refreshStatus();
  }
}

async function stopMining() {
  setError("");
  stopButton.disabled = true;
  try {
    await requestJson("/api/stop", { method: "POST", body: "{}" });
    addActivity("Miner stop requested.", "warn");
  } catch (error) {
    setError(error.message);
  } finally {
    await refreshStatus();
  }
}

async function quitMiner() {
  if (!window.confirm(
    "Quit BCN Miner? This will stop active mining and close the dashboard."
  )) {
    return;
  }

  setError("");
  state.quitting = true;
  quitButton.disabled = true;
  try {
    await requestJson("/api/quit", {
      method: "POST",
      body: "{}"
    });
    addActivity("Quit requested; stopping mining and closing the dashboard.", "warn");
    byId("connection-pill").className = "connection-pill warning";
    byId("connection-label").textContent = "Shutting down";
    byId("live-indicator").textContent = "QUITTING";
    byId("live-indicator").classList.remove("active");
    quitButton.innerHTML = "<span aria-hidden=\"true\">✓</span>Quitting…";
    document.querySelectorAll("button, input, select").forEach((control) => {
      control.disabled = true;
    });
  } catch (error) {
    state.quitting = false;
    quitButton.disabled = false;
    setError(error.message);
  }
}

async function togglePause() {
  if (!state.status) return;
  const isPaused = Boolean(state.status.paused);
  try {
    await requestJson("/api/control", {
      method: "POST",
      body: JSON.stringify({ action: isPaused ? "resume-all" : "pause-all" })
    });
    await refreshStatus();
  } catch (error) {
    setError(error.message);
  }
}

async function initialize() {
  byId("miner-form").addEventListener("submit", startMining);
  byId("pool-api-form").addEventListener("submit", loadPoolApi);
  byId("pool").addEventListener("input", syncPoolStatsApiUrl);
  byId("pool-api-url").addEventListener("input", () => {
    if (byId("pool-api-url").value.trim() !== bytecoinStatsApiUrl) {
      poolStatsApiUrlAutoFilled = false;
    }
  });
  byId("wallet").addEventListener("input", updatePoolUserCard);
  byId("stop-button").addEventListener("click", stopMining);
  byId("pause-button").addEventListener("click", togglePause);
  byId("quit-button").addEventListener("click", quitMiner);
  byId("refresh-button").addEventListener("click", refreshStatus);
  byId("clear-log").addEventListener("click", () => {
    state.lastEvent = "";
    byId("activity-list").innerHTML = '<div class="activity-empty"><span class="empty-pulse"></span><span>Activity log cleared.</span></div>';
  });
  window.addEventListener("resize", drawChart);
  document.querySelectorAll(".nav-link").forEach((link) => {
    link.addEventListener("click", () => {
      document.querySelectorAll(".nav-link").forEach((item) => item.classList.remove("active"));
      link.classList.add("active");
    });
  });

  try {
    const bootstrap = await requestJson("/api/bootstrap");
    state.token = bootstrap.token;
    await loadDevices();
    loadSettings(bootstrap.config);
    syncPoolStatsApiUrl();
    await refreshStatus();
    window.setInterval(refreshStatus, 1000);
  } catch (error) {
    setError(`Could not connect to the local miner service: ${error.message}`);
  }
}

initializeHeroParticles();
initialize();
