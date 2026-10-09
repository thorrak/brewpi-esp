# Native regression checks

After building a firmware target to install ArduinoJson, run:

```sh
python3 tests/run_native.py
```

The suite covers the portable controllers and observation telemetry, selection
and tuning persistence, Chill Test sequencing and recovery, bounded storage and
uploading, memory allocation failures, startup health, HTTP/TCP lifecycles and
the firmware serializer contract. Shared measurement regressions run through
both controller core suites.

Tests use temporary files, simulated I/O and loopback servers; they do not
contact a controller or external service. `CXX` selects the compiler for the
portable core checks and for individual runners that support it.

The individual test directories document focused checks. Full receiver and
Chillsim comparisons need their respective repositories; see
`water_test_contract/README.md` and the controller parity directories. UI tests
run separately from `ui/` using `npx jest --runInBand`.

After flashing network changes, run the explicit read-only hardware check in
[`http_connections/README.md`](http_connections/README.md). It exercises new
request admission while older browser-style connections remain idle, verifies
control-loop liveness, and closes its own connections even on failure. It refuses
active water tests and pending uploads by default.
