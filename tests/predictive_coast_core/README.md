# Predictive coast core checks

Run from the repository root:

```sh
c++ -std=c++11 -Wall -Wextra -Werror -pedantic -fno-fast-math -ffp-contract=off -I src src/PredictiveCoastController.cpp tests/predictive_coast_core/core_test.cpp -o /tmp/predictive-coast-core-test
/tmp/predictive-coast-core-test
```

The portable test checks near-target startup exposure, immediate sustained
cooling for large errors, the blind-budget boundary, predictive stopping,
coast and response learning, final-coast temperature semantics, learning
persistence, relay timing, faults, duplicate and invalid ticks, long uptime,
filter windows, buffer capacity, and configuration validation.

The full saved-trace Python/C++ parity replay is separate; see
`tests/predictive_coast_parity/` for its ABI adapter.
