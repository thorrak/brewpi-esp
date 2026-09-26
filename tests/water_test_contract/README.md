# Firmware-to-portal contract check

Run after installing the board's ArduinoJson dependency through PlatformIO, using
the portal's pinned Python environment:

```sh
/path/to/portal/.venv/bin/python tests/water_test_contract/run.py \
  --portal /path/to/glycol_data_collection
```

To compile and exercise the firmware serializers without a compatible portal
checkout and Python environment, run:

```sh
python3 tests/water_test_contract/run.py --firmware-only
```

This checks the generated three-pulse fixture and its final output state. It does
not exercise the portal API or analysis worker.

`--arduinojson PATH` can supply another existing copy of the project's ArduinoJson
headers. `CXX` selects the native C++ compiler. Nothing is sent over the network,
and no physical sensors or relays are accessed; Django uses an in-memory database.

The test compiles this checkout's actual `WaterTestCore.h`, `WaterTestProtocol.h::recordToJson`, `common`,
`recordOutput`, `recordPhase`, probe/output metadata serializers, manifest
construction, and the sample-record builder with the real ArduinoJson library.
Hardware discovery and saved-control metadata are stubbed. Finish bounds and
12-record batch framing reproduce the uploader structure; this does not exercise
ESP-IDF HTTP, task scheduling, flash durability, or electrical behavior.

A deterministic synthetic water/bath history runs the actual three-pulse state
machine, including one invalid glycol sample during pumping. The generated JSON
is ingested by the actual portal API without any upload credentials or enablement setting, testing
HTTP without redirects, finish-before-batch order, reversed batch arrival,
acknowledgements through the actual firmware acknowledgement helpers, and unchanged retries. The real analysis worker must generate
a replay-v2 comparison and PNG with the unchanged Chillsim package. This validates
integration and data meaning, not simulator accuracy on physical hardware.
