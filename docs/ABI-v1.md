
## Shusha clarifications (post-freeze, no renumber)

IPC access control, added without new call numbers:

- `SYS_SHM_CREATE` returns an unguessable capability handle,
  `(48-bit random tag << 16) | slot`, instead of a bare slot number.
  `SYS_SHM_MAP/UNMAP/CLOSE` refuse handles whose tag does not match, so a
  process can only map a region whose handle it was given.
- `SYS_MSG_RECV` (`rdi` handle, `rsi` buffer, `rdx` flags): bit 0 is the
  old "block" argument; bit 1 makes a successful call return the
  sender's pid (stamped by the kernel at send time) instead of 0. Upper
  bits are ignored. Only the queue's owner (its creator) may receive.
- `SYS_MSG_POLL` (`rdi` handle, `rsi` flags): with bit 0 set it returns
  the owning pid of the queue (anyone may ask). Without it, only the
  owner may poll. Callers must pass `rsi = 0` for a plain poll.
- `SYS_MSG_OPEN`: names under `/proc/<pid>/` can only be created by
  process `<pid>`; anyone may open an existing queue to send to it.

File ownership (Shusha, no new numbers):

- `SYS_STAT`: `vfs_stat_t` gained `uint32_t uid` and `uint16_t mode`
  appended after `readonly` (append-only; callers must pass the larger
  struct).
- File syscalls check Unix-style owner/other `rwx` bits against the
  caller's uid (root, uid 0, may do anything except write the read-only
  image): read/read_at need `r`, write/create/mkdir need `w` on the file
  or its parent directory, list_dir needs `r`, chdir needs `x`.
- Control files, used through `SYS_VFS_WRITE`/`SYS_VFS_READ`:
  `/dev/setuid` (write "<uid>"; root may pick any uid, others only their
  own; reading returns the caller's uid), `/dev/chmod` ("<octal> <path>",
  owner or root), `/dev/chown` ("<uid> <path>", root only).

Userspace networking (Shusha, no new numbers):

- `/dev/udp` (see `kernel/net/udp.h`): `SYS_VFS_WRITE` with a binary
  command - op 1 bind a port, op 2 unbind, op 3 send a datagram
  (`[op][pad][src port u16][dst port u16][pad u16][dst ip 4][payload]`,
  ports little-endian, payload <= 1400). `SYS_VFS_READ_AT` at offset 0
  returns the next datagram for any port the caller bound, prefixed by
  `[src ip 4][src port u16][dst port u16][len u16][pad u16]`, or 0.
  Ports are owned by the binding process and freed when it exits.
- `/dev/netinfo` reads as text: `ip`, `mask`, `mac`.
- Without a DHCP server the kernel takes a link-local address,
  169.254.<mac[4]>.<mac[5]>/16.
