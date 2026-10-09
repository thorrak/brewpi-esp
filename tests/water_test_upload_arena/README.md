# Fixed upload serializer workspace regression

Run `python3 tests/water_test_upload_arena/run.py` after one firmware build installs
ArduinoJson. The real batch serializer receives the device's 3072-byte workspace.
It is checked with both 1024-byte variant pools (matching ESP32 allocation size)
and conservative 2048-byte native pools.

For each of 60,672 combinations of record kinds, documented codes, roles and flag
bytes, a twelve-record batch includes numeric extremes and appropriate controller
targets. Counting and streaming must produce exactly the same length, regardless
of batch size. Canary bytes verify that the arena stays within its bounds.
Additional checks cover misaligned buffers, insufficient workspace, sink failures,
invalid checksums, noncontiguous sequences, wrong boots and excessive record counts.
No complete batch JSON body is constructed by this test.
