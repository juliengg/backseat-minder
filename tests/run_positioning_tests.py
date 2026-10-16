"""Run actual preview-cache C++ and portal-builder checks using host stubs.

Run: python tests/run_positioning_tests.py --zig
Browser logic: node tests/setup_positioning_test.cjs
"""
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
    executable = output / ("positioning_preview.exe" if os.name == "nt" else "positioning_preview")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Itests/stubs", "-Imain"]
    subprocess.run(compiler + flags + ["tests/positioning_preview_test.cpp",
        "main/positioning_preview.cpp", "-o", str(executable)], cwd=ROOT, env=env, check=True)
    subprocess.run([str(executable)], cwd=ROOT, check=True)

    # Compile the exact portal builder, without coupling host tests to Wi-Fi/NVS.
    source = (ROOT / "main/setup_mode.cpp").read_text(encoding="utf-8")
    limits = "\n".join(line for line in source.splitlines()
                       if line.startswith(("#define MAX_", "#define PORTAL_HTML_MAXLEN")))
    config = source[source.index("typedef struct {"):source.index("static void nvs_load_config")]
    builder = source[source.index("static std::string html_escape"):source.index("static const char *CONFIRM_HTML")]
    generated = output / "portal_builder_test.cpp"
    generated.write_text('''#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
''' + limits + "\n" + config + builder + '''
int main(int argc, char **argv) {
    assert(argc == 2);
    bsm_config_t cfg{};
    char html[PORTAL_HTML_MAXLEN];
    assert(build_portal_html(html, sizeof(html), &cfg));
    FILE *file = fopen(argv[1], "wb");
    assert(file);
    fputs(html, file);
    fclose(file);
    assert(strstr(html, "View positioning"));
    assert(strstr(html, "id='view-positioning'"));
    assert(strstr(html, "id='positioning-preview' hidden"));
    assert(strstr(html, "src='/positioning.js' defer"));
    assert(!strstr(html, "src='/positioning.jpg")); // No automatic camera request.
    memset(cfg.name, '&', sizeof(cfg.name) - 1);
    memset(cfg.phone, '\\'', sizeof(cfg.phone) - 1);
    memset(cfg.ec1, '<', sizeof(cfg.ec1) - 1);
    memset(cfg.ec2, '>', sizeof(cfg.ec2) - 1);
    memset(cfg.ec3, '\\"', sizeof(cfg.ec3) - 1);
    cfg.emerg_alerts = true;
    assert(build_portal_html(html, sizeof(html), &cfg)); // Worst-case HTML escaping.
    assert(strstr(html, "&amp;") && strstr(html, "&#39;"));
    assert(!build_portal_html(html, 100, &cfg));
    puts("PASS: actual setup HTML, preview off by default, escaped form values and capacity");
}
''', encoding="utf-8")
    portal_executable = output / ("portal_builder.exe" if os.name == "nt" else "portal_builder")
    subprocess.run(compiler + flags + [str(generated), "-o", str(portal_executable)],
                   cwd=ROOT, env=env, check=True)
    subprocess.run([str(portal_executable), str(output / "portal.html")], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
