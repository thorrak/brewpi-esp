# Predictive coast core checks

Run from the repository root:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -pedantic -fno-fast-math -ffp-contract=off -I src src/PredictiveCoastController.cpp tests/predictive_coast_core/core_test.cpp -o /tmp/predictive-coast-core-test
/tmp/predictive-coast-core-test
```

The portable test checks near-target startup exposure, immediate sustained
cooling for large errors, the blind-budget boundary, predictive stopping,
coast and response learning, final-coast temperature semantics, learning
retention, learned-tuning round trips and validation, relay timing, faults,
duplicate and invalid ticks, long uptime, filter windows, buffer capacity,
and configuration validation. Both core suites include the shared
`glycol_cooling_measurements/regressions.h` cases for quantized startup, tick
jitter, measurement gaps, and sensor reconnection.

The rate estimate requires a full startup window and sufficient retained time
coverage; it is zero while those requirements are unmet. This guard intentionally
differs from the original frozen Python reference. The separate
`cooling_observations` suite checks completion and interruption telemetry;
observation counts do not imply physical settling or a successful control run.

The core exposes learned tuning for saving and restoring, but performs no storage
I/O. Firmware integration saves those values to flash and restores them at boot.

The saved-trace Python/C++ parity replay documents the original port; see
`tests/predictive_coast_parity/` for its ABI adapter. Comparisons of the current
controller must use matching firmware sources and their generated controller
implementation identity.
