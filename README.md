# shusha
A security-first, multi-user operating system built in C

Built on the [ICDA](https://github.com/elabbas0/ICDA) kernel and drivers
(see [NOTICE](NOTICE)). Userspace (init, shell, window manager, apps) is
Shusha's own.

## Build and run

Needs `gcc`, `nasm`, `grub-mkrescue` (+ `xorriso`, `mtools`) and QEMU.

```sh
make              # builds shusha.iso
make qemu         # boot it in a window
make qemu-smoke   # headless boot test
make test         # crypto test vectors on the host
```

## Docs

- [Plan and roadmap](docs/PLAN.md)
- [Threat model](docs/THREAT_MODEL.md)
- [Workspace design](docs/WORKSPACE.md)
- [Encryption and decryption](docs/CRYPTO.md)
