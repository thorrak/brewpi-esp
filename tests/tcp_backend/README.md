# TCP backend lifecycle regressions

Run `python3 tests/tcp_backend/run.py` with a host C++17 compiler. The runner
compiles the production `PiStreamBackend` and `TcpBackend` classes verbatim,
without their unrelated embedded dependencies.

Real socket pairs exercise ordinary traffic, queued bytes followed by peer EOF,
and EOF detected by the command loop's `available()` poll. Narrow syscall hooks
cover transient and fatal receive/write/ioctl errors, positive partial writes,
data arriving between availability checks, and an externally replaced descriptor
surviving an old operation's failure. These tests do not contact a device.

Pass `--source /path/to/PiStream.h` to check an earlier header against the same
regressions.
