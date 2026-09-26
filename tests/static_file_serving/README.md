Run `python3 tests/static_file_serving/run.py` from the repository root.

The harness extracts the production static-file handlers and replaces only HTTP
responses and the filesystem mount prefix. It verifies that canonical and aliased
water-test filenames are rejected before filesystem access, while normal files,
compressed assets, missing files, and the water-test page retain their behavior.
