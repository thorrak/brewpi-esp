#!/usr/bin/env python3
"""Execute the production WaterTest.cpp against a deterministic ESP/RTOS facade.

Only platform include directives are replaced; every lifecycle, journal, reboot,
HTTP request/acknowledgement, and scheduling function comes from the real source.
Run from any directory after PlatformIO has installed ArduinoJson.
"""
from pathlib import Path
import argparse
import subprocess
import json
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--recovery-fixture-dir', type=Path,
                    help='Retain the recovered dual-controller JSON for portal contract checks.')
arguments = parser.parse_args()

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'scripts'))
from controller_identity import controller_identity
implementation_id = controller_identity(root)
headers = next((path for path in (root / '.pio/libdeps').glob('*/ArduinoJson/src')
                if (path / 'ArduinoJson.h').is_file()), None)
if headers is None:
    raise SystemExit('Build a firmware target first to install ArduinoJson.')
allowed = {'#include "WaterTest.h"', '#include "WaterTestCore.h"', '#include "WaterTestProtocol.h"',
           '#include "WaterTestStorage.h"', '#include "WaterTestTransport.h"', '#include "WaterTestUpload.h"',
           '#include "WaterTestControllerSnapshot.h"'}
source = '\n'.join(line for name in ('WaterTestStorage.cpp', 'WaterTestTransport.cpp', 'WaterTest.cpp')
                   for line in (root / 'src' / name).read_text().splitlines()
                   if not line.startswith('#include') or line in allowed)
with tempfile.TemporaryDirectory(prefix='water-test-backend-') as temp:
    build = Path(temp)
    history = (root / 'src/WaterTestCore.h').read_text()
    allocation = 'std::malloc(sizeof(History))'
    assert history.count(allocation) == 1
    (build / 'WaterTestCore.h').write_text(history.replace(allocation, 'Native::allocateHistory(sizeof(History))'))
    for header in ('WaterTestProtocol.h', 'WaterTestUpload.h'):
        (build / header).write_text((root / 'src' / header).read_text())
    translation = build / 'backend.cpp'
    translation.write_text('#include "NativeEnvironment.h"\n' + source + '\n' +
                           (root / 'tests/water_test_backend/test.inc.cpp').read_text())
    binary = build / 'test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-DARDUINOJSON_SIZEOF_POINTER=4', '-DARDUINOJSON_POOL_CAPACITY=64', '-I' + str(root / 'src'), '-I' + str(root / 'tests/water_test_backend'),
                    '-I' + str(headers), '-DCOOLING_IMPLEMENTATION_ID=' + json.dumps(implementation_id),
                    str(translation), str(root / 'src/GlycolCoolingController.cpp'),
                    str(root / 'src/PredictiveCoastController.cpp'), str(root / 'src/AdaptiveDoseController.cpp'),
                    '-o', str(binary)], check=True)
    for name in ['history_allocation_failure', 'history_held_allocation_failure', 'uploader_lifecycle', 'uploader_allocation_retry', 'uploader_network_backoff',
                 'flow_validation', 'flow_retention', 'normal_stop', 'baseline_stop', 'minimum_stop', 'full_pulse_stop',
                 'transient_sensor_errors', 'transient_bad_start', 'prolonged_invalid', 'silent_sensor',
                 'minimum_water_limit', 'bath_fault', 'status_freshness',
                 'fsync_failure', 'edge_fsync_failure', 'start_failure', 'queue_overflow', 'unexpected_output',
                 'upload_retry', 'upload_transport_diagnostics', 'upload_server_error_bounds',
                 'upload_request_setup_failure', 'upload_allocation_failure', 'upload_pacing',
                 'upload_bounded_streaming', 'upload_metadata_crc', 'upload_batch_crc',
                 'upload_partial_write_retry', 'upload_lost_ack_retry', 'upload_incomplete_ack_retry', 'upload_large_ack_retry', 'upload_workspace_allocation_failure',
                 'upload_pacing_http_retry', 'upload_pacing_ack_retry', 'upload_pacing_preparation_retry',
                 'all_pulses', 'completed', 'observation_timeout',
                 'failure_after_full_pulse', 'stale_stop_after_full_pulse',
                 'slow_sensor_fault', 'slow_temperature_limit', 'slow_deadline', 'slow_on_edge',
                 'slow_phase', 'slow_storage_repair',
                 'queue_snapshot', 'slow_queue_overflow', 'queued_unexpected_output', 'cleanup_failure',
                 'resume_pending_upload', 'independent_hardware_availability', 'configured_glycol_probe',
                 'offline_12h_budget', 'finish_persistence_failure', 'receipt_persistence_failure',
                 'reserve_persistence_failure', 'controller_interrupted_status', 'controller_continuous_pulse_limit',
                 'controller_fast_loop_predictive', 'controller_fast_loop_dose',
                 'controller_snapshot_predictive', 'controller_snapshot_dose',
                 'controller_checkpoint_precision', 'snapshot_exact_restoration',
                 'controller_observations_predictive', 'controller_observations_dose',
                 'controller_observation_write_failure', 'controller_observation_stop', 'controller_observation_stale',
                 'controller_declared_duration_recovery',
                 'dual_controllers_predictive_first', 'dual_controllers_dose_first',
                 'dual_controllers_full_duration',
                 'dual_controller_headroom_skip', 'dual_controller_checkpoint_failure',
                 'dual_controller_stale_run', 'dual_controller_corrupt_checkpoint',
                 'controller_run_write_deadline', 'controller_observation_write_deadline',
                 'controller_final_durable', 'controller_final_filters', 'controller_final_slow_write',
                 'controller_final_write_failure', 'controller_final_stop', 'controller_final_stale',
                 'controller_final_temperature', 'controller_final_unexpected_output',
                 'controller_final_fractional_deadline']:
        subprocess.run([str(binary), name, str(build / name)], check=True)
    for before, after in [('active', 'recovered_active'), ('stopped', 'recovered_ended'),
                          ('pending', 'recovered_ended'), ('partial_upload', 'recovered_ended'),
                          ('resumed_pending', 'recovered_resumed'), ('submitted', 'idle'),
                          ('corrupt', 'blocked'), ('controller_snapshot', 'recovered_snapshot'),
                          ('checkpoint_precision', 'recovered_checkpoint_precision'),
                          ('flow', 'recovered_flow'), ('controller_episodes', 'recovered_controller_episodes'),
                          ('controller_observations', 'recovered_controller_observations'),
                          ('dual_first_checkpoint', 'recovered_dual_first_checkpoint'),
                          ('dual_second_running', 'recovered_dual_second_running'),
                          ('controller_final_tail', 'recovered_controller_final_tail'),
                          ('controller_duration_boundary', 'recovered_controller_duration_boundary'),
                          ('dual_settled_before_checkpoint', 'recovered_dual_settled_before_checkpoint'),
                          ('orphan_empty', 'orphan_retry'),
                          ('orphan_nonempty', 'orphan_blocked'), ('orphan_manifest', 'orphan_blocked'),
                          ('orphan_finish', 'orphan_blocked'), ('orphan_finish_tmp', 'orphan_blocked'),
                          ('orphan_ack_tmp', 'orphan_blocked'), ('orphan_resumed_tmp', 'orphan_blocked'),
                          ('orphan_boots_tmp', 'orphan_blocked'), ('orphan_unreadable', 'orphan_unreadable'),
                          ('orphan_controller_one_tmp', 'orphan_blocked'),
                          ('orphan_controller_only', 'orphan_blocked'),
                          ('orphan_controller_tmp_only', 'orphan_blocked'),
                          ('orphan_controller_two_tmp', 'orphan_blocked')]:
        reboot = build / before
        subprocess.run([str(binary), before + '_before_reboot', str(reboot)], check=True)
        subprocess.run([str(binary), after + '_after_reboot', str(reboot)], check=True)
        if before == 'controller_duration_boundary' and arguments.recovery_fixture_dir:
            arguments.recovery_fixture_dir.mkdir(parents=True, exist_ok=True)
            name = 'recovered-duration-boundary.json'
            (arguments.recovery_fixture_dir / name).write_bytes((reboot / name).read_bytes())
        if before == 'dual_settled_before_checkpoint' and arguments.recovery_fixture_dir:
            arguments.recovery_fixture_dir.mkdir(parents=True, exist_ok=True)
            name = 'recovered-controller-observation.json'
            (arguments.recovery_fixture_dir / name).write_bytes((reboot / name).read_bytes())
    reboot = build / 'double_reboot'
    for scenario in ['active_before_reboot', 'recover_only_after_reboot', 'recovered_active_after_reboot']:
        subprocess.run([str(binary), scenario, str(reboot)], check=True)
    reboot = build / 'lazy_recovery'
    for scenario in ['stopped_before_reboot', 'recovered_lazy_after_reboot']:
        subprocess.run([str(binary), scenario, str(reboot)], check=True)
