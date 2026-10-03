# Durable metadata storage regression

Run `python3 tests/water_test_storage/run.py` after installing ArduinoJson through
one firmware build. The test compiles the production storage implementation with
native file operations and a controllable durable-write failure.

The saved fixture contains 9.6 KiB of JSON, beyond the string-growth allocation
that aborted on the ESP32. During save and recovery, any C++ allocation above
1 KiB throws; saving and reading require no C++ allocations. The retained
ArduinoJson document and test capture buffer are allocated separately. Streaming
uploads validate the envelope and decoded length, rewind the same file, and
deliver chunks of at most 256 bytes without a body allocation.
This test targets temporary payload copies, not the memory needed to keep a
parsed manifest in memory.

It also checks compatibility with the previous envelope writer, exact raw upload
bytes, UTF-8 and Unicode escapes, control characters, CRC failures, truncation,
oversized metadata, zero sink calls for corrupt files or mismatched expected
lengths, immediate sink-failure cancellation, and a failed replacement leaving
the previous durable document intact. All files are temporary and no hardware is
accessed.
