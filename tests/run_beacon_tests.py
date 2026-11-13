"""Host tests of actual BLE tracker and continuous-monitoring boot initialization: python tests/run_beacon_tests.py --zig."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--zig", action="store_true")
    args = parser.parse_args()
    output = ROOT / "tests" / ".build"
    output.mkdir(exist_ok=True)
    compiler = ([sys.executable, "-m", "ziglang", "c++"] if args.zig
                else [os.environ.get("CXX", "c++")])
    env = os.environ.copy()
    env["ZIG_GLOBAL_CACHE_DIR"] = str(output / "zig-cache")
    for name, sources in (("beacon_tracker", ["main/beacon_tracker.cpp"]),
                          ("driver_power", [])):
        executable = output / (name + (".exe" if os.name == "nt" else ""))
        subprocess.run(compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-Itests/beacon_stubs", "-Itests/stubs", "-Imain",
            f"tests/{name}_test.cpp"] + sources + ["-o", str(executable)],
            cwd=ROOT, env=env, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
