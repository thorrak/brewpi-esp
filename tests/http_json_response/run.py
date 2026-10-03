#!/usr/bin/env python3
"""Exercise production JSON response streaming and PUT acknowledgment semantics."""

import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def production_function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    json_headers = next((path for path in (ROOT / ".pio/libdeps").glob("*/ArduinoJson/src")
                         if (path / "ArduinoJson.h").is_file()), None)
    if json_headers is None:
        raise SystemExit("Build a firmware target first to install ArduinoJson.")
    with tempfile.TemporaryDirectory(prefix="brewpi-http-json-") as temporary:
        binary = Path(temporary) / "test"
        subprocess.run([
            os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
            "-Werror", "-pedantic", "-DENABLE_HTTP_INTERFACE",
            "-I", str(HERE / "shim"), "-I", str(ROOT / "src"), "-I", str(json_headers),
            str(ROOT / "src/HttpJsonResponse.cpp"), str(HERE / "test.cpp"), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)

        # Compile the actual parser and PUT template without the HTTP server's
        # unrelated device, route-registration, and RTOS dependencies.
        source = (ROOT / "src/http_server.cpp").read_text()
        production = Path(temporary) / "production_put_handler.h"
        production.write_text("\n".join(production_function(source, signature) for signature in (
            "esp_err_t httpServer::parseJsonBody(",
            "template<bool (*Handler)(const JsonDocument&, bool)>",
        )))
        put_binary = Path(temporary) / "put_test"
        subprocess.run([
            os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
            "-Werror", "-pedantic", "-I", temporary, "-I", str(json_headers),
            str(HERE / "put_handler_test.cpp"), "-o", str(put_binary),
        ], check=True)
        subprocess.run([str(put_binary)], check=True)
    print("HTTP JSON response and PUT handler regressions passed.")


if __name__ == "__main__":
    main()
