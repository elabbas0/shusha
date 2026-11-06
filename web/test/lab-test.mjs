// End-to-end check of the browser lab in headless Chromium: two Shusha
// machines opened from the lab page (each in its own window) create accounts, form a workspace, share an
// encrypted file, edit it from the other machine, and the lab's wire search
// confirms the file's text never crossed the virtual network in the clear.
//
//   cd web && python3 -m http.server 8000 &
//   node test/lab-test.mjs http://localhost:8000/ out-dir
//
// Coordinates are for the 1280x800 desktop (as in scripts/ws-gui-demo.py).

import { createRequire } from 'node:module';
const { chromium } = createRequire(import.meta.url)(process.env.PLAYWRIGHT || 'playwright');
import fs from 'node:fs';

const BASE = process.argv[2] || 'http://localhost:8000/';
const OUT = process.argv[3] || 'lab-shots';
fs.mkdirSync(OUT, { recursive: true });

const DOCK = { workspace: [670, 754], qalam: [730, 754] };
const origin = (n, h) => { const x = 24 + 36 * n; let y = 85 + 30 * n; if (y + h > 710) y = 75; return [x, y]; };
const inApp = (o, xy) => [o[0] + xy[0], o[1] + xy[1]];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const t0 = Date.now();
const say = (s) => console.log(`[${((Date.now() - t0) / 1000).toFixed(0)}s] ${s}`);

const browser = await chromium.launch({ args: ['--enable-features=SharedArrayBuffer'] });
const ctx = await browser.newContext({ viewport: { width: 1400, height: 1000 } });
const page = await ctx.newPage();
ctx.on('page', (p) => {
  p.on('console', (msg) => { if (msg.type() === 'error') console.log('console:', msg.text()); });
  p.on('pageerror', (e) => console.log('pageerror:', e.message));
});

await page.goto(BASE + 'index.html');
await page.waitForFunction(() => self.crossOriginIsolated, null, { timeout: 30000 });
say('lab page is cross-origin isolated');

// Open two machines with the lab's button; each comes up in its own window.
const m = {};
for (const k of [1, 2]) {
  const next = ctx.waitForEvent('page');
  await page.click('#add');
  m[k] = await next;
  await m[k].setViewportSize({ width: 1340, height: 960 });
  await m[k].waitForURL(new RegExp(`machine\\.html\\?n=${k}&`));
  await sleep(1500);
}
const log = (k) => m[k].evaluate(() => document.getElementById('log').textContent);

async function waitLog(k, text, from = 0, timeout = +(process.env.WAIT || 300000)) {
  const end = Date.now() + timeout;
  for (;;) {
    const l = await log(k);
    const i = l.indexOf(text, from);
    if (i >= 0) return l;
    if (Date.now() > end) {
      await shot('fail');
      for (const j of [1, 2]) console.log(`machine ${j}: frames=${await m[j].evaluate(() => self.__frames)} rings=${await m[j].evaluate(() => self.__probe && self.__probe())}`);
      fs.writeFileSync(`${OUT}/fail-log-1.txt`, await log(1));
      fs.writeFileSync(`${OUT}/fail-log-2.txt`, await log(2));
      throw new Error(`machine ${k}: timed out waiting for "${text}"`);
    }
    await sleep(500);
  }
}
const mark = async (k) => (await log(k)).length;

async function box(k) {
  return m[k].locator('canvas').boundingBox();
}
async function click(k, [x, y], wait = 800) {
  const b = await box(k);
  const mouse = m[k].mouse;
  await mouse.move(b.x + x * b.width / 1280, b.y + y * b.height / 800);
  await sleep(80);
  await mouse.down();
  await sleep(80);
  await mouse.up();
  await sleep(wait);
}
// Types through the page's own key queue (handles Shift for symbols).
async function typ(k, text, wait = 300) {
  await m[k].evaluate((t) => dispatchEvent(new MessageEvent('message',
    { data: { shusha: 'type', text: t }, origin: location.origin })), text);
  await sleep(60 * text.length + wait); // a key down or up every 30 ms
}
async function key(k, combo, n = 1, wait = 200) {
  await m[k].locator('canvas').focus();
  for (let i = 0; i < n; i++) { await m[k].keyboard.press(combo); await sleep(wait); }
}
async function shot(name) {
  for (const k of [1, 2]) await m[k].screenshot({ path: `${OUT}/${name}-machine${k}.png` });
  await page.screenshot({ path: `${OUT}/${name}-lab.png`, fullPage: true });
  say('screenshot ' + name);
}

const booted = Date.now();
await waitLog(1, '[S22 shell]');
await waitLog(2, '[S22 shell]');
say(`both machines booted in ${((Date.now() - booted) / 1000).toFixed(0)} s`);
await sleep(8000);
await shot('01-two-machines-booted');

async function firstBoot(k, name, pw) {
  await click(k, [640, 790], 2000); // focus the screen (empty bottom strip)
  let from = await mark(k);
  await typ(k, `${name}\t${pw}\t${pw}\n`);
  await waitLog(k, 'authd: created account ' + name, from);
  await sleep(2000);
  await typ(k, '\n', 1500);
  from = await mark(k);
  await typ(k, pw + '\n');
  await waitLog(k, 'wm: session started for ' + name, from);
  await sleep(5000);
  await click(k, [762, 68], 1500); // close the Explorer window of the new session
  say(`machine ${k}: signed in as ${name}`);
}
await firstBoot(1, 'admin', 'password1');
await firstBoot(2, 'alice', 'alicepass1');
await shot('02-signed-in');

// machine 1 creates the workspace and an invite
await click(1, DOCK.workspace, 4000);
let from = await mark(1);
await typ(1, 'shusha-team\n');
await waitLog(1, 'workspaced: Created workspace', from);
say('machine 1: created workspace');
from = await mark(1);
const w1 = origin(1, 580); // second window of the session (Explorer is the first)
await click(1, inApp(w1, [690, 36]), 2000);
const l1 = await waitLog(1, 'workspace: invite shu', from);
const code = /invite (shu[a-z2-7]{40,})/.exec(l1.slice(from))[1];
say('machine 1: invite ' + code);
await sleep(500);
await shot('03-invite-created');

// machine 2 joins with the "Type machine 1's invite" button
await click(2, DOCK.workspace, 4000);
const wo = origin(1, 580);
await click(2, inApp(wo, [700, 239]), 400);
await m[2].locator('#type-invite').click();
await sleep(60 * code.length + 800);
await click(2, inApp(wo, [700, 279]), 400);
await typ(2, 'alice');
from = await mark(2);
await click(2, inApp(wo, [609, 323]), 500);
await waitLog(2, 'workspaced: Joined workspace', from, 180000);
say('machine 2: joined the workspace over the virtual LAN');
await sleep(6000);
await shot('04-joined');

// machine 2 writes a file in Qələm and saves it into the workspace
const SECRET = 'Shusha Workspace secret plan 4711';
await click(2, DOCK.qalam, 5000);
await key(2, 'Control+n', 1, 800);
await typ(2, SECRET + '\n', 800);
await key(2, 'F1', 1, 800);
await typ(2, 'encrypted\n', 800);
await key(2, 'Backspace', 12, 60);
from = await mark(1);
const from2 = await mark(2);
await typ(2, 'plan.txt\n', 500);
await waitLog(2, 'workspaced: You shared plan.txt', from2, 180000);
say('machine 2: shared plan.txt');
await waitLog(1, 'alice shared plan.txt', from, 180000);
say('machine 1: received plan.txt from alice');
await sleep(3000);
await shot('05-shared-file');

// machine 1 (the admin, a higher rank) edits it; machine 2 sees the edit
await click(1, DOCK.qalam, 5000);
await key(1, 'Control+p', 1, 1000);
await typ(1, 'plan\n', 2500);
await key(1, 'Control+End', 1, 300);
await typ(1, 'Reviewed by the admin\n', 800);
from = await mark(2);
await key(1, 'Control+s', 1, 1500);
await waitLog(2, 'admin edited plan.txt', from, 180000);
say('machine 2: saw the admin edit plan.txt');
await sleep(3000);
await shot('06-admin-edit-arrived');

// the wire: was any of it in the clear?
const search = async (text) => {
  await page.fill('#search', text);
  await page.click('#search-form button');
  await sleep(300);
  return page.textContent('#search-result');
};
for (const t of [SECRET, 'secret plan', 'Reviewed by the admin', 'plan.txt', 'shusha-team', 'alice']) {
  say(`wire search "${t}": ${await search(t)}`);
}
say('wire stats: ' + await page.textContent('#wire-stats'));
await page.locator('.wire').scrollIntoViewIfNeeded();
await search('secret plan');
await shot('07-wire');
await browser.close();
say('PASS');
