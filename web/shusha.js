// One Shusha machine: boots the kernel in QEMU compiled to WebAssembly,
// draws the guest display on a canvas, sends keyboard and pointer input
// back, and plugs the guest's e1000 network card into a virtual Ethernet
// hub shared by every Shusha machine open in this browser (other tabs,
// windows and the frames of the lab page).  See web/README.md.

import initQemu from './qemu/qemu-system-x86_64.js';
import { LAN_CHANNEL, INVITE_CHANNEL, claimMachineNumber, machineIdentity, startPresence } from './lan.js';

const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d');
const statusEl = document.getElementById('status');
const logEl = document.getElementById('log');
const startBtn = document.getElementById('start');
const titleEl = document.getElementById('title');
const netEl = document.getElementById('net');

const params = new URLSearchParams(location.search);

const EV_KEY = 1, EV_ABS = 2, EV_BTN = 3, EV_WHEEL = 4;

// KeyboardEvent.code -> Linux input keycode (QEMU maps these to its own codes)
const LINUX_KEYS = {
  Escape: 1, Digit1: 2, Digit2: 3, Digit3: 4, Digit4: 5, Digit5: 6, Digit6: 7,
  Digit7: 8, Digit8: 9, Digit9: 10, Digit0: 11, Minus: 12, Equal: 13,
  Backspace: 14, Tab: 15, KeyQ: 16, KeyW: 17, KeyE: 18, KeyR: 19, KeyT: 20,
  KeyY: 21, KeyU: 22, KeyI: 23, KeyO: 24, KeyP: 25, BracketLeft: 26,
  BracketRight: 27, Enter: 28, ControlLeft: 29, KeyA: 30, KeyS: 31, KeyD: 32,
  KeyF: 33, KeyG: 34, KeyH: 35, KeyJ: 36, KeyK: 37, KeyL: 38, Semicolon: 39,
  Quote: 40, Backquote: 41, ShiftLeft: 42, Backslash: 43, KeyZ: 44, KeyX: 45,
  KeyC: 46, KeyV: 47, KeyB: 48, KeyN: 49, KeyM: 50, Comma: 51, Period: 52,
  Slash: 53, ShiftRight: 54, NumpadMultiply: 55, AltLeft: 56, Space: 57,
  CapsLock: 58, F1: 59, F2: 60, F3: 61, F4: 62, F5: 63, F6: 64, F7: 65,
  F8: 66, F9: 67, F10: 68, NumLock: 69, ScrollLock: 70, Numpad7: 71,
  Numpad8: 72, Numpad9: 73, NumpadSubtract: 74, Numpad4: 75, Numpad5: 76,
  Numpad6: 77, NumpadAdd: 78, Numpad1: 79, Numpad2: 80, Numpad3: 81,
  Numpad0: 82, NumpadDecimal: 83, IntlBackslash: 86, F11: 87, F12: 88,
  NumpadEnter: 96, ControlRight: 97, NumpadDivide: 98, PrintScreen: 99,
  AltRight: 100, Home: 102, ArrowUp: 103, PageUp: 104, ArrowLeft: 105,
  ArrowRight: 106, End: 107, ArrowDown: 108, PageDown: 109, Insert: 110,
  Delete: 111, Pause: 119, MetaLeft: 125, MetaRight: 126, ContextMenu: 127,
};

let Module = null;
let ring = null;       // Int32Array view of the input event ring
let shared = null;     // Int32Array view of [pending, head, tail]
let ringSize = 0;
let image = null;
let booted = false;
let machine = null;    // { n, mac, ip, uuid }
let publishStatus = null;

function setStatus(text) {
  statusEl.textContent = text;
  if (publishStatus) publishStatus(text);
}

function log(line) {
  if (!booted) {
    const stage = /^\[stage\] \[S\d+ ([^\]]+)\]/.exec(line);
    if (stage) setStatus(`Booting: ${stage[1]}…`);
    if (stage && stage[1] === 'shell') {
      booted = true;
      setStatus(matchMedia('(pointer: coarse)').matches
        ? 'Running. Tap Keyboard to type.'
        : 'Running. Click the screen to type.');
    }
  }
  const invite = /workspace: invite (shu[a-z2-7]{40,})/.exec(line);
  if (invite) showInvite(invite[1]);
  logEl.textContent += line + '\n';
  if (logEl.textContent.length > 200000) {
    logEl.textContent = logEl.textContent.slice(-100000);
  }
  logEl.scrollTop = logEl.scrollHeight;
}

// Invite codes the Workspace app creates are mirrored to the serial console;
// show them so they can be copied into another machine.
const inviteBox = document.getElementById('invite');
function showInvite(code) {
  inviteBox.hidden = false;
  inviteBox.querySelector('code').textContent = code;
  const ch = new BroadcastChannel(INVITE_CHANNEL);
  ch.postMessage({ from: machine.n, code });
  ch.close();
}

function push(type, a, b) {
  if (!ring) return;
  const head = Atomics.load(shared, 1);
  const tail = Atomics.load(shared, 2);
  if (head - tail >= ringSize) return; // full: drop the event
  const i = (head % ringSize) * 4;
  ring[i] = type;
  ring[i + 1] = a;
  ring[i + 2] = b;
  Atomics.store(shared, 1, head + 1);
}

function pointerPos(e) {
  const r = canvas.getBoundingClientRect();
  const x = Math.round((e.clientX - r.left) * canvas.width / r.width);
  const y = Math.round((e.clientY - r.top) * canvas.height / r.height);
  return [Math.max(0, Math.min(canvas.width - 1, x)),
          Math.max(0, Math.min(canvas.height - 1, y))];
}

// Characters typed on a phone's on-screen keyboard -> [Linux keycode, shift]
const CHAR_KEYS = (() => {
  const map = { ' ': [57, 0], '\n': [28, 0], '\t': [15, 0] };
  const rows = [
    ['1234567890-=', '!@#$%^&*()_+', 2],
    ['qwertyuiop[]', 'QWERTYUIOP{}', 16],
    ["asdfghjkl;'`", 'ASDFGHJKL:"~', 30],
    ['\\zxcvbnm,./', '|ZXCVBNM<>?', 43],
  ];
  for (const [plain, shifted, first] of rows) {
    [...plain].forEach((c, i) => { map[c] = [first + i, 0]; });
    [...shifted].forEach((c, i) => { map[c] = [first + i, 1]; });
  }
  return map;
})();

// Bursts of key presses (pasted text, an autocorrected word) are fed to
// QEMU one event every 30 ms, because the guest drops or reorders keys when
// several land in one display tick.
const keyQueue = [];
let keyTimer = 0;

function queueKey(code, down) {
  keyQueue.push([code, down]);
  if (!keyTimer) {
    keyTimer = setInterval(() => {
      if (keyQueue.length) push(EV_KEY, ...keyQueue.shift());
      if (!keyQueue.length) { clearInterval(keyTimer); keyTimer = 0; }
    }, 30);
  }
}

function tapKey(code, shift) {
  if (shift) queueKey(42, 1);
  queueKey(code, 1);
  queueKey(code, 0);
  if (shift) queueKey(42, 0);
}

function typeText(text) {
  for (const c of text.normalize('NFKD')) {
    const k = CHAR_KEYS[c];
    if (k) tapKey(k[0], k[1]);
  }
}

function releaseModifiers() {
  for (const k of ['ShiftLeft', 'ShiftRight', 'ControlLeft', 'ControlRight',
                   'AltLeft', 'AltRight', 'MetaLeft', 'MetaRight']) {
    push(EV_KEY, LINUX_KEYS[k], 0);
  }
}

// Phones only show their keyboard for a focused text field, and many send no
// usable key codes.  On touch devices a hidden textarea takes the focus and
// whatever the keyboard does to its text is replayed as key presses.
function wireSoftKeyboard() {
  const field = document.getElementById('softkbd');
  const button = document.getElementById('kbd');
  const FILL = '    ';
  let last = FILL;
  let wanted = false;

  const reset = () => {
    field.value = FILL;
    last = FILL;
    field.setSelectionRange(FILL.length, FILL.length);
  };
  const open = () => {
    wanted = true;
    reset();
    field.focus({ preventScroll: true });
    button.setAttribute('aria-pressed', 'true');
  };
  const close = () => {
    wanted = false;
    field.blur();
    button.setAttribute('aria-pressed', 'false');
  };

  if (matchMedia('(pointer: coarse)').matches) button.hidden = false;
  button.addEventListener('pointerdown', (e) => e.preventDefault());
  button.addEventListener('click', () => (wanted ? close() : open()));

  field.addEventListener('keydown', (e) => {
    const code = LINUX_KEYS[e.code];
    if (code && !e.isComposing && e.keyCode !== 229) {
      push(EV_KEY, code, 1);
      e.preventDefault();
    }
  });
  field.addEventListener('keyup', (e) => {
    const code = LINUX_KEYS[e.code];
    if (code && !e.isComposing && e.keyCode !== 229) push(EV_KEY, code, 0);
  });
  field.addEventListener('input', (e) => {
    const now = field.value;
    let same = 0;
    while (same < last.length && same < now.length && last[same] === now[same]) same++;
    for (let i = same; i < last.length; i++) tapKey(14, 0); // Backspace
    typeText(now.slice(same));
    last = now;
    if (now.length < 2 || /\n/.test(now) || (now.length > 64 && !e.isComposing)) reset();
  });
  field.addEventListener('compositionend', () => {
    if (last.length > 64) reset();
  });
  field.addEventListener('blur', () => {
    releaseModifiers();
    wanted = false;
    button.setAttribute('aria-pressed', 'false');
  });

  return { active: () => wanted };
}

function wireInput() {
  const softKeyboard = wireSoftKeyboard();
  canvas.addEventListener('keydown', (e) => {
    const code = LINUX_KEYS[e.code];
    if (code) { push(EV_KEY, code, 1); e.preventDefault(); }
  });
  canvas.addEventListener('keyup', (e) => {
    const code = LINUX_KEYS[e.code];
    if (code) { push(EV_KEY, code, 0); e.preventDefault(); }
  });
  // Paste text into the guest (handy for invite codes between machines).
  canvas.addEventListener('paste', (e) => {
    typeText(e.clipboardData.getData('text'));
    e.preventDefault();
  });
  canvas.addEventListener('pointermove', (e) => push(EV_ABS, ...pointerPos(e)));
  canvas.addEventListener('pointerdown', (e) => {
    if (!softKeyboard.active()) canvas.focus();
    push(EV_ABS, ...pointerPos(e));
    if (e.button <= 2) push(EV_BTN, e.button, 1);
    e.preventDefault();
  });
  canvas.addEventListener('pointerup', (e) => {
    if (e.button <= 2) push(EV_BTN, e.button, 0);
    e.preventDefault();
  });
  canvas.addEventListener('wheel', (e) => {
    push(EV_WHEEL, e.deltaY < 0 ? 1 : -1, 0);
    e.preventDefault();
  }, { passive: false });
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  canvas.addEventListener('blur', releaseModifiers);
  document.getElementById('copy-invite').addEventListener('click', () => {
    navigator.clipboard.writeText(inviteBox.querySelector('code').textContent).catch(() => {});
  });
  // Another machine's invite, offered by the "Type invite" button.
  const inv = new BroadcastChannel(INVITE_CHANNEL);
  const typeInvite = document.getElementById('type-invite');
  let lastForeign = '';
  inv.onmessage = (e) => {
    if (!e.data || e.data.from === machine.n) return;
    lastForeign = e.data.code;
    typeInvite.hidden = false;
    typeInvite.textContent = `Type machine ${e.data.from}'s invite`;
  };
  typeInvite.addEventListener('click', () => {
    if (lastForeign) typeText(lastForeign);
    canvas.focus();
  });
  // The lab page forwards text to type (its "Type text" box).
  addEventListener('message', (e) => {
    if (e.origin !== location.origin || !e.data || e.data.shusha !== 'type') return;
    typeText(String(e.data.text));
  });
}

// ---- network ----------------------------------------------------------------
// The patched QEMU exposes two rings of fixed-size slots (net/browser.c):
// frames the guest sends (TX) and frames for the guest (RX).  Every frame
// sent is broadcast on a BroadcastChannel, which reaches every other Shusha
// machine of this site in this browser: together they form one Ethernet hub.

const net = { shared: null, tx: null, rx: null, slots: 0, slotSize: 0, sent: 0, recv: 0, dropped: 0 };
let lan = null;

function updateNetStatus() {
  if (netEl) netEl.textContent = `LAN: ${net.sent} sent · ${net.recv} received`;
}

function drainTx() {
  if (!net.shared) return;
  let tail = Atomics.load(net.shared, 1);
  const head = Atomics.load(net.shared, 0);
  while (tail !== head) {
    const off = (tail % net.slots) * net.slotSize;
    const len = new DataView(net.tx.buffer, net.tx.byteOffset + off, 4).getInt32(0, true);
    if (len > 0 && len <= net.slotSize - 4) {
      const frame = net.tx.slice(off + 4, off + 4 + len); // copies out of shared memory
      lan.postMessage({ from: machine.n, frame });
      net.sent++;
    }
    tail++;
  }
  Atomics.store(net.shared, 1, tail);
  updateNetStatus();
}

function deliver(frame) {
  // Like a switch port: only frames for our MAC, or broadcast/multicast.
  const multicast = frame[0] & 1;
  if (!multicast) {
    for (let i = 0; i < 6; i++) if (frame[i] !== machine.macBytes[i]) return;
  }
  if (!net.shared || frame.length > net.slotSize - 4) return;
  const head = Atomics.load(net.shared, 2);
  const tail = Atomics.load(net.shared, 3);
  if (head - tail >= net.slots) { net.dropped++; return; }
  const off = (head % net.slots) * net.slotSize;
  new DataView(net.rx.buffer, net.rx.byteOffset + off, 4).setInt32(0, frame.length, true);
  net.rx.set(frame, off + 4);
  Atomics.store(net.shared, 2, head + 1);
  net.recv++;
  updateNetStatus();
}

// ---- boot -------------------------------------------------------------------

async function fetchWithProgress(url, label) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  const total = Number(res.headers.get('content-length')) || 0;
  const reader = res.body.getReader();
  const chunks = [];
  let got = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    chunks.push(value);
    got += value.length;
    const mb = (got / 1048576).toFixed(1);
    setStatus(total ? `Downloading ${label}: ${mb} / ${(total / 1048576).toFixed(1)} MB`
                    : `Downloading ${label}: ${mb} MB`);
  }
  const out = new Uint8Array(got);
  let off = 0;
  for (const c of chunks) { out.set(c, off); off += c.length; }
  return out;
}

async function boot() {
  startBtn.disabled = true;
  if (!self.crossOriginIsolated) {
    setStatus('This page needs cross-origin isolation (SharedArrayBuffer). Reloading…');
    return;
  }
  const gz = await fetchWithProgress('kernel.bin.gz', 'Shusha');
  setStatus('Unpacking the kernel…');
  const kernel = new Uint8Array(await new Response(
    new Blob([gz]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
  const shim = new Uint8Array(await (await fetch('shim/shim.elf')).arrayBuffer());
  const bios = {};
  for (const f of ['bios-256k.bin', 'vgabios-stdvga.bin', 'kvmvapic.bin',
                   'efi-e1000.rom', 'multiboot_dma.bin']) {
    bios[f] = new Uint8Array(await (await fetch('qemu/pc-bios/' + f)).arrayBuffer());
  }
  setStatus('Starting the emulator…');

  lan = new BroadcastChannel(LAN_CHANNEL);
  lan.onmessage = (e) => {
    if (e.data && e.data.from !== machine.n && e.data.frame) deliver(e.data.frame);
  };

  Module = {
    arguments: [
      '-L', '/pc-bios',
      '-m', '256M',
      // -cpu max gives the guest RDRAND/RDSEED for its CSPRNG under TCG
      '-cpu', 'max',
      '-accel', (params.get('accel') || 'tcg,tb-size=256'),
      '-display', 'canvas',
      '-vga', 'std',
      '-serial', (params.get('serial') || 'file:/dev/stdout'),
      '-monitor', 'none',
      // Shusha has no USB stack, so the pointer is the PS/2 mouse; the
      // canvas display turns page coordinates into relative moves.
      '-netdev', 'browser,id=n0',
      '-device', `e1000,netdev=n0,mac=${machine.mac}`,
      // the SMBIOS UUID is one of the IDs Shusha binds its keys to
      '-uuid', machine.uuid,
      '-kernel', '/shusha/shim.elf',
      '-initrd', '/shusha/kernel.bin',
      '-append', `icda.live=1 shusha.uuid=${machine.uuid}`,
      '-no-reboot',
    ],
    mainScriptUrlOrBlob: new URL('./qemu/qemu-system-x86_64.js', import.meta.url).href,
    locateFile: (p) => new URL('./qemu/' + p, import.meta.url).href,
    print: log,
    printErr: log,
    preRun: [(mod) => {
      mod.FS.mkdir('/pc-bios');
      for (const [name, data] of Object.entries(bios)) {
        mod.FS.writeFile('/pc-bios/' + name, data);
      }
      mod.FS.mkdir('/shusha');
      mod.FS.writeFile('/shusha/shim.elf', shim);
      mod.FS.writeFile('/shusha/kernel.bin', kernel);
    }],
    canvasInit(ringPtr, sharedPtr, size) {
      self.__probe = () => [...new Int32Array(Module.HEAP32.buffer, sharedPtr, 4)];
      ring = new Int32Array(Module.HEAP32.buffer, ringPtr, size * 4);
      shared = new Int32Array(Module.HEAP32.buffer, sharedPtr, 4);
      ringSize = size;
      setStatus('Booting Shusha…');
    },
    canvasResize(w, h) {
      self.__resizes = (self.__resizes || 0) + 1;
      canvas.width = w;
      canvas.height = h;
      image = ctx.createImageData(w, h);
    },
    canvasFrame(ptr, w, h, pendingPtr) {
      self.__frames = (self.__frames || 0) + 1;
      if (!image || image.width !== w || image.height !== h) {
        canvas.width = w;
        canvas.height = h;
        image = ctx.createImageData(w, h);
      }
      image.data.set(Module.HEAPU8.subarray(ptr, ptr + w * h * 4));
      Atomics.store(Module.HEAP32, pendingPtr >> 2, 0);
      ctx.putImageData(image, 0, 0);
    },
    netInit(sharedPtr, txPtr, rxPtr, slots, slotSize) {
      const buf = Module.HEAPU8.buffer;
      net.shared = new Int32Array(buf, sharedPtr, 4);
      net.tx = new Uint8Array(buf, txPtr, slots * slotSize);
      net.rx = new Uint8Array(buf, rxPtr, slots * slotSize);
      net.slots = slots;
      net.slotSize = slotSize;
      // Backstop for a notification that raced with a drain.
      setInterval(drainTx, 50);
      updateNetStatus();
    },
    netTx: drainTx,
  };
  wireInput();
  await initQemu(Module);
  canvas.focus();
}

async function main() {
  const asked = Number(params.get('n'));
  const n = await claimMachineNumber(Number.isInteger(asked) && asked >= 1 && asked <= 254 ? asked : 0);
  machine = machineIdentity(n);
  publishStatus = startPresence(n);
  document.title = `Shusha ${n}`;
  titleEl.textContent = `Shusha machine ${n}`;
  document.getElementById('addr').textContent = `${machine.ip} · MAC ${machine.mac}`;

  startBtn.addEventListener('click', () => {
    boot().catch((err) => {
      setStatus('Failed: ' + err.message);
      console.error(err);
    });
  });
  if (params.has('autostart')) startBtn.click();
}

main();
