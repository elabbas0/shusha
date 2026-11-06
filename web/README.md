# Shusha in the browser

`web/` is a static site that runs Shusha machines inside the browser and
links them on one virtual Ethernet segment, so the Workspace (secure
channels, invites, ranks, shared files, co-editing in Qələm) works between
them. Each machine is an x86-64 PC emulated by
[qemu-wasm](https://github.com/ktock/qemu-wasm) (QEMU 8.2 compiled to
WebAssembly). Nothing leaves the browser.

- `index.html`: the **network lab**. It opens machines (each in its own
  window), lists the machines on the network, relays invite codes, and
  captures every frame on the segment. Its "Is this text on the wire?"
  search checks whether a string ever crossed the network in the clear.
- `machine.html?n=K`: one machine. Machine K gets MAC `52:54:00:12:00:K`,
  so Shusha gives it 169.254.0.K, the same scheme as `scripts/lan.py`. It
  also gets its own SMBIOS UUID (one of the IDs Shusha binds keys to).
  Without `n`, the page takes the lowest free number.

In headless Chromium a machine reaches the sign-in screen about 10 seconds
after the page loads. `test/lab-test.mjs` drives two machines through
accounts, a workspace invite and join, a shared encrypted file, an edit by
the admin arriving on the other machine, and the wire search.

## What is in here

| File | What it is |
| --- | --- |
| `index.html`, `lab.js` | The lab page. |
| `machine.html`, `shusha.js` | One machine: downloads the kernel, starts QEMU, draws frames to a canvas, sends keyboard and pointer events back, and bridges the network card. |
| `lan.js` | Channel names, machine numbering and identities, presence. |
| `style.css` | Shared styles (light and dark). |
| `coi-serviceworker.min.js` | Adds the COOP/COEP headers that SharedArrayBuffer needs, for hosts such as GitHub Pages that cannot set headers. MIT, from [coi-serviceworker](https://github.com/gzuidhof/coi-serviceworker). |
| `qemu-wasm-shusha.patch` | Changes to qemu-wasm: the `-display canvas` backend (`ui/canvas.c`), the `-netdev browser` network backend (`net/browser.c`), and a fix for glib comparators that trap in WebAssembly. |
| `build-qemu.sh` | Builds QEMU with the patch in Docker and writes `web/qemu/`. |
| `shim/` | A small Multiboot 1 loader (from ICDA's web demo). QEMU starts it with `-kernel shim.elf -initrd kernel.bin -append ...`; it sets the 1280x800x24 video mode, copies the kernel's ELF segments into place, builds the Multiboot 2 tags (command line from `-append`, memory map, framebuffer, ACPI RSDP) and jumps to it. |
| `test/lab-test.mjs` | The end-to-end test (Playwright). |

`kernel.bin.gz`, `shim/shim.elf` and `qemu/` are build outputs and are not committed.

## Building and running locally

```sh
make web-kernel         # web/kernel.bin.gz (console mirrored to serial), web/shim/shim.elf
sh web/build-qemu.sh    # web/qemu/ (Docker; slow the first time)
cd web && python3 -m http.server 8000
```

Open <http://localhost:8000/>. The service worker reloads the page once to
turn on cross-origin isolation. To run the test, with the server up:

```sh
node web/test/lab-test.mjs http://localhost:8000/ build/lab-shots
```

## The emulated machine

`-m 256M -cpu max -vga std -netdev browser,id=n0 -device e1000,netdev=n0,mac=...
-uuid ...`. `-cpu max` gives the guest RDRAND/RDSEED for its CSPRNG.
Shusha has no USB stack, so the pointer is the PS/2 mouse: the canvas
display walks the guest pointer towards the browser pointer with relative
moves (after pinning it to the top-left corner once), and holds a click
until the pointer has arrived. The kernel is built with the console mirrored
to serial, which is how the page picks up invite codes from the Workspace
app.

Live session only: there is no disk, so reloading a machine wipes it.

## How the network works

`net/browser.c` keeps two rings of 2 KB slots in shared memory. Frames the
guest sends go in the TX ring and the page is told to drain it; the page
posts each frame on a `BroadcastChannel`, which reaches every Shusha page of
this site in this browser. Each machine keeps frames for its own MAC plus
broadcast and multicast (like a switch port) and writes them into its RX
ring, which a 2 ms timer on the QEMU main loop feeds to the e1000. The lab
page listens on the same channel, so it sees every frame, like a capture on
a hub port.

Machines in other browsers or on other computers are not connected; that
would need a relay server.

## Why each machine gets its own window

Two of these emulators in one page (for example in iframes) share the
page's main thread, and under load (both machines running Argon2id at
sign-in) one of them would hang about one time in three. Opened as separate
windows without an opener, each machine gets its own renderer, and the hang
did not happen in any test run.
