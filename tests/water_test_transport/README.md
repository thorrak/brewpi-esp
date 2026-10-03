# Bounded HTTP upload transport regression

Run `python3 tests/water_test_transport/run.py` after one firmware build installs
ArduinoJson. This compiles the production HTTP transport against a deterministic
socket facade. It checks partial, zero and failed writes, exact source lengths,
512-byte write buffering, open/allocation failures, the request deadline,
read timeouts, mandatory response completion, and cleanup on failure.

Responses are read in 256-byte chunks with an 8192-byte wire limit. Only required
acknowledgement and API error fields are retained, with a 2304-byte parser budget.
Tests cover large ignored fields, oversized included fields, chunked responses,
truncated bodies, trailing data, sanitized/truncated server errors, and disabled
redirects. Native variant pools are configured to match the device's 1024-byte
pool allocation; no network or device is accessed.
