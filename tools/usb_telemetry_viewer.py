#!/usr/bin/env python3
"""Display Backseat Minder visible/thermal cameras and telemetry.

Install: python -m pip install pyserial pillow matplotlib numpy
Run:     python tools/usb_telemetry_viewer.py --port COM4
"""

import argparse
from collections import deque
import io
import json
import math
import struct
import sys
import time

import serial


def regression_line(times, values):
    """Return endpoints of a least-squares fit to the last 10 finite samples."""
    points = [(x, y) for x, y in zip(times, values)
              if math.isfinite(x) and math.isfinite(y)][-10:]
    if len(points) < 10:
        return [], []
    xs, ys = zip(*points)
    mean_x = sum(xs) / len(xs)
    mean_y = sum(ys) / len(ys)
    variance = sum((x - mean_x) ** 2 for x in xs)
    if variance == 0:
        return [], []
    slope = sum((x - mean_x) * (y - mean_y) for x, y in points) / variance
    endpoints = [xs[0], xs[-1]]
    return endpoints, [mean_y + slope * (x - mean_x) for x in endpoints]


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
    lower_panel = tk.Frame(root)
    lower_panel.pack(fill="both", expand=True, padx=12, pady=(0, 12))
    values_frame = tk.Frame(lower_panel)
    values_frame.pack(side="left", fill="y", padx=(0, 12))
    telemetry_label = tk.Label(values_frame, text="Waiting for telemetry…",
                               justify="left", anchor="w")
    telemetry_label.pack(fill="x", pady=(0, 4))
    detection_frame = tk.Frame(values_frame)
    detection_frame.pack(fill="x")
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

    plot_frame = tk.LabelFrame(lower_panel, text="Temperature and humidity — last 5 minutes")
    plot_frame.pack(side="left", fill="both", expand=True)
    trend_figure = Figure(figsize=(6.5, 2.8), dpi=100)
    temperature_axis = trend_figure.add_subplot(111)
    humidity_axis = temperature_axis.twinx()
    temperature_line, = temperature_axis.plot(
        [], [], color="tab:red", label="Temperature", marker=".", markersize=3)
    humidity_line, = humidity_axis.plot(
        [], [], color="tab:blue", label="Humidity", marker=".", markersize=3)
    temperature_fit, = temperature_axis.plot(
        [], [], color="darkred", linestyle="--", linewidth=2, zorder=4,
        label="Temperature fit (10 readings)")
    humidity_fit, = humidity_axis.plot(
        [], [], color="navy", linestyle="--", linewidth=2, zorder=4,
        label="Humidity fit (10 readings)")
    temperature_axis.set_xlabel("Time (seconds)")
    temperature_axis.set_ylabel("Temperature (°F)", color="tab:red")
    humidity_axis.set_ylabel("Humidity (%)", color="tab:blue")
    temperature_axis.tick_params(axis="y", labelcolor="tab:red")
    humidity_axis.tick_params(axis="y", labelcolor="tab:blue")
    temperature_axis.set_xlim(0, 300)
    temperature_axis.set_ylim(32, 120)
    humidity_axis.set_ylim(0, 100)
    temperature_axis.grid(True, alpha=0.25)
    temperature_axis.legend(
        handles=[temperature_line, humidity_line, temperature_fit, humidity_fit],
        loc="upper left", fontsize=8, ncol=2)
    trend_figure.tight_layout()
    trend_canvas = FigureCanvasTkAgg(trend_figure, master=plot_frame)
    trend_canvas.get_tk_widget().pack(fill="both", expand=True)
    history = deque(maxlen=3000)
    first_reading_time = None

    def update_trend(sample: dict) -> None:
        nonlocal first_reading_time
        now = time.monotonic()
        if first_reading_time is None:
            first_reading_time = now
        try:
            temperature = float(sample["temperature_f"])
            humidity = float(sample["humidity_percent"])
            valid = (sample.get("temperature_humidity_valid", False)
                     and np.isfinite(temperature) and np.isfinite(humidity))
        except (KeyError, TypeError, ValueError):
            valid = False
        if not valid:
            # Gaps prevent failed sensor reads from looking like real measurements.
            temperature = humidity = float("nan")
        history.append((now, temperature, humidity))
        while history and history[0][0] < now - 300:
            history.popleft()
        times, temperatures, humidities = zip(*history)
        elapsed = [timestamp - first_reading_time for timestamp in times]
        temperature_line.set_data(elapsed, temperatures)
        humidity_line.set_data(elapsed, humidities)
        temperature_fit.set_data(*regression_line(elapsed, temperatures))
        humidity_fit.set_data(*regression_line(elapsed, humidities))
        latest_time = now - first_reading_time
        temperature_axis.set_xlim(max(0, latest_time - 300), max(300, latest_time))
        temperature_axis.relim()
        temperature_axis.autoscale_view(scalex=False, scaley=True)
        trend_canvas.draw_idle()

    receive_buffer = bytearray()
    maximum_lengths = {
        b"BSMF": 200_000, b"BSMT": 512, b"BSMH": 8_000, b"BSMD": 5,
    }
    thermal_frame_count = 0

    def poll_device() -> None:
        nonlocal thermal_frame_count
        available = device.in_waiting
        if available:
            receive_buffer.extend(device.read(available))

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
                    update_trend(sample)
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
