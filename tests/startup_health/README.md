# Startup and sensor health regressions

Run `python3 tests/startup_health/run.py` after a firmware build has installed
ArduinoJson. This runs production startup, scanner, and health-serialization code
against simulated ESP/RTOS boundaries without contacting hardware.

The suite injects control-loop task, scanner mutex, sensor bus, and sensor-worker
allocation failures. It checks the existing-stack control-loop fallback, bounded
startup retries, successful reads after recovery, and read-only diagnostics.
Source guards also check that sensor startup gets allocation priority over uploads.

These checks do not emulate ESP32 scheduling, physical OneWire wiring, or real
heap fragmentation. Hardware diagnostics are needed to identify a device's
actual failure.
