# Predictive controller parity verification

`adapter.cpp` exposes the portable `PredictiveCooling::Controller` through a small
C ABI. It contains no controller decisions. The independent Python harness lives
in the Chillsim repository and compiles this branch's actual C++ source.

From a Chillsim checkout with its Python dependencies installed, run:

```sh
BREWPI_PREDICTIVE_SOURCE=/path/to/this/brewpi-checkout \
    .venv/bin/pytest -q tests/test_predictive_native.py

.venv/bin/python examples/verify_predictive_cpp_port.py \
    --source /path/to/this/brewpi-checkout \
    --traces output/analysis/cooling-alternatives-20260921-v1 \
    --output-dir output/analysis/predictive-cpp-port-20260922
```

The replay requires all 21 saved predictive benchmark scenarios. It runs each
43,200-second trace twice: once with the original Celsius double setpoints, then
with the legacy BrewPi Fahrenheit-to-Q9 setpoint encoding applied to both
implementations. The saved DS18B20 samples are already exact multiples of
1/16 degree Celsius and can be represented exactly in Q9.

Across 1,814,400 ticks it compares pump commands, phases, learning counters,
smoothed temperature, regression rate, predicted endpoint, coast horizon, and
budget gain. It also checks the runtime pulse budget, completed ON duration,
and continuous-demand flag. The first pass checks the frozen Python results
against the original saved commands and diagnostics; the second measures
language parity under firmware input quantization, not closed-loop performance
with changed setpoints. Numeric parity tolerance is 2e-11; state and command
agreement is exact. No physical parameters or controller parameters are fitted
or tuned by this audit.

The output report includes per-scenario mismatch counts, relay edges, diagnostic
error bounds, exact controller configurations, and source/compiler/library
SHA-256 provenance. It also saves copies of the sources used for the comparison.

Host regression tests cover fractional and large uptimes, hourly regression
rebasing, missed samples, sensor faults, setpoint changes, coast and budget
learning, memory limits, relay timing, and threshold boundaries. They separately
exercise intentional embedded-boundary differences: Python raises on repeated
or invalid times, while the firmware caches valid duplicate ticks and fails OFF
on invalid clocks or readings. The fixed-capacity embedded rings also fail OFF
if configured or sampled beyond their supported capacity. Runtime inhibition
preserves learned estimates and the real OFF edge; a full reset clears learning.

The test adapter is outside `src/` and is not part of the firmware image.
