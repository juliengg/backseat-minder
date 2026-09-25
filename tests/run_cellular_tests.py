"""Host tests of actual firmware sources, using fake UART/modem/GPIO devices.

Run with a C++ compiler on PATH, or: python run_cellular_tests.py --zig
(--zig uses the optional project-local `ziglang` Python package).
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--zig', action='store_true')
    args = parser.parse_args()
    output = ROOT / 'tests' / '.build'
    output.mkdir(exist_ok=True)
    compiler = [sys.executable, '-m', 'ziglang', 'c++'] if args.zig else [os.environ.get('CXX', 'c++')]
    env = os.environ.copy()
    env['ZIG_GLOBAL_CACHE_DIR'] = str(output / 'zig-cache')
    # Use the checked-in camera configuration in the primary pin-conflict guard.
    config_dir = output / 'config'
    config_dir.mkdir(exist_ok=True)
    camera_defines = []
    for line in (ROOT / 'sdkconfig').read_text().splitlines():
        if line.startswith('CONFIG_CAMERA_PIN_'):
            name, value = line.split('=', 1)
            camera_defines.append(f'#define {name} {value}\n')
    (config_dir / 'sdkconfig.h').write_text(''.join(camera_defines))
    for real_sms in (0, 1):
        executable = output / f'cellular_{real_sms}{".exe" if os.name == "nt" else ""}'
        command = compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-O0',
            f'-DBSM_SEND_REAL_SMS={real_sms}', '-Itests/stubs', '-Ishared', '-Imain',
            '-Icell/include', 'tests/cellular_host_test.cpp', 'main/boot_button.cpp',
            '-o', str(executable)]
        subprocess.run(command, cwd=ROOT, env=env, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)
    executable = output / f'cellular_link{".exe" if os.name == "nt" else ""}'
    command = compiler + ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-O0',
        '-Itests/stubs', '-Ishared', '-Imain', f'-I{config_dir}',
        'tests/cellular_link_test.cpp', 'main/cellular_diagnostics.cpp', '-o', str(executable)]
    subprocess.run(command, cwd=ROOT, env=env, check=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == '__main__':
    main()
