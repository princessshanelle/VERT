const POLL_MS = 2500;
let lastGoodData = null;

function pillClass(kind) {
  return { neutral: 'pill-neutral', good: 'pill-good', active: 'pill-active', bad: 'pill-bad' }[kind];
}

function renderDoor(state) {
  const el = document.getElementById('doorState');
  el.textContent = state;
  const map = { open: 'good', closed: 'neutral', opening: 'active', closing: 'active', error: 'bad', unknown: 'neutral' };
  el.className = 'pill ' + pillClass(map[state] || 'neutral');
}

function renderPump(on, auto) {
  const el = document.getElementById('pumpState');
  el.textContent = on ? 'running' : 'off';
  el.className = 'pill ' + pillClass(on ? 'good' : 'neutral');
  document.getElementById('autoToggle').checked = auto;
  document.getElementById('manualPumpBtns').style.opacity = auto ? '0.4' : '1';
  document.getElementById('manualPumpBtns').style.pointerEvents = auto ? 'none' : 'auto';
}

function renderBulb(on) {
  const el = document.getElementById('bulbState');
  el.textContent = on ? 'on' : 'off';
  el.className = 'pill ' + pillClass(on ? 'good' : 'neutral');
}

function renderSoilMiniGrid(values) {
  const wrap = document.getElementById('soilMiniGrid');
  wrap.innerHTML = '';
  values.forEach((v, i) => {
    const bar = document.createElement('div');
    bar.className = 'mini-bar';
    bar.style.height = Math.max(4, v) + '%';
    bar.style.background = v < 30 ? 'var(--warning)' : 'var(--accent)';
    const label = document.createElement('div');
    label.className = 'mini-bar-label';
    label.textContent = 'S' + (i + 1);
    bar.appendChild(label);
    wrap.appendChild(bar);
  });
}

function fillThresholdFormIfIdle(d) {
  // Don't clobber the form while the farmer is actively typing in it.
  if (document.activeElement && document.activeElement.closest('#thresholdForm')) return;
  document.getElementById('soilMin').value = d.soilMin;
  document.getElementById('soilMax').value = d.soilMax;
  document.getElementById('tempMin').value = d.tempMin;
  document.getElementById('tempMax').value = d.tempMax;
  document.getElementById('phMin').value = d.phMin;
  document.getElementById('phMax').value = d.phMax;
}

async function refresh() {
  const badge = document.getElementById('connIndicator');
  try {
    const res = await fetch('/data');
    if (!res.ok) throw new Error('bad status');
    const d = await res.json();
    lastGoodData = d;

    badge.textContent = 'Live';
    badge.className = 'conn-badge conn-ok';

    document.getElementById('soilAvg').textContent = d.soilAvg.toFixed(0) + '%';
    renderSoilMiniGrid(d.soil);

    document.getElementById('ph1').textContent = d.ph[0].toFixed(2);
    document.getElementById('ph2').textContent = d.ph[1].toFixed(2);

    document.getElementById('temp').innerHTML = d.temp >= 0 ? d.temp.toFixed(1) + '&deg;C' : '--';
    document.getElementById('humidity').textContent = d.humidity >= 0 ? d.humidity.toFixed(0) + '%' : '--';

    renderDoor(d.doorState);
    renderPump(d.pumpOn, d.autoIrrigation);
    renderBulb(d.bulbOn);
    fillThresholdFormIfIdle(d);
  } catch (e) {
    badge.textContent = 'Offline';
    badge.className = 'conn-badge conn-bad';
  }
}

async function sendDoor(action) {
  await fetch('/door?action=' + action);
  refresh();
}

async function sendPump(state) {
  await fetch('/pump?state=' + state);
  refresh();
}

async function sendBulb(state) {
  await fetch('/bulb?state=' + state);
  refresh();
}

async function toggleAuto() {
  const checked = document.getElementById('autoToggle').checked;
  await fetch('/mode?auto=' + (checked ? '1' : '0'));
  refresh();
}

async function saveThresholds(evt) {
  evt.preventDefault();
  const params = new URLSearchParams({
    soilMin: document.getElementById('soilMin').value,
    soilMax: document.getElementById('soilMax').value,
    tempMin: document.getElementById('tempMin').value,
    tempMax: document.getElementById('tempMax').value,
    phMin: document.getElementById('phMin').value,
    phMax: document.getElementById('phMax').value,
  });
  const status = document.getElementById('saveStatus');
  try {
    await fetch('/setThresholds', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: params.toString() });
    status.textContent = 'Saved';
    setTimeout(() => (status.textContent = ''), 2500);
  } catch (e) {
    status.textContent = 'Could not save - check connection';
  }
}

refresh();
setInterval(refresh, POLL_MS);
