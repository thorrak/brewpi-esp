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

This checks generated adaptive campaign fixtures for both cooling algorithms,
including exact configuration/tuning snapshots, source identity, and final output
state. Plan v3 gives each controller a full 10,800-second run, independently of
its observation count. Production observation closures retain their ordinal,
reason, rate qualification, and coast duration; interrupted observations do not
count toward the goal. Legacy episode flags are absent from new uploads.
The final controller stops before a separate OFF observation lasting at
least 90 seconds with six unique valid beer readings. The fixture checks the
physical OFF anchor, durable sample timestamps and counts, immutable run finish,
and absence of later controller steps or pump ON events. The `--firmware-only`
mode omits the portal API and analysis worker.

`--arduinojson PATH` can supply another existing copy of the project's ArduinoJson
headers. `CXX` selects the native C++ compiler. Nothing is sent over the network,
and no physical sensors or relays are accessed; Django uses an in-memory database.

The test compiles this checkout's actual `WaterTestCore.h`, `WaterTestProtocol.h::recordToJson`, `common`,
`recordOutput`, `recordPhase`, `recordController`, observation recording/closure, controller snapshot and
recording/probe/output metadata serializers, manifest
construction, and the sample-record builder with the real ArduinoJson library.
Hardware discovery and saved-control metadata are stubbed. The actual
`WaterTestUpload.h::writeBatch` streams each immutable 12-record batch through its
fixed 3,072-byte arena, first measuring and then writing the same body. Every
resulting field is compared with the previous serializer, including JSON numeric
rounding. The portal accepts an old key-order request followed by its streaming
retry under the same batch identifier. This does not exercise ESP-IDF HTTP,
task scheduling, flash durability, or electrical behavior; the backend and
transport suites cover HTTP lifecycle and interrupted writes separately.

Native tests select 32-bit ArduinoJson slot identifiers and 1,024-byte allocation
pools. Host pointers still occupy 64 bits, so the fixed arena must accommodate
slots at least as large as those on the ESP32; there is no heap fallback.

A deterministic synthetic water/bath history runs the actual adaptive campaign state
machine and both production controllers, including one invalid glycol sample during pumping.
Every fresh reading drives control. The synthetic recording follows the firmware's
sparse/edge cadence and dense-record budget, while retaining all qualifying final
observation readings; the fixture must fit the declared 12,500-record limit.
The production cadence constants are extracted from the firmware, while the
fixture's sample selection is simulated; the backend suite exercises the real
acquisition and recording path. The generated JSON
is ingested by the actual portal API without any upload credentials or enablement setting, testing
HTTP without redirects, finish-before-batch order, reversed batch arrival,
acknowledgements through the actual firmware acknowledgement helpers, and unchanged retries. The real analysis worker must generate
a campaign comparison and PNG with the portal virtualenv's pinned Chillsim package.
Do not put an unrelated Chillsim source checkout on `PYTHONPATH`. This validates
integration and data meaning, not simulator accuracy on physical hardware.

Both selected-first algorithm orders are submitted as separate experiments. Each
experiment contains both controller runs. Their complete manifest,
finish, and raw-record payloads must survive unchanged in retained data; each
analysis must preserve the exact controller snapshots in its immutable inputs.
The fixtures also retain a reported pump rating and a measured fermenter flow,
respectively, without changing their entered values or units.
