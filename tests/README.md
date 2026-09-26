# Native regression checks

After building a firmware target to install ArduinoJson, run:

```sh
python3 tests/run_native.py
```

This compiles and runs the portable cooling controllers, algorithm selection,
settings persistence, firmware integration, water-test program and protocol,
storage/recovery backend, static HTTP serving, and firmware serializer harness.
It uses temporary files and simulated I/O; it does not contact a server or
operate hardware. `CXX` can select another native C++ compiler.

The individual test directories document focused checks. Full receiver and
Chillsim comparisons need their respective repositories; see
`water_test_contract/README.md` and the controller parity directories. UI tests
run separately from `ui/` using `npx jest --runInBand`.
