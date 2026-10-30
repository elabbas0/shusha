
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
