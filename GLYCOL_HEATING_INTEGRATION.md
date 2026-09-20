# PR #146 integration into v17_glycol_rebased

This branch ports [Riccardo Tresoldi's PR #146](https://github.com/thorrak/brewpi-esp/pull/146)
onto `v17_glycol_rebased` at `542919c`. The reviewed PR head was
`3f76a757ff55469775c879ece10e120282e1c560`.

The work lives on `codex/glycol-heating-v17`. The base branch is unchanged.
The controller and supporting API/documentation ports are attributed to Riccardo
in two commits; subsequent review fixes are separate. This is a source port,
not a merge of the old Arduino branch or a replay of every intermediate commit.

## What was integrated

- Beer-temperature PID heating with time-proportional heater windows alongside
  predictive glycol cooling.
- Separate `ChamberMode` and `GlycolMode` controllers with shared references in
  `ControlContext`; orchestration and persistence remain in `TempControl`.
- HTTP updates for `KpHeat`, `KiHeat`, `KdHeat`, and `pidMaxHeat`, using the newer
  branch's existing JSON keys and persisted fields.
- Heating state logging and one archived glycol CSV log, with a 30 KB rotation
  threshold per file; consistent removal of the unused coast-observation field.
- The contributor's 300-second learned response-delay limit and algorithm docs.

The port keeps ESP-IDF timing/filesystem APIs, initialized default sensors, the
newer WiFi infrastructure, and the Vue source/build pipeline. The PR's old
compiled UI bundles were omitted. The existing Vue glycol-mode controls remain;
editable heating PID fields are still API-only, as in the PR.

## Review fixes

- Measure the cooling-to-heating delay from the end of cooling and enforce the
  heating-to-cooling delay.
- Preserve pump-off timestamps across interrupted cycles while clearing stale
  PID demand, integration and runtime state on mode-off or sensor loss.
- Latch heater demand once per window so an increase during the off slice cannot
  create a late pulse shorter than the configured minimum. Repeated settings
  that reassert the same mode preserve the current window. Safety shutoffs still
  cancel heat immediately.
- Exclude heater-influenced samples from passive drift learning, and use unsigned
  elapsed time in the rate regression to handle millisecond counter rollover.
- Honor the configured heater output: when the light output is the glycol heater,
  door and camera-light overrides cannot energize it independently.
- Validate new HTTP heating constants before conversion and avoid truncating the
  active log if archive rotation fails. Use the current logging API so the
  optional CSV-logging configuration compiles.

## Validation and limits

Validation consists of source review and firmware compilation. No regression
files or automated tests were added or run, as requested. No controller was
flashed and no hardware behavior was observed during this integration.

Builds completed successfully:

| Configuration | Result |
|---|---|
| `esp32_wifi_tft` | Passed |
| `esp32_wifi_iic` | Passed |
| `esp32_s2_wifi` | Passed |
| `esp32_wifi_espi` | Passed |
| IIC with `ENABLE_GLYCOL_LOGGING` | Passed |

The logging configuration was compiled with a temporary PlatformIO configuration
extending `env:esp32_wifi_iic` and appending `-DENABLE_GLYCOL_LOGGING` to its build
flags. It is not an added project environment. `git diff --check` also passed.
Existing unused-symbol warnings in `EspDS18B20.cpp` and `ESP_BP_WiFi.cpp` remain.
The old PR's ESP8266 build is not part of this ESP-IDF target.

Hardware validation remains necessary before merging or deploying, especially
heater duty behavior, real relay/smart-plug switching, sensor interruptions,
cooling adaptation, and traditional chamber operation. Heating gains remain
installation-dependent.
