#!/usr/bin/env python3
"""Check real metadata storage with bounded C++ allocations and damaged files."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
headers = next((root / '.pio/libdeps').glob('*/ArduinoJson/src'))
source = (root / 'src/WaterTestStorage.cpp').read_text().replace('#include "ESPEepromAccess.h"', '')
with tempfile.TemporaryDirectory(prefix='water-test-storage-') as folder:
    build = Path(folder)
    cpp, binary = build / 'test.cpp', build / 'test'
    cpp.write_text((here / 'environment.h').read_text() + '\n' + source + '\n' +
                   (here / 'test.cpp').read_text())
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-DARDUINOJSON_SIZEOF_POINTER=4', '-I' + str(root / 'src'), '-I' + str(headers),
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(build)], check=True)
