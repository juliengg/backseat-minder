#!/usr/bin/env python3
"""Display Backseat Minder visible/thermal cameras and telemetry.

Install: python -m pip install pyserial pillow matplotlib numpy
Run:     python tools/usb_telemetry_viewer.py --port COM4
"""

import argparse
import io
import json
import struct
import sys

import serial


def detection_statuses(sample: dict) -> dict:
    """Read sensor results, including packets from older firmware."""
    face_detected = bool(sample.get("face_detected", False))
    mmwave_presence_detected = bool(sample.get(
        "mmwave_presence_detected", sample.get("mmwave_person_detected", False)))
    thermal_heat_detected = bool(sample.get("heat_trace_detected", False))
    return {
        "Human presence": bool(sample.get(
            "human_presence_detected",
            face_detected or mmwave_presence_detected or thermal_heat_detected)),
        "Face detected": face_detected,
        "mmWave radar presence": mmwave_presence_detected,
        "Thermal heat detected": thermal_heat_detected,
    }


def update_detection_labels(labels: dict, sample: dict) -> None:
    for name, detected in detection_statuses(sample).items():
        labels[name].configure(
            text="✓" if detected else "✗",
            fg="green" if detected else "red")


def main() -> int:
    parser = argparse.ArgumentParser(description="View Backseat Minder cameras and telemetry")
    parser.add_argument("--port", required=True, help="COM port, e.g. COM4")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    try:
        with serial.Serial(args.port, args.baud, timeout=1) as device:
            print(f"Listening on {args.port}. Press Ctrl+C to stop.")
            return show_viewer(device)
    except serial.SerialException as error:
        print(f"Could not open {args.port}: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0


def show_viewer(device: serial.Serial) -> int:
    try:
        import tkinter as tk
        import numpy as np
        from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
        from matplotlib.figure import Figure
        from matplotlib.patches import Rectangle
        from PIL import Image, ImageTk
    except ImportError:
        print("Install viewer dependencies with: "
              "python -m pip install pillow matplotlib numpy", file=sys.stderr)
        return 1

    root = tk.Tk()
    root.title("Backseat Minder Camera and Thermal View")
    view_height = 360
    views = tk.Frame(root)
    views.pack(fill="both", expand=True, padx=12, pady=12)

    visible_frame = tk.LabelFrame(views, text="Visible camera")
    visible_frame.grid(row=0, column=0, sticky="nsew", padx=(0, 6))
    camera_placeholder = tk.PhotoImage(width=480, height=view_height)
    image_label = tk.Label(visible_frame, text="Waiting for camera frames…",
                           image=camera_placeholder, compound="center")
    image_label.image = camera_placeholder
    image_label.pack(fill="both", expand=True)

    thermal_frame = tk.LabelFrame(views, text="MLX90640 thermal camera")
    thermal_frame.grid(row=0, column=1, sticky="nsew", padx=(6, 0))
    figure = Figure(figsize=(4.8, view_height / 100), dpi=100)
    thermal_axis = figure.add_subplot(111)
    thermal_image = thermal_axis.imshow(
        np.zeros((24, 32)), cmap="jet", interpolation="bicubic",
        vmin=15, vmax=40, origin="upper")
    heat_trace_rectangle = Rectangle(
        (0, 0), 0, 0, fill=False, edgecolor="white", linewidth=2.5,
        visible=False)
    thermal_axis.add_patch(heat_trace_rectangle)
    colorbar = figure.colorbar(thermal_image, ax=thermal_axis)
    colorbar.set_label("Temperature (°C)")
    thermal_title = thermal_axis.set_title("Waiting for thermal frames…")
    thermal_axis.axis("off")
    figure.tight_layout()
    thermal_canvas = FigureCanvasTkAgg(figure, master=thermal_frame)
    thermal_canvas.get_tk_widget().pack(fill="both", expand=True)

    views.columnconfigure(0, weight=1)
    views.columnconfigure(1, weight=1)
    views.rowconfigure(0, weight=1)
    telemetry_label = tk.Label(root, text="Waiting for telemetry…",
                               justify="left", anchor="w")
    telemetry_label.pack(fill="x", padx=12, pady=(0, 4))
    detection_frame = tk.Frame(root)
    detection_frame.pack(fill="x", padx=12, pady=(0, 12))
    detection_labels = {}
    for row, name in enumerate(detection_statuses({})):
        font = (("TkDefaultFont", 12, "bold") if name == "Human presence"
                else ("TkDefaultFont", 10))
        name_label = tk.Label(detection_frame, text=f"{name}:", anchor="w",
                              font=font)
        name_label.grid(row=row, column=0, sticky="w")
        status_label = tk.Label(detection_frame, text="—", anchor="center",
                                width=3, fg="gray", font=font)
        status_label.grid(row=row, column=1, sticky="n", padx=(8, 0))
        detection_labels[name] = status_label

    receive_buffer = bytearray()
    maximum_lengths = {
        b"BSMF": 200_000, b"BSMT": 512, b"BSMH": 8_000, b"BSMD": 5,
    }
    thermal_frame_count = 0

    def poll_device() -> None:
        nonlocal thermal_frame_count
        available = device.in_waiting
        receive_buffer.extend(device.read(available if available else 1))

        while True:
            candidates = [receive_buffer.find(marker) for marker in maximum_lengths]
            candidates = [index for index in candidates if index >= 0]
            marker_index = min(candidates) if candidates else -1
            if marker_index < 0:
                del receive_buffer[:-3]
                break
            if marker_index:
                del receive_buffer[:marker_index]
            if len(receive_buffer) < 8:
                break

            packet_type = bytes(receive_buffer[:4])
            payload_length = struct.unpack(">I", receive_buffer[4:8])[0]
            if (packet_type not in maximum_lengths or payload_length == 0 or
                    payload_length > maximum_lengths[packet_type]):
                del receive_buffer[:4]
                continue
            if len(receive_buffer) < 8 + payload_length:
                break
            payload = bytes(receive_buffer[8:8 + payload_length])
            del receive_buffer[:8 + payload_length]

            if packet_type == b"BSMT":
                try:
                    sample = json.loads(payload.decode("utf-8"))
                    if not isinstance(sample, dict):
                        raise ValueError("expected a telemetry object")
                    telemetry_label.configure(
                        text=(f"Temperature: {sample.get('temperature_f', 0):.1f} °F\n"
                              f"Humidity: {sample.get('humidity_percent', 0):.1f} %\n"
                              f"Temperature/humidity valid: {sample.get('temperature_humidity_valid')}")
                    )
                    update_detection_labels(detection_labels, sample)
                except (UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError):
                    pass
                continue

            if packet_type == b"BSMH":
                try:
                    values = np.fromstring(payload.decode("ascii"), sep=",")
                    if values.size != 768 or not np.isfinite(values).all():
                        raise ValueError("expected 768 finite temperatures")
                    frame = values.reshape((24, 32))
                    thermal_image.set_data(frame)
                    thermal_frame_count += 1
                    thermal_title.set_text(
                        f"Frame {thermal_frame_count}  |  "
                        f"{frame.min():.1f}–{frame.max():.1f} °C")
                    thermal_canvas.draw_idle()
                except (UnicodeDecodeError, ValueError):
                    pass
                continue

            if packet_type == b"BSMD":
                if len(payload) == 5 and payload[0]:
                    _, x, y, width, height = payload
                    # imshow pixels are centered on integer coordinates, so
                    # rectangle edges sit half a pixel outside the blob.
                    heat_trace_rectangle.set_xy((x - 0.5, y - 0.5))
                    heat_trace_rectangle.set_width(width)
                    heat_trace_rectangle.set_height(height)
                    heat_trace_rectangle.set_visible(True)
                else:
                    heat_trace_rectangle.set_visible(False)
                thermal_canvas.draw_idle()
                continue

            try:
                image = Image.open(io.BytesIO(payload))
                image.load()
                scaled_width = round(image.width * view_height / image.height)
                image = image.resize((scaled_width, view_height), Image.Resampling.LANCZOS)
                preview = ImageTk.PhotoImage(image)
                image_label.configure(image=preview, text="")
                image_label.image = preview
            except Exception as error:
                image_label.configure(text=f"Could not decode camera frame: {error}", image="")

        root.after(10, poll_device)

    root.after(10, poll_device)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
