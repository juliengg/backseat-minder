# Primary ESP-IDF environment

The project configuration and dependency lock use ESP-IDF **v5.5.4**. Its existing
camera and face-detection interfaces require legacy ESP-WHO, not the current
ESP-WHO API. The local setup uses the official `release/v1.1.0` branch, pinned at
`205da6d3193989794a68c49f0b7dcf524bff5a74`, with its ESP-DL submodule pinned at
`78a81eb0a20c19ff2ad982850082784eba7594e1`.

Local installation paths:

- ESP-IDF: `$env:USERPROFILE/esp/esp-idf-v5.5.4`
- ESP-WHO: `$env:USERPROFILE/esp/esp-who-legacy`
- Compiler, tools and isolated Python environment: `$env:USERPROFILE/.espressif`

These framework downloads and tool installations are outside the Git repository.
No global system PATH change is required. Espressif recommends short, space-free
Windows tool paths; the build helper also uses a short build-output directory.

From the repository in PowerShell, build the primary without flashing:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File ./tools/Build-Primary.ps1
```

The command above permits local scripts for that PowerShell process only. It does
not change the machine's execution policy. If scripts are already permitted in
your terminal, `./tools/Build-Primary.ps1` is sufficient.

For interactive ESP-IDF commands, use a terminal that permits local scripts
(`powershell.exe -NoProfile -ExecutionPolicy RemoteSigned`) and dot-source:

```powershell
. ./tools/Enter-PrimaryEnvironment.ps1
python "$env:IDF_PATH/tools/idf.py" --version
python "$env:IDF_PATH/tools/idf.py" -B "$env:USERPROFILE/esp/build-backseat-minder" build
```

The scripts accept `-IdfPath` and `-WhoPath` for other installation locations.
The root CMake project uses the `ESP_WHO_PATH` environment variable or CMake cache
setting rather than a hardcoded directory from another computer. Keep the
existing `sdkconfig` to retain the ESP32-S3 target, custom camera pins, and other
board settings; no `set-target` reset is required for this existing project.

Installing the environment and running `build` do not update either board.
Flashing remains a separate operation requiring the correct board connection and
authorization. Hardware behavior must be verified separately after flashing.

Official references:

- [ESP-IDF v5.5.4 Windows setup](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/windows-setup.html)
- [Legacy ESP-WHO source](https://github.com/espressif/esp-who/tree/release/v1.1.0)

The legacy ESP-WHO README names IDF 5.0 as its baseline. This project's complete
ESP32-S3 build with the pinned legacy framework and IDF 5.5.4 was successfully
verified on 2026-11-05, including the new alert workflow. The application image is
`0x1e5d40` bytes and fits the existing `0x200000`-byte application partition
(approximately 5% free). No device was flashed by this verification.

Output image: `$env:USERPROFILE/esp/build-backseat-minder/backseat-minder.bin`.
The first build compiles about 1,500 steps; later builds reuse the compiled output.
Hardware behavior still requires separate testing after flashing.
