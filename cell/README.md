# ESP32 to TEL0161 SMS sender

This Arduino sketch sends one SMS to a preset number through a DFRobot TEL0161 (SIM7600G). It is configured for the Freenove ESP32-S3 WROOM and uses UART1, leaving the usual USB serial interface free for the Serial Monitor. A press of the board's BOOT button is the temporary alert trigger until the temperature sensor is added.

## What you must edit

Open `config.h` and set:

- `DESTINATION_NUMBER` to the recipient in E.164 format, for example `+15551234567`.
- `SMS_MESSAGE` to the text you want to send. The supplied sketch uses GSM 7-bit text, so keep it to plain ASCII characters and at most 160 characters.
- `SEND_REAL_SMS` to `true` only after the SIM, network registration, wiring, and test mode are confirmed.
- `MODEM_RX_PIN` and `MODEM_TX_PIN` only if you intentionally use different pins. The checked-in defaults match the Freenove ESP32-S3 WROOM pinout.

## Wiring

Use the TEL0161 UART connector labels, not wire colors.

| TEL0161 UART | ESP32 | Notes |
|---|---|---|
| `T` / TX | GPIO18 (`U1RXD`) | Module transmit to ESP32 receive |
| `R` / RX | `MODEM_TX_PIN` (default GPIO17) | ESP32 transmit to module receive |
| `-` / GND | GND | Grounds must be connected together |
| `+` / VCC | Separate 5 V supply | Do **not** power it from the ESP32 3.3 V pin |

The TEL0161 board accepts 5-12 V on its power input when using UART. Use a stable supply and attach the LTE `MAIN` antenna before network testing. The TEL0161 UART is specified for 3.3-5 V logic, so direct ESP32 3.3 V UART signaling is appropriate.

The Freenove pinout labels GPIO17 as `U1TXD` and GPIO18 as `U1RXD`, which is why they are the defaults. Do not use ESP32-S3 strapping pins GPIO0, GPIO3, GPIO45, or GPIO46 for this connection.

## Build and flash with Visual Studio Code

This is now a PlatformIO project. You do not need Arduino IDE.

1. Install [Visual Studio Code](https://code.visualstudio.com/) if necessary. (This workflow is for **Visual Studio Code**, not the larger Visual Studio IDE.)
2. In Visual Studio Code, open **Extensions** (`Ctrl+Shift+X`), search for **PlatformIO IDE**, and install the extension published by PlatformIO.
3. Select **File > Open Folder** and open this `tel0161_sms_esp32` folder - the folder containing `platformio.ini`.
4. Connect the Freenove ESP32-S3 to the laptop with a data-capable USB cable. PlatformIO will install its ESP32 build tools the first time you build.
5. Keep `SEND_REAL_SMS = false` in `include/config.h` for the first run. Click the **PlatformIO: Build** check-mark in VS Code's bottom status bar. A successful build ends with `SUCCESS` in the terminal.
6. Click **PlatformIO: Upload** (right-arrow) to flash the board. If it waits at `Connecting...`, hold **BOOT**, start Upload, then release BOOT once writing starts.
7. Click **PlatformIO: Serial Monitor** (plug icon), or run **PlatformIO: Monitor** from the Command Palette. It is already configured for 115200 baud. Press **RST/EN** once to rerun the sketch.

The `platformio.ini` file uses PlatformIO's `esp32-s3-devkitc-1` board profile with the Arduino framework. This profile is a compatible baseline for the Freenove ESP32-S3 WROOM; no additional libraries are needed for this sketch. PlatformIO expects the compiled source in `src/main.cpp` and settings in `include/config.h`.

## Arduino IDE alternative

1. Install Arduino IDE 2 from [arduino.cc](https://www.arduino.cc/en/software) if it is not already installed.
2. In **File > Preferences**, add this URL under **Additional boards manager URLs**:

   ```text
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. Open **Tools > Board > Boards Manager**, search for `esp32`, and install **esp32 by Espressif Systems**.
4. Connect the Freenove ESP32-S3 to the laptop using a data-capable USB cable. In **Tools > Board**, select **ESP32S3 Dev Module**. In **Tools > Port**, select the new COM port. Leave the remaining board settings at their defaults initially.
5. Open `tel0161_sms_esp32.ino` from this folder. Arduino opens the matching sketch folder and also finds `config.h` automatically.
6. Confirm that `SEND_REAL_SMS` is still `false` in `config.h`, then click **Verify** (check-mark) to compile. Click **Upload** (right-arrow) to build and flash it.
7. If upload remains on `Connecting...`, hold **BOOT**, click Upload, release BOOT when writing begins, then press **RST/EN** once after upload completes.
8. Open **Tools > Serial Monitor**, choose **115200 baud**, and press **RST/EN** to rerun the sketch.

For the first ESP32 test, disconnect the TEL0161 USB-C cable from the laptop and power the TEL0161 through its own 5 V input. Keep the ESP32 connected to the laptop only for programming/debugging. Do not use the ESP32 3.3 V pin to power the TEL0161.

With `SEND_REAL_SMS = false`, press and release BOOT after startup. The sketch checks modem communication and prints `TEST MODE` instead of sending. With an active SIM, the log should show `+CPIN: READY`, signal quality from `AT+CSQ`, and network registration from `AT+CREG?` / `AT+CEREG?` before you enable real sending. Do not hold BOOT while powering or resetting the ESP32, because it is GPIO0, a boot-selection pin.

## SIM and carrier requirements

You need an activated physical SIM with SMS service; a data-only SIM will not work. The carrier must allow the SIM7600G/TEL0161 on its network. If the SIM has a PIN, either disable it before use or add the PIN command to the sketch; do not hard-code a real PIN in source code shared with others.

## What the code sends

The SIMCom manual specifies this text-mode transaction:

```text
AT+CMGF=1
AT+CMGS="+15551234567"
> message text, followed by Ctrl-Z (0x1A)
+CMGS: <reference>
OK
```

The sketch checks each response and prints modem errors to the Serial Monitor. `+CMS ERROR` usually points to a missing SIM, lack of network/SMS service, a malformed recipient number, or insufficient account credit.

## Still needed from you

Before changing the defaults, please confirm:

1. Which temperature sensor will eventually trigger the SMS? The BOOT-button trigger can then be replaced with sensor-trigger and alert/cooldown logic.
