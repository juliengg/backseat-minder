"""
MLX90640 Thermal Camera Visualizer
------------------------------------
Reads CSV frames (768 comma-separated floats per line) from the
ESP32-S3 over serial and displays them as a live thermal heatmap.

Install dependencies:
    pip install pyserial matplotlib numpy
"""

import threading
import serial
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.animation as animation

# ---- Configuration ----
SERIAL_PORT = "COM4"       # Change to match your ESP32-S3's port
BAUD_RATE = 115200
FRAME_WIDTH = 32
FRAME_HEIGHT = 24
FRAME_SIZE = FRAME_WIDTH * FRAME_HEIGHT  # 768

# ---- Shared state between serial thread and GUI thread ----
latest_frame = np.zeros((FRAME_HEIGHT, FRAME_WIDTH))
frame_lock = threading.Lock()
frame_count = 0
parse_errors = 0
running = True


def serial_reader():
    """Runs in a background thread, continuously reading and parsing
    lines so the GUI thread never blocks on serial I/O."""
    global latest_frame, frame_count, parse_errors

    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    ser.reset_input_buffer()

    while running:
        try:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
        except serial.SerialException:
            break

        if not line:
            continue

        values = line.split(",")
        if len(values) != FRAME_SIZE:
            parse_errors += 1
            continue

        try:
            frame = np.array(values, dtype=float).reshape(
                (FRAME_HEIGHT, FRAME_WIDTH)
            )
        except ValueError:
            parse_errors += 1
            continue

        with frame_lock:
            latest_frame = frame
            frame_count += 1

    ser.close()


# ---- Start background serial thread ----
reader_thread = threading.Thread(target=serial_reader, daemon=True)
reader_thread.start()

# ---- Matplotlib setup ----
fig, ax = plt.subplots(figsize=(8, 6))

img = ax.imshow(
    latest_frame,
    cmap="inferno",
    interpolation="bicubic",
    vmin=15, vmax=40   # fixed range — adjust to your expected temps
)
cbar = fig.colorbar(img, ax=ax)
cbar.set_label("Temperature (°C)")
title = ax.set_title("MLX90640 Thermal Camera — waiting for data...")
ax.axis("off")


def update(frame_num):
    with frame_lock:
        data = latest_frame.copy()
        count = frame_count
        errors = parse_errors

    img.set_data(data)
    title.set_text(f"MLX90640 Thermal Camera  |  frames: {count}  errors: {errors}")

    return [img, title]


ani = animation.FuncAnimation(fig, update, interval=100, blit=False)

plt.tight_layout()
plt.show()

running = False
reader_thread.join(timeout=1)