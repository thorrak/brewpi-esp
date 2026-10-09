# Read-only HTTP connection pressure check

Run this explicitly against an idle controller after flashing the HTTP capacity
fix. It sends only `GET /api/health/`, `/api/heap/`, `/api/uptime/`, and a preflight
`/api/water-test/`. It never changes settings, starts/stops a test, or restarts the
controller. Close other device browser tabs for a repeatable first run.

```sh
python3 tests/http_connections/check.py DEVICE_IP > connection-check.json
```

There is no default host. By default the script refuses pressure if a Chill Test
is active, an upload is pending/uploading/error, or the state cannot be verified.
`--skip-water-test-check` is available for firmware without that endpoint after
checking its state separately. `--allow-busy` deliberately overrides a known busy
state; leave it off for routine validation.

One cycle opens eight separate HTTP connections sequentially. Each completes and
consumes a JSON response before remaining idle with keep-alive, then a new,
short-lived connection requests health. An LRU server may close earlier idle
sockets; keeping all eight alive is **not** required. The important observation
is that each new request still gets HTTP 200 and the control loop remains fresh.
A server that declines persistent responses cannot validate this pressure case
and is reported as a failure rather than a pass.

The default health threshold is `control_loop.last_tick_age_ms <= 5000`, with
`control_loop.started` true. `--max-tick-age-ms` changes that threshold. Heap and
uptime snapshots are recorded before and after; no arbitrary heap threshold is
assumed. This is a sampled liveness check, not proof of every control-loop tick.

Every owned connection closes in `finally`, including on failure. Up to three
fresh health probes then check recovery after cleanup, followed by heap/uptime.
Recovery does not turn an admission failure during pressure into a pass.

`--cycles` is limited to 1–3 (default 1), each using eight connections; `--timeout`
is a per-socket inactivity timeout of 0.1–10 seconds (default 3). Responses are
limited to 64 KiB. There are no indefinite retries or concurrent request floods.
Exit status is 0 for pass, 1 for failure, and 2 for a preflight refusal or invalid
arguments. JSON on stdout records requests, timings, observed control-loop ages,
cleanup recovery, and failure details. Keep the report alongside the firmware
revision used for the run.

Local harness validation, without contacting hardware:

```sh
python3 -m unittest discover -s tests/http_connections -p 'test_*.py' -v
```
