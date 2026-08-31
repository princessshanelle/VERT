// Hostname/IP of the camera board. Matches MDNS_NAME in XIAO_ESP32S3_CAM.ino
// by default, but is editable from the page (see the "Camera address" field)
// and persisted in localStorage - this matters because plenty of browsers,
// Windows Chrome/Edge especially, won't resolve .local (mDNS) hostnames
// without extra software (Bonjour, etc.) installed. When that's the case,
// the farmer can paste the camera's IP address (from its Serial Monitor
// output at boot) here instead, without touching any code.
const CAM_HOST_KEY = 'vertCamHost';
const DEFAULT_CAM_HOST = 'http://vert-cam.local';

function getCamHost() {
  return (localStorage.getItem(CAM_HOST_KEY) || DEFAULT_CAM_HOST).replace(/\/+$/, '');
}

function saveCamHost() {
  const input = document.getElementById('camHostInput');
  const status = document.getElementById('camHostStatus');
  let value = input.value.trim();
  if (!value) {
    localStorage.removeItem(CAM_HOST_KEY);
    value = DEFAULT_CAM_HOST;
  } else {
    if (!/^https?:\/\//i.test(value)) value = 'http://' + value; // tolerate bare host/IP entry
    value = value.replace(/\/+$/, '');
    localStorage.setItem(CAM_HOST_KEY, value);
  }
  input.value = value;
  status.textContent = 'Saved';
  setTimeout(() => (status.textContent = ''), 2000);
  // Reconnect immediately using the new address.
  if (streamOn) startStream();
  pollStatus();
}

const STATUS_POLL_MS = 3000;
const HISTORY_KEY = 'vertPlantHealthHistory';
const HISTORY_MAX_SAMPLES = 200;

const labelPillClass = { healthy: 'pill-good', sick: 'pill-bad' };
const labelChartColor = {
  healthy: '#6fa85b',
  sick: '#a23b2e',
};
function colorForLabel(label) {
  return labelChartColor[label] || '#7a4e2d';
}

let streamOn = false;
let streamAbortController = null;
let streamObjectUrl = null;

// ===========================================================================
// --- MJPEG streaming ---
// Plain <img src="…mjpeg…"> relies on the browser natively re-painting on
// each multipart/x-mixed-replace part, which isn't reliable everywhere -
// some browsers just decode the first JPEG part and never repaint after
// that, which looks exactly like a frozen/static image even though the
// camera board is sending a continuous stream. To make this work the same
// way everywhere, we instead fetch() the stream ourselves, manually split
// the multipart body on the "--frame" boundary the XIAO sends, and paint
// each JPEG part into the <img> as a Blob URL as it arrives.
// ===========================================================================
async function runMjpegStream(signal) {
  const overlay = document.getElementById('videoOverlay');
  const img = document.getElementById('camStream');
  const url = getCamHost() + ':81/stream?_=' + Date.now(); // cache-bust, fresh connection

  const res = await fetch(url, { signal });
  if (!res.ok || !res.body) throw new Error('bad stream response');

  const reader = res.body.getReader();
  let buffer = new Uint8Array(0);

  function appendChunk(chunk) {
    const merged = new Uint8Array(buffer.length + chunk.length);
    merged.set(buffer, 0);
    merged.set(chunk, buffer.length);
    buffer = merged;
  }

  // JPEG frames always start with SOI (0xFFD8) and end with EOI (0xFFD9).
  // Scanning for those markers directly is more robust than parsing the
  // multipart Content-Length header/boundary text byte-by-byte.
  function findMarker(marker, from) {
    for (let i = from; i < buffer.length - 1; i++) {
      if (buffer[i] === marker[0] && buffer[i + 1] === marker[1]) return i;
    }
    return -1;
  }

  let firstFrame = true;
  while (true) {
    const { done, value } = await reader.read();
    if (done) throw new Error('stream ended');
    appendChunk(value);

    let soi = findMarker([0xff, 0xd8], 0);
    while (soi !== -1) {
      const eoi = findMarker([0xff, 0xd9], soi + 2);
      if (eoi === -1) break; // frame not fully received yet, wait for more chunks
      const frameBytes = buffer.slice(soi, eoi + 2);
      const blob = new Blob([frameBytes], { type: 'image/jpeg' });
      const newUrl = URL.createObjectURL(blob);
      const oldUrl = streamObjectUrl;
      img.src = newUrl;
      streamObjectUrl = newUrl;
      if (oldUrl) URL.revokeObjectURL(oldUrl);

      if (firstFrame) {
        overlay.style.display = 'none';
        document.getElementById('videoFrame').classList.add('is-live');
        firstFrame = false;
      }

      buffer = buffer.slice(eoi + 2);
      soi = findMarker([0xff, 0xd8], 0);
    }

    // Cap runaway buffer growth if we somehow never find a full frame
    // (e.g. a non-JPEG response) instead of leaking memory forever.
    if (buffer.length > 2_000_000) throw new Error('stream buffer overflow');
  }
}

async function startStream() {
  streamAbortController = new AbortController();
  const overlay = document.getElementById('videoOverlay');
  overlay.textContent = 'Connecting to camera…';
  overlay.style.display = 'flex';

  try {
    await runMjpegStream(streamAbortController.signal);
  } catch (e) {
    if (e.name === 'AbortError') return; // expected when the toggle is switched off
    document.getElementById('videoFrame').classList.remove('is-live');
    overlay.textContent = 'Camera stream unavailable';
    overlay.style.display = 'flex';
    // Auto-retry after a moment if the toggle is still on - a dropped
    // connection shouldn't require the farmer to manually flip the switch.
    if (streamOn) setTimeout(() => { if (streamOn) startStream(); }, 2000);
  }
}

function stopStream() {
  if (streamAbortController) {
    streamAbortController.abort();
    streamAbortController = null;
  }
  if (streamObjectUrl) {
    URL.revokeObjectURL(streamObjectUrl);
    streamObjectUrl = null;
  }
  document.getElementById('camStream').removeAttribute('src');
}

function setStreamButtonState(on) {
  const btn = document.getElementById('streamToggleBtn');
  btn.setAttribute('aria-pressed', on ? 'true' : 'false');
  btn.title = on ? 'Stop live stream' : 'Start live stream';
  btn.classList.toggle('icon-btn-play', !on);
  btn.classList.toggle('icon-btn-stop', on);
  btn.innerHTML = on
    ? '<svg viewBox="0 0 24 24" width="16" height="16"><rect x="6" y="6" width="12" height="12" fill="currentColor"/></svg><span>Stop</span>'
    : '<svg viewBox="0 0 24 24" width="16" height="16"><path d="M6 4l14 8-14 8z" fill="currentColor"/></svg><span>Start</span>';
}

function toggleStream() {
  streamOn = !streamOn;
  setStreamButtonState(streamOn);
  document.getElementById('videoFrame').classList.remove('is-live');
  if (streamOn) {
    startStream();
  } else {
    stopStream();
    const overlay = document.getElementById('videoOverlay');
    overlay.textContent = 'Stream is off — press Start to watch';
    overlay.style.display = 'flex';
  }
}

function toggleCamSettings() {
  const panel = document.getElementById('camSettingsPanel');
  const btn = document.getElementById('camSettingsBtn');
  const willShow = panel.hidden;
  panel.hidden = !willShow;
  btn.setAttribute('aria-expanded', willShow ? 'true' : 'false');
  btn.classList.toggle('icon-btn-active', willShow);
}

// ===========================================================================
// --- Plant health history (logged client-side from /status polls) ---
// ===========================================================================
function loadHistory() {
  try {
    return JSON.parse(localStorage.getItem(HISTORY_KEY)) || [];
  } catch (e) {
    return [];
  }
}

function saveHistory(history) {
  localStorage.setItem(HISTORY_KEY, JSON.stringify(history));
}

function recordSample(label, confidence) {
  const history = loadHistory();
  history.push({ t: Date.now(), label, confidence });
  while (history.length > HISTORY_MAX_SAMPLES) history.shift();
  saveHistory(history);
  return history;
}

// Marks a break in the chart line for a poll where nothing was confidently
// detected (see CONFIDENCE_THRESHOLD on the XIAO) - drawHealthChart skips
// plotting a point here and doesn't draw a connecting line across it, so an
// empty/unclear frame shows as a real gap instead of a fabricated 0% or a
// misleading low-confidence guess.
function recordGap() {
  const history = loadHistory();
  if (history.length > 0 && history[history.length - 1].gap) return history; // don't pile up consecutive gaps
  history.push({ t: Date.now(), gap: true });
  while (history.length > HISTORY_MAX_SAMPLES) history.shift();
  saveHistory(history);
  return history;
}

function drawHealthChart(history) {
  const canvas = document.getElementById('healthChart');
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height;
  const padL = 40, padR = 12, padT = 12, padB = 24;

  ctx.clearRect(0, 0, w, h);

  // gridlines + y-axis labels (0/50/100%)
  ctx.strokeStyle = '#c9c4ae';
  ctx.fillStyle = '#56604f';
  ctx.font = '11px monospace';
  ctx.lineWidth = 1;
  [0, 0.5, 1].forEach((frac) => {
    const y = padT + (1 - frac) * (h - padT - padB);
    ctx.beginPath();
    ctx.moveTo(padL, y);
    ctx.lineTo(w - padR, y);
    ctx.stroke();
    ctx.fillText(Math.round(frac * 100) + '%', 4, y + 4);
  });

  if (history.length === 0 || history.every((s) => s.gap)) {
    ctx.fillStyle = '#56604f';
    ctx.fillText('No confident detections logged yet', padL + 10, h / 2);
    return;
  }

  const plotW = w - padL - padR;
  const plotH = h - padT - padB;
  const n = history.length;
  const xFor = (i) => padL + (n === 1 ? plotW : (i / (n - 1)) * plotW);
  const yFor = (s) => padT + (1 - s.confidence) * plotH;

  // Line connecting confidence values - broken (a moveTo instead of lineTo)
  // wherever a gap sample sits, so periods with no confident detection show
  // as an actual break rather than a misleading straight interpolation.
  ctx.beginPath();
  ctx.strokeStyle = '#2f5233';
  ctx.lineWidth = 1.5;
  let penDown = false;
  history.forEach((s, i) => {
    if (s.gap) { penDown = false; return; }
    const x = xFor(i), y = yFor(s);
    if (!penDown) { ctx.moveTo(x, y); penDown = true; }
    else ctx.lineTo(x, y);
  });
  ctx.stroke();

  // points, colored by label - gaps have no point at all
  history.forEach((s, i) => {
    if (s.gap) return;
    const x = xFor(i), y = yFor(s);
    ctx.beginPath();
    ctx.fillStyle = colorForLabel(s.label);
    ctx.arc(x, y, 3, 0, Math.PI * 2);
    ctx.fill();
  });
}

function drawChartLegend(history) {
  const labels = [...new Set(history.filter((s) => !s.gap).map((s) => s.label))];
  const legend = document.getElementById('chartLegend');
  legend.innerHTML = '';
  labels.forEach((label) => {
    const item = document.createElement('span');
    item.className = 'legend-item';
    item.innerHTML = `<span class="legend-swatch" style="background:${colorForLabel(label)}"></span>${label}`;
    legend.appendChild(item);
  });
}

async function pollStatus() {
  const badge = document.getElementById('camConnIndicator');
  try {
    const res = await fetch(getCamHost() + '/status');
    if (!res.ok) throw new Error('bad status');
    const d = await res.json();

    badge.textContent = 'Live';
    badge.className = 'conn-badge conn-ok';

    const labelEl = document.getElementById('predictionLabel');
    const confidenceValueEl = document.getElementById('confidenceValue');
    const confidenceBarEl = document.getElementById('confidenceBar');
    const ageSec = Math.round(d.ageMs / 1000);

    if (d.detected) {
      labelEl.textContent = d.label;
      labelEl.className = 'pill ' + (labelPillClass[d.label] || 'pill-neutral');
      const pct = Math.round(d.confidence * 100);
      confidenceValueEl.textContent = pct + '%';
      confidenceBarEl.style.width = pct + '%';
      document.getElementById('predictionAge').textContent =
        ageSec < 2 ? 'just now' : `updated ${ageSec}s ago`;
    } else {
      // Below CONFIDENCE_THRESHOLD on the camera board - nothing confidently
      // recognized in frame. Show that plainly instead of a shaky low-% guess.
      labelEl.textContent = 'no confident detection';
      labelEl.className = 'pill pill-neutral';
      confidenceValueEl.textContent = '--';
      confidenceBarEl.style.width = '0%';
      document.getElementById('predictionAge').textContent =
        ageSec < 2 ? 'checked just now' : `last checked ${ageSec}s ago`;
    }

    // Only log a new history point once per fresh inference (ageMs resets
    // to ~0 right after the camera board runs a new prediction), so we
    // don't pile up duplicate samples for the same reading every poll.
    if (d.ageMs < STATUS_POLL_MS) {
      const history = d.detected ? recordSample(d.label, d.confidence) : recordGap();
      drawHealthChart(history);
      drawChartLegend(history);
    }
  } catch (e) {
    badge.textContent = 'Offline';
    badge.className = 'conn-badge conn-bad';
  }
}

document.getElementById('camHostInput').value = getCamHost();

// Stream starts OFF by default - farmer opts in via the toggle to avoid
// pulling continuous MJPEG bandwidth just from having the dashboard open.
drawHealthChart(loadHistory());
drawChartLegend(loadHistory());
pollStatus();
setInterval(pollStatus, STATUS_POLL_MS);
