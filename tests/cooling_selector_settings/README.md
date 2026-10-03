Run `python3 tests/cooling_selector_settings/run.py` after any PlatformIO build,
or pass `--arduinojson /path/to/ArduinoJson/src`.

The native harness compiles the branch's actual extended-settings declarations,
serialization and file loading, HTTP update and GET providers, and Telnet
command methods. Only filesystem location, display, mode and transport I/O are
stubbed. It checks upgrade defaults, both selections surviving reload, omitted
keys, selector-only updates, idempotence, and whole-request rejection of invalid
selectors or supplied field types before any mode/display/flash effects.
