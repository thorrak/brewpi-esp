#!/usr/bin/env python3
"""Run the actual static HTTP handlers with native filesystem/response I/O."""
import os
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / "src/http_server.cpp").read_text()
start = source.index("static bool endsWith(")
end = source.index("// Route registration", start)
with tempfile.TemporaryDirectory(prefix="brewpi-static-files-") as temp:
    build = Path(temp)
    translation = build / "test.cpp"
    translation.write_text(
        '#include "shim.h"\n' + source[start:end] + '\n' +
        (here / "test.inc.cpp").read_text()
    )
    binary = build / "test"
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-I", str(here), str(translation), "-o", str(binary)
    ], check=True)
    subprocess.run([str(binary), str(build)], check=True)
