// Shared by the machine page and the lab page: the names of the channels
// that join every Shusha machine open in this browser, machine numbering,
// and each machine's hardware identity.

// Ethernet frames: { from: machine number, frame: Uint8Array }
export const LAN_CHANNEL = 'shusha-lan';
// Invite codes seen on a machine's console: { from, code }
export const INVITE_CHANNEL = 'shusha-invites';
// Presence: { type: 'here' | 'who' | 'bye' | 'status', n, text }
const CTL_CHANNEL = 'shusha-lan-ctl';

// Machine k has MAC 52:54:00:12:00:kk, so Shusha gives it the link-local
// address 169.254.0.k (kernel/net/net.c), the same scheme as scripts/lan.py.
export function machineIdentity(n) {
  const hex = n.toString(16).padStart(2, '0');
  return {
    n,
    mac: `52:54:00:12:00:${hex}`,
    macBytes: [0x52, 0x54, 0x00, 0x12, 0x00, n],
    ip: `169.254.0.${n}`,
    uuid: `5a5a5a5a-0000-4000-8000-0000000000${hex}`,
  };
}

// Numbers of the machines that answer within `wait` ms.
export function listMachines(wait = 400) {
  return new Promise((resolve) => {
    const ch = new BroadcastChannel(CTL_CHANNEL);
    const seen = new Set();
    ch.onmessage = (e) => {
      if (e.data && e.data.type === 'here') seen.add(e.data.n);
    };
    ch.postMessage({ type: 'who' });
    setTimeout(() => { ch.close(); resolve(seen); }, wait);
  });
}

// Use `wanted` if it is free, otherwise the lowest free number.
export async function claimMachineNumber(wanted) {
  const used = await listMachines();
  if (wanted && !used.has(wanted)) return wanted;
  for (let n = 1; n <= 254; n++) if (!used.has(n)) return n;
  throw new Error('All 254 machine numbers are in use');
}

// Answer "who" so other pages know this number is taken.  The returned
// function publishes this machine's status line.
export function startPresence(n) {
  const ch = new BroadcastChannel(CTL_CHANNEL);
  let text = '';
  ch.onmessage = (e) => {
    if (e.data && e.data.type === 'who') ch.postMessage({ type: 'here', n, text });
  };
  ch.postMessage({ type: 'here', n, text });
  addEventListener('pagehide', () => ch.postMessage({ type: 'bye', n }));
  return (t) => { text = t; ch.postMessage({ type: 'status', n, text }); };
}

// For the lab page: follow machines appearing, leaving and their status.
export function watchMachines(onChange) {
  const ch = new BroadcastChannel(CTL_CHANNEL);
  const alive = new Map();
  const status = new Map();
  const changed = () => onChange(new Set(alive.keys()), status);
  ch.onmessage = (e) => {
    const d = e.data || {};
    if (d.type === 'here' || d.type === 'status') {
      alive.set(d.n, Date.now());
      if (d.text !== undefined) status.set(d.n, d.text);
      changed();
    } else if (d.type === 'bye' && alive.delete(d.n)) {
      changed();
    }
  };
  const poll = () => {
    const now = Date.now();
    let gone = false;
    for (const [n, t] of alive) if (now - t > 5000) { alive.delete(n); gone = true; }
    if (gone) changed();
    ch.postMessage({ type: 'who' });
  };
  poll();
  setInterval(poll, 2000);
  return ch;
}
