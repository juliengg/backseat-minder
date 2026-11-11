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
from bluetooth_transport import BluetoothConnection


CELLULAR_STATUSES = {
    "ALERT_STARTED": ("Presence alert started; driver away", "darkorange"),
    "ALERT_CANCELLED": ("Alert cancelled; monitoring suppressed", "blue"),
    "ALERT_REARMED": ("Driver returned and sensors clear; alert rearmed", "green"),
    "ALERT_STOPPED": ("Horn and voice cancellation acknowledged", "blue"),
    "ALERT_STOP_FAILED": ("Could not confirm cancellation; check device", "red"),
    "STOP_UNAUTHORIZED": ("STOP ignored: sender is not a saved contact", "darkorange"),
    "ALARM_ACCEPTED": ("Repeating horn request accepted", "blue"),
    "ALARM_OK": ("Repeating horn scheduled: 1 second on, 2 seconds off", "darkorange"),
    "ALARM_FAILED": ("Repeating horn failed; check secondary logs", "red"),
    "CALL_WAITING_ACK": ("Recorded call: waiting for secondary", "darkorange"),
    "CALL_ACCEPTED": ("Recorded call workflow accepted", "blue"),
    "CALL_OK": ("Recording finished; call remains connected", "green"),
    "CALL_DRY_RUN": ("Call disabled in this firmware; no number dialed", "darkorange"),
    "CALL_CANCELLED": ("Recorded call cancelled", "blue"),
    "RX_READY": ("Incoming SMS reception ready", "green"),
    "RX_ERROR": ("Could not configure/read incoming SMS - check secondary logs", "red"),
    "RX_QUEUE_FULL": ("Incoming SMS queue full - some texts were not displayed", "red"),
    "READY": ("SMS/horn UART ready (secondary not yet checked)", "gray"),
    "HORN_WAITING_ACK": ("Horn: waiting for secondary acknowledgement", "darkorange"),
    "HORN_ACCEPTED": ("Horn: secondary running chirp sequence", "blue"),
    "HORN_OK": ("Horn: CAN sequence submitted", "green"),
    "HORN_BUSY": ("Horn rejected: SMS or horn already in progress", "darkorange"),
    "HORN_CAN_UNAVAILABLE": ("Horn: MCP2515 unavailable - check secondary logs", "red"),
    "HORN_CAN_SEND_FAILED": ("Horn: CAN send failed - restart secondary after checking bus", "red"),
    "HORN_CAN_OFF_FAILED": ("Horn: OFF frame failed - check vehicle and secondary logs", "red"),
    "HORN_ACK_TIMEOUT": ("Horn: no acknowledgement - waiting for result", "darkorange"),
    "HORN_RESULT_TIMEOUT": ("Horn: result timed out - outcome unknown", "red"),
    "HORN_LINK_UNAVAILABLE": ("Horn: primary UART unavailable", "red"),
    "HORN_UART_WRITE_FAILED": ("Horn: UART write failed - outcome unknown", "red"),
    "HORN_QUEUE_FAILED": ("Horn: could not queue request", "red"),
    "HORN_INVALID_RESPONSE": ("Horn: invalid response from secondary", "red"),
    "WAITING_ACK": ("Waiting for secondary acknowledgement", "darkorange"),
    "ACCEPTED": ("Acknowledged - checking modem / sending", "blue"),
    "OK": ("SMS submitted successfully", "green"),
    "DRY_RUN": ("Dry run complete - no SMS sent", "darkorange"),
    "NO_PHONE": ("No valid phone number saved - open setup", "red"),
    "NO_NAME": ("No name saved - open setup", "red"),
    "INVALID_NAME": ("Name unsupported in SMS - update it in setup", "red"),
    "SIM_NOT_READY": ("SIM missing, locked, or not ready", "red"),
    "NOT_REGISTERED": ("Not registered on the cellular network", "red"),
    "MODEM_UNAVAILABLE": ("Modem not responding - check power/wiring", "red"),
    "BUSY": ("Busy - additional request rejected", "darkorange"),
    "ACK_TIMEOUT": ("No acknowledgement - still waiting for result", "darkorange"),
    "RESULT_TIMEOUT": ("Result timed out - SMS outcome unknown", "red"),
    "SMS_OUTCOME_UNKNOWN": ("SMS outcome unknown - check before retrying", "red"),
    "LINK_UNAVAILABLE": ("Primary cellular UART unavailable", "red"),
    "UART_WRITE_FAILED": ("UART write failed - SMS outcome unknown", "red"),
    "INVALID_REQUEST": ("Invalid phone number or message", "red"),
    "INVALID_RESPONSE": ("Invalid response received from secondary", "red"),
    "QUEUE_FAILED": ("Could not queue the SMS request", "red"),
    "SMS_REJECTED": ("SMS rejected by modem", "red"),
    "SMS_PROMPT": ("Modem did not accept the SMS recipient", "red"),
    "SMS_SETUP": ("Modem SMS setup failed", "red"),
    "MODEM_SETUP": ("Modem setup failed", "red"),
    "MODEM_TIMEOUT": ("Modem timed out", "red"),
}


class SmsControl:
    """Send one deliberate USB request; wait for firmware receipt, never retry."""

    def __init__(self, device):
        self.device = device
        self.last_received = None
        self.requests = None
        self.uptime = None
        self.supported = False
        self.busy = False
        self.pending_since = None
        self.message = "Waiting for SMS controls from device..."

    def observe(self, sample, now):
        self.supported = (sample.get("sms_control") is True
                          and type(sample.get("sms_busy")) is bool
                          and type(sample.get("sms_requests")) is int
                          and 0 <= sample["sms_requests"] <= 0xFFFFFFFF)
        self.last_received = now
        if not self.supported:
            return
        if self.pending_since is not None:
            if self.uptime is not None and sample["uptime_ms"] < self.uptime:
                self.message = "Device restarted - SMS outcome unknown; check before retrying."
                self.pending_since = None
            elif sample["sms_requests"] != self.requests:
                self.message = "Request received by device - see cellular status below."
                self.pending_since = None
        elif self.requests is None:
            self.message = "Uses the name and phone number saved in device setup."
        self.requests = sample["sms_requests"]
        self.uptime = sample["uptime_ms"]
        self.busy = sample["sms_busy"]

    def refresh(self, now):
        if self.pending_since is not None and now - self.pending_since > 10:
            self.pending_since = None
            self.message = "No command confirmation - SMS outcome unknown; check before retrying."
        if self.last_received is None or now - self.last_received > 10:
            return False, "Waiting for fresh cellular telemetry..."
        if not self.supported:
            return False, "Update primary firmware to enable dashboard SMS."
        if self.pending_since is not None:
            return False, self.message
        if self.busy:
            return False, "SMS or horn in progress - waiting for the result."
        return True, self.message

    def send(self, now):
        if not self.refresh(now)[0]:
            return
        # Latch before writing so repeated clicks cannot submit another request.
        self.pending_since = now
        self.message = "Command sent - waiting for device confirmation..."
        try:
            command = b"SEND_SMS\n"
            if self.device.write(command) != len(command):
                raise OSError("incomplete USB write")
        except (serial.SerialException, OSError):
            # A failed/partial write may still have reached the device. Do not retry.
            self.message = ("Bluetooth" if getattr(self.device,"is_bluetooth",False) is True else "USB") + " write failed - SMS outcome unknown; check before retrying."


class UsbConnection:
    """Passively reconnect after deep sleep; reading never keeps the board awake."""

    def __init__(self, device):
        self.device = device
        self.next_attempt = 0
        self.generation = 0

    def read_available(self, now):
        if not self.device.is_open:
            if now < self.next_attempt:
                return None
            try:
                self.device.open()
                self.generation += 1
            except (serial.SerialException, OSError):
                self.next_attempt = now + 0.5
                return None
        try:
            available = self.device.in_waiting
            return self.device.read(available) if available else b""
        except (serial.SerialException, OSError):
            try:
                self.device.close()
            except (serial.SerialException, OSError):
                pass
            self.next_attempt = now + 0.5
            return None


def driver_view(sample):
    """Format a current driver flag without inventing a physical distance."""
    if type(sample.get("driver_present")) is not bool:
        return "Driver present: unknown (no beacon telemetry)", "gray"
    if sample.get("driver_checking") is True:
        if sample.get("power_state") == "beacon_probe" and sample["driver_present"]:
            return "Driver present: YES\nDeep sleep mode (periodic beacon check; last confirmed)", "green"
        previous = "YES (last confirmed); " if sample["driver_present"] else ""
        return f"Driver present: {previous}checking beacon...", "gray"
    if sample.get("beacon_available") is not True:
        return "Driver present: unknown (BLE unavailable; monitoring active)", "red"
    present = sample["driver_present"]
    rssi = sample.get("beacon_rssi_dbm")
    signal = (f" | {rssi} dBm" if sample.get("beacon_rssi_valid") is True
              and type(rssi) is int else " | no fresh beacon signal")
    power = ("Deep sleep mode (last announced)" if sample.get("deep_sleep_pending")
             else "Deep sleep mode (periodic beacon check)"
             if sample.get("power_state") == "beacon_probe" and present else "Active")
    return f"Driver present: {'YES' if present else 'NO'}{signal}\n{power}", (
        "green" if present else "darkorange")


class DriverDisplay:
    """Hold a confirmed sleep/presence report through USB gaps and wake probes."""

    def __init__(self):
        self.sample = {}
        self.last_received = None

    def observe(self, sample, now):
        if type(sample.get("driver_present")) is not bool:
            return
        self.last_received = now
        report = dict(sample)
        if (sample.get("driver_checking") is True
                and sample.get("power_state") == "beacon_probe"
                and (sample["driver_present"] or self.sample.get("driver_present") is True)):
            report["driver_present"] = True
        self.sample = report

    def view(self, now):
        text, color = driver_view(self.sample)
        if self.last_received is not None and now - self.last_received > 15:
            text += f"\nLast report: {int(now - self.last_received)} seconds ago"
        return text, color


def cellular_view(sample: dict) -> tuple:
    """Validate a BSMC snapshot and format its bounded event history."""
    if not isinstance(sample, dict) or not isinstance(sample.get("events"), list):
        raise ValueError("expected cellular events")
    uptime = sample.get("uptime_ms")
    if type(uptime) is not int or uptime < 0 or len(sample["events"]) > 8:
        raise ValueError("invalid cellular snapshot")
    history = []
    text, color = "Waiting for a cellular event", "gray"
    for event in sample["events"]:
        if not isinstance(event, dict):
            raise ValueError("invalid cellular event")
        status, timestamp = event.get("status"), event.get("uptime_ms")
        if (not isinstance(status, str) or not 1 <= len(status) <= 47
                or any(c not in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_" for c in status)
                or type(timestamp) is not int or not 0 <= timestamp <= uptime):
            raise ValueError("invalid cellular event fields")
        if status == "SMS_RECEIVED":
            sender, message = event.get("sender"), event.get("message")
            if (not isinstance(sender, str) or not 1 <= len(sender) <= 31
                    or not sender.lstrip("+").isascii()
                    or not sender.lstrip("+").isdigit() or "+" in sender[1:]
                    or not isinstance(message, str) or len(message.encode("utf-8")) > 480
                    or any((ord(c) < 32 and c not in "\n\r\t") or ord(c) == 127 for c in message)):
                raise ValueError("invalid received SMS")
            text, color = f"Received SMS from {sender}", "green"
        else:
            text, color = CELLULAR_STATUSES.get(status, (f"Cellular error: {status}", "red"))
        minutes, seconds = divmod(timestamp // 1000, 60)
        history.append(f"{minutes:02d}:{seconds:02d}  {text}")
        if status == "SMS_RECEIVED":
            body = message.replace("\r\n", "\n").replace("\r", "\n")
            history.extend(f"    {line}" for line in body.split("\n"))
    return text, color, "\n".join(history)


def update_cellular_panel(label, history_widget, sample: dict) -> None:
    text, color, history = cellular_view(sample)
    label.configure(text=text, fg=color)
    history_widget.configure(state="normal")
    history_widget.delete("1.0", "end")
    history_widget.insert("end", history)
    history_widget.see("end")
    history_widget.configure(state="disabled")


def device_forecast_line(sample: dict, elapsed: float, temperature: float):
    """Display only the forecast transmitted by the ESP32; never fit one here."""
    forecast = sample.get("forecast_7min_f")
    if (sample.get("alert_state") == "active" and sample.get("forecast_valid")
            and sample.get("temperature_humidity_valid")
            and isinstance(forecast, (int, float)) and math.isfinite(forecast)
            and math.isfinite(temperature)):
        return [elapsed, elapsed + 420], [temperature, forecast]
    return [], []


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
    transport=parser.add_mutually_exclusive_group(required=True)
    transport.add_argument("--port",help="Native USB COM port, e.g. COM15")
    transport.add_argument("--bluetooth",action="store_true",help="Use paired Bluetooth LE instead of USB")
    parser.add_argument("--bluetooth-address",help="Optional address/UUID of a specific BLE device")
    parser.add_argument("--bluetooth-name",default="Backseat Minder")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    if args.bluetooth_address and not args.bluetooth:
        parser.error("--bluetooth-address requires --bluetooth")
    device = None
    try:
        if args.bluetooth:
            try:
                from bluetooth_transport import BluetoothSerial
                device=BluetoothSerial(args.bluetooth_address,args.bluetooth_name)
            except ImportError:
                print("Bluetooth mode needs: python -m pip install 'bleak>=1.0'",file=sys.stderr)
                return 1
            print("Bluetooth mode: searching for Backseat Minder; approve pairing if prompted.")
            return show_viewer(device)
        # The COM port may not exist until the next timed beacon check.
        device = serial.Serial(port=None, baudrate=args.baud, timeout=0, write_timeout=0.25)
        device.dtr = False
        device.rts = False
        device.port = args.port
        print(f"Waiting for {args.port}; reconnecting automatically after sleep. Press Ctrl+C to stop.")
        return show_viewer(device)
    except serial.SerialException as error:
        print(f"Could not open {args.port}: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0
    finally:
        if device is not None:
            device.close()


def show_viewer(device) -> int:
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

    bluetooth=getattr(device,"is_bluetooth",False) is True
    transport_name="Bluetooth" if bluetooth else "USB"
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

    driver_label = tk.Label(values_frame, text=driver_view({})[0],
                            anchor="w", justify="left", wraplength=310,
                            fg="gray", font=("TkDefaultFont", 10, "bold"))
    driver_label.pack(fill="x", pady=(8, 0))
    connection_label = tk.Label(values_frame, text=f"{transport_name} connected", anchor="w",
                                justify="left", wraplength=310, fg="gray")
    connection_label.pack(fill="x")

    cellular_frame = tk.LabelFrame(values_frame, text="Cellular alerts")
    cellular_frame.pack(fill="both", expand=True, pady=(8, 0))
    sms_control = SmsControl(device)

    def refresh_sms_button():
        enabled, message = sms_control.refresh(time.monotonic())
        sms_button.configure(state="normal" if enabled else "disabled")
        sms_hint.configure(text=message)

    def send_sms():
        sms_control.send(time.monotonic())
        refresh_sms_button()

    sms_button = tk.Button(cellular_frame, text="Send SMS", command=send_sms,
                           state="disabled")
    sms_button.pack(anchor="w", padx=4, pady=4)
    sms_hint = tk.Label(cellular_frame, text=sms_control.message,
                        anchor="w", justify="left", wraplength=310, fg="gray")
    sms_hint.pack(fill="x", padx=4)
    cellular_label = tk.Label(cellular_frame, text="Waiting for cellular telemetry...",
                              anchor="w", justify="left", wraplength=310, fg="gray")
    cellular_label.pack(fill="x", padx=4, pady=4)
    cellular_history = tk.Text(cellular_frame, height=4, width=42, wrap="word",
                               state="disabled", font=("TkDefaultFont", 9))
    cellular_scroll = tk.Scrollbar(cellular_frame, command=cellular_history.yview)
    cellular_scroll.pack(side="right", fill="y")
    cellular_history.configure(yscrollcommand=cellular_scroll.set)
    cellular_history.pack(fill="both", expand=True, padx=4, pady=(0, 4))

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
        label="ESP32 forecast (+7 minutes)")
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
    temperature_axis.axhline(104,color="firebrick",linestyle=":",linewidth=1)
    temperature_axis.grid(True, alpha=0.25)
    temperature_axis.legend(
        handles=[temperature_line, humidity_line, temperature_fit, humidity_fit],
        loc="upper left", fontsize=8, ncol=2)
    trend_figure.tight_layout()
    trend_canvas = FigureCanvasTkAgg(trend_figure, master=plot_frame)
    trend_canvas.get_tk_widget().pack(fill="both", expand=True)
    history = deque(maxlen=3000)
    first_reading_time = None
    previous_alert_state = None

    def update_trend(sample: dict) -> None:
        nonlocal first_reading_time, previous_alert_state
        now = float(sample.get("uptime_ms", time.monotonic()*1000))/1000
        state = sample.get("alert_state", "armed")
        if (history and now < history[-1][0]) or (state == "active" and previous_alert_state != "active"):
            history.clear()
            first_reading_time = None
        if state == "active" and first_reading_time is None:
            first_reading_time = now - float(sample.get("alert_elapsed_ms",0))/1000
        previous_alert_state = state
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
        forecast_ok = bool(sample.get("forecast_valid")) and state == "active" and valid
        forecast = sample.get("forecast_7min_f")
        if forecast_ok and isinstance(forecast,(float,int)) and np.isfinite(forecast):
            temperature_fit.set_data(*device_forecast_line(sample,elapsed[-1],temperature))
        else:
            temperature_fit.set_data([],[])
        temperature_axis.set_title(
            f"Alert: {state} | elapsed {float(sample.get('alert_elapsed_ms',0))/60000:.1f} min | "
            + (f"ESP32 slope {sample.get('slope_f_per_min',0):.2f} F/min; +7 min {forecast:.1f} F"
               if forecast_ok and isinstance(forecast,(float,int)) else "ESP32 forecast unavailable"),fontsize=8)
        humidity_fit.set_data(*regression_line(elapsed, humidities))
        latest_time = now - first_reading_time
        temperature_axis.set_xlim(max(0, latest_time - 300), max(300, latest_time + (420 if forecast_ok else 0)))
        temperature_axis.relim()
        temperature_axis.autoscale_view(scalex=False, scaley=True)
        trend_canvas.draw_idle()

    receive_buffer = bytearray()
    maximum_lengths = {
        b"BSMF": 200_000, b"BSMT": 1024, b"BSMH": 8_000, b"BSMD": 5,
        b"BSMC": 10000, b"BSMP": 512,
    }
    thermal_frame_count = 0
    cellular_last_received = None
    cellular_stale = False
    cellular_rendered_events = None
    driver_display = DriverDisplay()
    connection = BluetoothConnection(device) if bluetooth else UsbConnection(device)

    def update_driver(sample):
        driver_display.observe(sample, time.monotonic())
        text, color = driver_display.view(time.monotonic())
        driver_label.configure(text=text, fg=color)
        if sample.get("deep_sleep_pending") is True or sample.get("power_state") == "beacon_probe":
            sms_control.last_received = None
            connection_label.configure(text=f"Sleep announced; {transport_name} will reconnect for beacon checks",
                                        fg="gray")
        else:
            connection_label.configure(text=f"{transport_name} connected", fg="gray")

    def poll_device() -> None:
        nonlocal thermal_frame_count, cellular_last_received, cellular_stale
        nonlocal cellular_rendered_events
        previous_generation = connection.generation
        data = connection.read_available(time.monotonic())
        if data is None:
            receive_buffer.clear()
            sms_control.last_received = None
            refresh_sms_button()
            connection_label.configure(
                text=((device.status if bluetooth else "USB disconnected (sleep or unplugged); reconnecting automatically.")
                          + "\nDriver flag shows the last report."), fg="gray")
        else:
            if connection.generation != previous_generation:
                receive_buffer.clear()  # A packet cannot span a device reboot.
                cellular_stale = True
                connection_label.configure(text=f"{transport_name} reconnected", fg="gray")
            receive_buffer.extend(data)

        text, color = driver_display.view(time.monotonic())
        driver_label.configure(text=text, fg=color)

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

            if packet_type == b"BSMP":
                try:
                    sample = json.loads(payload.decode("utf-8"))
                    if not isinstance(sample, dict):
                        raise ValueError("expected driver telemetry object")
                    update_driver(sample)
                except (UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError):
                    pass
                continue

            if packet_type == b"BSMC":
                try:
                    sample = json.loads(payload.decode("utf-8"))
                    cellular_view(sample)  # Validate even an unchanged snapshot.
                    sms_control.observe(sample, time.monotonic())
                    if sample["events"] != cellular_rendered_events or cellular_stale:
                        update_cellular_panel(cellular_label, cellular_history, sample)
                        cellular_rendered_events = sample["events"]
                    cellular_last_received = time.monotonic()
                    cellular_stale = False
                except (UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError):
                    pass
                continue

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
                    update_driver(sample)
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

        if (cellular_last_received is not None and not cellular_stale
                and time.monotonic() - cellular_last_received > 10):
            cellular_label.configure(text="Cellular telemetry stale - showing last events", fg="gray")
            cellular_stale = True
        refresh_sms_button()
        root.after(100 if data is None else 10, poll_device)

    root.after(10, poll_device)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
