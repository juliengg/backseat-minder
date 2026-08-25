#!/usr/bin/env python3
"""Display the Backseat Minder camera preview and telemetry.

Install once:
    python -m pip install pyserial

Run on Windows (replace COM5 with the USB-OTG port):
    python tools/usb_telemetry_viewer.py --port COM5
"""

import argparse
import io
import json
import struct
import sys

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description="View Backseat Minder camera and telemetry")
    parser.add_argument("--port", required=True, help="COM port, e.g. COM4")
    parser.add_argument("--baud", type=int, default=115200,
                        help="Serial baud rate (defaults to the firmware's 115200 setting)")
    args = parser.parse_args()

    try:
        with serial.Serial(args.port, args.baud, timeout=1) as device:
            print(f"Listening on {args.port}. Press Ctrl+C to stop.")
            return show_camera(device)
    except serial.SerialException as error:
        print(f"Could not open {args.port}: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0


def show_camera(device: serial.Serial) -> int:
    try:
        import tkinter as tk
        from PIL import Image, ImageTk
    except ImportError:
        print("Camera preview requires Pillow. Install it with: python -m pip install pillow", file=sys.stderr)
        return 1

    root = tk.Tk()
    root.title("Backseat Minder Camera")
    image_label = tk.Label(root, text="Waiting for camera frames…")
    image_label.pack(padx=12, pady=12)
    telemetry_label = tk.Label(root, text="Waiting for telemetry…", justify="left", anchor="w")
    telemetry_label.pack(fill="x", padx=12, pady=(0, 12))
    receive_buffer = bytearray()
    max_jpeg_length = 200_000

    def poll_camera() -> None:
        available = device.in_waiting
        receive_buffer.extend(device.read(available if available else 1))

        while True:
            frame_index = receive_buffer.find(b"BSMF")
            telemetry_index = receive_buffer.find(b"BSMT")
            marker_candidates = [index for index in (frame_index, telemetry_index) if index >= 0]
            marker_index = min(marker_candidates) if marker_candidates else -1
            if marker_index < 0:
                # Retain enough trailing data to recognize a split marker.
                del receive_buffer[:-3]
                break
            if marker_index:
                del receive_buffer[:marker_index]
            if len(receive_buffer) < 8:
                break

            packet_type = bytes(receive_buffer[:4])
            payload_length = struct.unpack(">I", receive_buffer[4:8])[0]
            max_payload_length = max_jpeg_length if packet_type == b"BSMF" else 512
            if payload_length == 0 or payload_length > max_payload_length:
                del receive_buffer[:4]
                continue
            if len(receive_buffer) < 8 + payload_length:
                break

            payload = bytes(receive_buffer[8:8 + payload_length])
            del receive_buffer[:8 + payload_length]
            if packet_type == b"BSMT":
                try:
                    sample = json.loads(payload.decode("utf-8"))
                    telemetry_label.configure(
                        text=(f"Temperature: {sample.get('temperature_f', 0):.1f} °F\n"
                              f"Humidity: {sample.get('humidity_percent', 0):.1f} %\n"
                              f"Face detected: {'yes' if sample.get('face_detected') else 'no'}\n"
                              f"Sensor valid: {sample.get('temperature_humidity_valid')}")
                    )
                except (UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError):
                    pass
                continue

            try:
                image = Image.open(io.BytesIO(payload))
                image.load()
                preview = ImageTk.PhotoImage(image)
                image_label.configure(image=preview, text="")
                image_label.image = preview
            except Exception as error:
                image_label.configure(text=f"Could not decode camera frame: {error}", image="")

        root.after(10, poll_camera)

    root.after(10, poll_camera)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
