"""Exercise the real runner's host preflight with simulated interface metadata."""
import ipaddress
import json
import pathlib
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'scripts'))
import jetson_smoke as smoke

addresses = {'127.0.0.1', '172.16.1.50'}
original_is_file = pathlib.Path.is_file
original_read_text = pathlib.Path.read_text
with patch.object(smoke, 'local_ipv4_addresses', return_value=addresses, create=True):
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory) / 'report'
        argv = ['smoke', 'missing-source.yaml', '--host', '192.0.2.55', '--output-dir', str(root)]
        with patch.object(sys, 'argv', argv), patch.object(smoke.platform, 'machine', return_value='aarch64'), \
             patch.object(pathlib.Path, 'is_file', lambda p: str(p) == '/etc/nv_tegra_release' or original_is_file(p)), \
             patch.object(pathlib.Path, 'read_text', lambda p: 'test L4T' if str(p) == '/etc/nv_tegra_release' else original_read_text(p)):
            assert smoke.main() == 1
        report = json.loads((root / 'report.json').read_text())
        assert 'assigned to this Jetson' in report.get('error', ''), report
        assert 'edge_pid' not in report, report
    smoke.validate_host(ipaddress.IPv4Address('172.16.1.50'), False)
    smoke.validate_host(ipaddress.IPv4Address('127.0.0.1'), True)
    try:
        smoke.validate_host(ipaddress.IPv4Address('127.0.0.1'), False)
    except RuntimeError:
        pass
    else:
        raise AssertionError('real Jetson accepted loopback as its LAN host')
