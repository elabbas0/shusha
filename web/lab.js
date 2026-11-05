// The lab page: launches Shusha machines (each in its own window) on one
// virtual Ethernet segment, lists them, and shows every frame on that
// segment so you can check what actually crosses the "network".

import { LAN_CHANNEL, INVITE_CHANNEL, claimMachineNumber, machineIdentity, watchMachines } from './lan.js';

const listEl = document.getElementById('machines');
const addBtn = document.getElementById('add');
// Emulator options on the lab URL (e.g. ?accel=...) are passed to each machine.
const passOn = (() => {
  const q = new URLSearchParams(location.search);
  const s = q.toString();
  return s ? '&' + s : '';
})();

// ---- machines ---------------------------------------------------------------
// Each machine runs in its own window.  Two emulators sharing one page (and
// so one browser main thread) can deadlock under load, while separate
// windows opened without an opener get their own renderer each.

let opened = 0;
async function openMachine() {
  addBtn.disabled = true;
  try {
    const n = await claimMachineNumber(1);
    const sw = screen.availWidth || 1600, sh = screen.availHeight || 1000;
    const w = Math.max(640, Math.floor(sw / 2)), h = Math.max(480, Math.min(sh, Math.floor(w / 1.6) + 170));
    const left = (opened % 2) * (sw - w), top = Math.floor(opened / 2) % 2 * Math.max(0, sh - h);
    opened++;
    window.open(`machine.html?n=${n}&autostart${passOn}`, '_blank',
      `noopener,popup,width=${w},height=${h},left=${left},top=${top}`);
  } finally {
    setTimeout(() => { addBtn.disabled = false; }, 1200); // let the new machine claim its number
  }
}
addBtn.addEventListener('click', openMachine);

const rows = new Map();
watchMachines((alive, status) => {
  for (const [n, row] of rows) if (!alive.has(n)) { row.remove(); rows.delete(n); }
  for (const n of [...alive].sort((a, b) => a - b)) {
    let row = rows.get(n);
    if (!row) {
      row = document.createElement('li');
      row.innerHTML = `<strong>Machine ${n}</strong> <span class="muted">${machineIdentity(n).ip}</span> <span class="status muted"></span>`;
      rows.set(n, row);
      const next = [...rows.keys()].filter((k) => k > n).sort((a, b) => a - b)[0];
      listEl.insertBefore(row, next ? rows.get(next) : null);
    }
    row.querySelector('.status').textContent = status.get(n) || '';
  }
  document.getElementById('empty').hidden = alive.size > 0;
});

// ---- invites ----------------------------------------------------------------

const inviteEl = document.getElementById('lab-invite');
new BroadcastChannel(INVITE_CHANNEL).onmessage = (e) => {
  if (!e.data) return;
  inviteEl.hidden = false;
  inviteEl.querySelector('.from').textContent = `machine ${e.data.from}`;
  inviteEl.querySelector('code').textContent = e.data.code;
};
document.getElementById('lab-copy').addEventListener('click', () => {
  navigator.clipboard.writeText(inviteEl.querySelector('code').textContent).catch(() => {});
});

// ---- the wire ---------------------------------------------------------------
// Every machine posts each Ethernet frame it sends on LAN_CHANNEL; this page
// hears all of them, like a capture on a hub port.

const KEEP_BYTES = 8 * 1048576;
const captured = [];   // { t, from, frame }
let keptBytes = 0;
const stats = { frames: 0, bytes: 0, ws: 0, wsBytes: 0, hist: new Uint32Array(256) };
const rowsEl = document.getElementById('wire-rows');
const statsEl = document.getElementById('wire-stats');
let paused = false;
const pauseBtn = document.getElementById('wire-pause');
pauseBtn.addEventListener('click', () => {
  paused = !paused;
  pauseBtn.textContent = paused ? 'Resume' : 'Pause';
  pauseBtn.setAttribute('aria-pressed', String(paused));
});

const hex = (b) => b.toString(16).padStart(2, '0');
const mac = (f, o) => Array.from(f.subarray(o, o + 6), hex).join(':');
const ip4 = (f, o) => `${f[o]}.${f[o + 1]}.${f[o + 2]}.${f[o + 3]}`;

function describe(f) {
  const type = (f[12] << 8) | f[13];
  if (type === 0x0806) {
    const op = (f[20] << 8) | f[21];
    return { proto: 'ARP', src: ip4(f, 28), dst: ip4(f, 38),
             info: op === 1 ? `who has ${ip4(f, 38)}?` : `${ip4(f, 28)} is at ${mac(f, 22)}` };
  }
  if (type !== 0x0800) return { proto: '0x' + type.toString(16), src: mac(f, 6), dst: mac(f, 0), info: '' };
  const ihl = (f[14] & 15) * 4;
  const p = f[23];
  const l4 = 14 + ihl;
  const src = ip4(f, 26), dst = ip4(f, 30);
  if (p === 17) {
    const sp = (f[l4] << 8) | f[l4 + 1], dp = (f[l4 + 2] << 8) | f[l4 + 3];
    const payload = f.subarray(l4 + 8);
    const ws = sp === 7100 || dp === 7100;
    return { proto: 'UDP', src: `${src}:${sp}`, dst: `${dst}:${dp}`, payload, ws,
             info: ws ? 'Shusha secure channel' : '' };
  }
  if (p === 1) return { proto: 'ICMP', src, dst, info: f[l4] === 8 ? 'echo request' : f[l4] === 0 ? 'echo reply' : '' };
  if (p === 6) return { proto: 'TCP', src, dst, info: '' };
  return { proto: 'IP/' + p, src, dst, info: '' };
}

function entropy(hist, total) {
  if (!total) return 0;
  let h = 0;
  for (const c of hist) if (c) { const q = c / total; h -= q * Math.log2(q); }
  return h;
}

function renderStats() {
  const e = entropy(stats.hist, stats.wsBytes);
  statsEl.textContent = `${stats.frames} frames, ${(stats.bytes / 1024).toFixed(1)} KB seen. ` +
    `Secure-channel packets (UDP 7100): ${stats.ws}, ${(stats.wsBytes / 1024).toFixed(1)} KB of payload` +
    (stats.wsBytes > 4096 ? `, entropy ${e.toFixed(3)} bits per byte (8 = looks random).` : '.');
}

let pendingRows = [];
function flushRows() {
  if (!pendingRows.length) return;
  const frag = document.createDocumentFragment();
  for (const r of pendingRows) frag.prepend(r);
  rowsEl.prepend(frag);
  pendingRows = [];
  while (rowsEl.children.length > 200) rowsEl.lastChild.remove();
}
setInterval(flushRows, 250);
setInterval(renderStats, 1000);

new BroadcastChannel(LAN_CHANNEL).onmessage = (e) => {
  const d = e.data;
  if (!d || !d.frame) return;
  const f = d.frame;
  const t = new Date();
  captured.push({ t, from: d.from, frame: f });
  keptBytes += f.length;
  while (keptBytes > KEEP_BYTES) keptBytes -= captured.shift().frame.length;
  stats.frames++;
  stats.bytes += f.length;
  const info = describe(f);
  if (info.ws && info.payload) {
    stats.ws++;
    stats.wsBytes += info.payload.length;
    for (const b of info.payload) stats.hist[b]++;
  }
  if (paused) return;
  const tr = document.createElement('tr');
  const peek = info.payload ? Array.from(info.payload.subarray(0, 16), hex).join(' ') : '';
  for (const text of [t.toLocaleTimeString(), `machine ${d.from}`, info.proto, info.src, info.dst,
                      String(f.length), info.info, peek]) {
    const td = document.createElement('td');
    td.textContent = text;
    tr.append(td);
  }
  pendingRows.push(tr);
};

// "Is this text on the wire?"  Searches every frame still kept, as UTF-8 and
// as UTF-16 (in case something sends wide strings).
function indexOfBytes(hay, needle) {
  outer: for (let i = 0; i + needle.length <= hay.length; i++) {
    for (let j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
    return i;
  }
  return -1;
}

document.getElementById('search-form').addEventListener('submit', (e) => {
  e.preventDefault();
  const text = document.getElementById('search').value;
  const out = document.getElementById('search-result');
  if (!text) { out.textContent = ''; return; }
  const utf8 = new TextEncoder().encode(text);
  const utf16 = new Uint8Array([...text].flatMap((c) => [c.charCodeAt(0) & 255, c.charCodeAt(0) >> 8]));
  const lower = new TextEncoder().encode(text.toLowerCase());
  let hits = 0, first = null;
  for (const c of captured) {
    if (indexOfBytes(c.frame, utf8) >= 0 || indexOfBytes(c.frame, utf16) >= 0 ||
        (lower.length && indexOfBytes(c.frame, lower) >= 0)) {
      hits++;
      if (!first) first = c;
    }
  }
  const kb = (keptBytes / 1024).toFixed(1);
  out.className = hits ? 'found' : 'clean';
  out.textContent = hits
    ? `Found in ${hits} frame(s), first from machine ${first.from} at ${first.t.toLocaleTimeString()}. It crossed the network in the clear.`
    : `Not found in any of the ${captured.length} frames captured (${kb} KB). It never crossed the network in the clear.`;
});

