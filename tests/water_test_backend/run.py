#!/usr/bin/env python3
"""Execute the production WaterTest.cpp against a deterministic ESP/RTOS facade.

Only platform include directives are replaced; every lifecycle, journal, recovery,
HTTP request/acknowledgement, and scheduling function comes from the real source.
Run from any directory after PlatformIO has installed ArduinoJson.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/WaterTest.cpp').read_text()
allowed = {'#include "WaterTest.h"', '#include "WaterTestCore.h"', '#include "WaterTestProtocol.h"'}
source = '\n'.join(line for line in source.splitlines()
                   if not line.startswith('#include') or line in allowed)
with tempfile.TemporaryDirectory(prefix='water-test-backend-') as temp:
    build = Path(temp)
    translation = build / 'backend.cpp'
    translation.write_text('#include "NativeEnvironment.h"\n' + source + '\n' +
                           (root / 'tests/water_test_backend/test.inc.cpp').read_text())
    binary = build / 'test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'src'), '-I' + str(root / 'tests/water_test_backend'),
                    '-I' + str(root / '.pio/libdeps/esp32_wifi_iic/ArduinoJson/src'),
                    str(translation), '-o', str(binary)], check=True)
    for name in ['normal_stop', 'minimum_stop', 'sensor_fault', 'bath_fault', 'status_freshness',
                 'fsync_failure', 'edge_fsync_failure', 'start_failure', 'queue_overflow', 'unexpected_output',
                 'upload_retry', 'all_pulses']:
        subprocess.run([str(binary), name, str(build / name)], check=True)
    recovery = build / 'reboot'
    subprocess.run([str(binary), 'active_before_reboot', str(recovery)], check=True)
    subprocess.run([str(binary), 'after_reboot', str(recovery)], check=True)

    upload_recovery = build / 'upload-reboot'
    subprocess.run([str(binary), 'partial_upload_before_reboot', str(upload_recovery)], check=True)
    subprocess.run([str(binary), 'after_partial_upload_reboot', str(upload_recovery)], check=True)
