#!/usr/bin/env python3
"""Display STM32 tuner readings from the ST-LINK virtual serial port."""

import argparse
from dataclasses import dataclass
import json
import math
from queue import Empty, Full, Queue
import re
from threading import Event, Thread
import time
import tkinter as tk


BAUD = 115200
A4_HZ = 440
IN_TUNE_CENTS = 5
GAUGE_LIMIT = 50
MAX_LINE_BYTES = 512
READING_TIMEOUT = 1.0
NOTE_PATTERN = re.compile(r"[A-G]#?-?\d{1,2}")


@dataclass(frozen=True)
class Reading:
    a4_hz: float
    status: str
    note: str = ""
    frequency_hz: float = 0.0
    target_hz: float = 0.0
    cents: float = 0.0


def parse_reading(line):
    def number(value):
        return type(value) in (int, float) and math.isfinite(value)

    try:
        record = json.loads(line)
        a4_hz = record["a4_hz"]
        status = record["status"]
        if not number(a4_hz) or a4_hz <= 0:
            return None
        if status in ("NO_SIGNAL", "AUDIO_ERROR"):
            return Reading(a4_hz, status)
        if status not in ("IN_TUNE", "FLAT", "SHARP"):
            return None

        note = record["note"]
        frequency, target, cents = (record[key] for key in ("frequency_hz", "target_hz", "cents"))
        if not isinstance(note, str) or not NOTE_PATTERN.fullmatch(note):
            return None
        if not all(number(value) for value in (frequency, target, cents)):
            return None
        if frequency <= 0 or target <= 0:
            return None
        return Reading(a4_hz, status, note, frequency, target, cents)
    except (KeyError, TypeError, ValueError, OverflowError):
        return None


def find_port(list_ports):
    ports = list(list_ports.comports())
    st_links = [
        port.device for port in ports
        if port.vid == 0x0483 or "st-link" in (port.description or "").lower()
    ]
    if len(st_links) == 1:
        return st_links[0]

    if not st_links:
        usb_modems = [port.device for port in ports if port.device.startswith("/dev/cu.usbmodem")]
        if len(usb_modems) == 1:
            return usb_modems[0]
    return None


def receive_serial(serial, list_ports, requested_port, publish, stopped):
    while not stopped.is_set():
        port = requested_port or find_port(list_ports)
        if not port:
            publish(None, "Connect the board or select a port with --port")
            stopped.wait(1)
            continue

        try:
            with serial.Serial(port, BAUD, timeout=0.10) as connection:
                publish(None, f"Waiting for UART · {port}")
                pending = bytearray()
                discard_line = False
                while not stopped.is_set():
                    chunk = connection.read(connection.in_waiting or 1)
                    for part in chunk.splitlines(keepends=True):
                        if not discard_line:
                            pending.extend(part)
                            discard_line = len(pending) > MAX_LINE_BYTES
                        if part.endswith(b"\n"):
                            reading = None if discard_line else parse_reading(pending)
                            if reading:
                                publish(reading, port)
                            pending.clear()
                            discard_line = False
        except (OSError, serial.SerialException):
            publish(None, f"UART unavailable · {port}")
            stopped.wait(1)


class TunerWindow:
    BACKGROUND = "#0D2333"
    TEXT = "#F1F7F9"
    MUTED = "#B5D3D6"
    ACCENT = "#7DD9D0"
    TRACK = "#466B7D"
    BAND = "#184C57"
    NEEDLE = "#F6CB6B"

    def __init__(self, root):
        self.root = root
        self.root.title("STM32 Tuner")
        self.root.geometry("720x455")
        self.root.minsize(450, 290)
        self.canvas = tk.Canvas(root, background=self.BACKGROUND, highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)
        self.reading = None
        self.connection = ""
        self.canvas.bind("<Configure>", lambda _event: self.draw())

    def update(self, reading, connection):
        if (reading, connection) != (self.reading, self.connection):
            self.reading, self.connection = reading, connection
            self.draw()

    def draw(self):
        canvas = self.canvas
        canvas.delete("all")
        scale = min(canvas.winfo_width() / 680, canvas.winfo_height() / 425)
        left = (canvas.winfo_width() - 680 * scale) / 2
        top = (canvas.winfo_height() - 425 * scale) / 2

        def point(x, y):
            return left + x * scale, top + y * scale

        def text(x, y, value, size, color, weight="normal", anchor="center"):
            canvas.create_text(*point(x, y), text=value, fill=color, anchor=anchor,
                               font=("Helvetica", max(1, round(size * scale)), weight))

        def line(x1, y1, x2, y2, color, width):
            canvas.create_line(*point(x1, y1), *point(x2, y2),
                               fill=color, width=max(1, width * scale))

        def rounded_rect(x1, y1, x2, y2, radius, color):
            canvas.create_rectangle(*point(x1 + radius, y1), *point(x2 - radius, y2),
                                    fill=color, outline="")
            canvas.create_oval(*point(x1, y1), *point(x1 + 2 * radius, y2),
                               fill=color, outline="")
            canvas.create_oval(*point(x2 - 2 * radius, y1), *point(x2, y2),
                               fill=color, outline="")

        reading = self.reading
        a4_hz = reading.a4_hz if reading else A4_HZ
        has_pitch = reading is not None and bool(reading.note)
        text(30, 35, "TUNER", 16, self.ACCENT, "bold", "w")
        text(650, 35, f"A4 = {a4_hz:g} Hz", 14, self.MUTED, anchor="e")

        text(340, 140, reading.note if has_pitch else "–", 84, self.TEXT, "bold")
        if has_pitch:
            text(340, 209, f"{reading.frequency_hz:.1f} Hz  ·  {reading.cents:+.0f} cents",
                 23, self.MUTED)
        else:
            message = "Waiting for a single note" if reading else (self.connection or "Waiting for UART")
            if reading and reading.status == "AUDIO_ERROR":
                message = "STM32 audio capture error"
            text(340, 209, message, 17, self.MUTED)

        def gauge_x(cents):
            return 340 + cents * 4.8

        rounded_rect(gauge_x(-IN_TUNE_CENTS) - 6, 268,
                     gauge_x(IN_TUNE_CENTS) + 6, 298, 15, self.BAND)
        line(100, 283, 580, 283, self.TRACK, 4)
        for cents in (-50, -25, 0, 25, 50):
            x = gauge_x(cents)
            if cents not in (-50, 50):
                tick = 11 if cents == 0 else 7
                line(x, 283 - tick, x, 283 + tick, self.MUTED, 2)
            label = f"{cents:+d}" if cents > 0 else str(cents).replace("-", "−")
            text(x, 315, label, 13, self.MUTED)

        if has_pitch:
            x = gauge_x(max(-GAUGE_LIMIT, min(GAUGE_LIMIT, reading.cents)))
            canvas.create_polygon(*point(x - 4, 255), *point(x + 4, 255),
                                  *point(x, 268), fill=self.NEEDLE)
            line(x, 263, x, 297, self.NEEDLE, 3)

        if has_pitch:
            status = reading.status.replace("_", " ")
            color = "#30C8B4" if reading.status == "IN_TUNE" else self.NEEDLE
        elif reading:
            status = "NO SIGNAL" if reading.status == "NO_SIGNAL" else "AUDIO ERROR"
            color = "#264654" if reading.status == "NO_SIGNAL" else self.NEEDLE
        else:
            status, color = "WAITING", "#264654"

        rounded_rect(252, 347, 428, 383, 18, color)
        text(340, 365, status, 16,
             self.MUTED if color == "#264654" else self.BACKGROUND, "bold")
        text(30, 376, "FLAT", 13, self.ACCENT, anchor="w")
        text(650, 376, "SHARP", 13, self.ACCENT, anchor="e")
        if has_pitch:
            text(340, 410, f"Target {reading.target_hz:.2f} Hz", 12, self.MUTED)
        if self.connection == "Demo":
            text(650, 410, "Demo", 12, self.MUTED, anchor="e")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Serial device, for example /dev/cu.usbmodem123")
    parser.add_argument("--demo", action="store_true", help="Preview with a simulated A4 reading")
    args = parser.parse_args()

    if not args.demo:
        try:
            import serial
            from serial.tools import list_ports
        except ImportError:
            parser.exit(1, "Install pyserial with: python3 -m pip install pyserial\n")

    root = tk.Tk()
    window = TunerWindow(root)
    stopped = Event()
    events = Queue(maxsize=1)

    def publish(reading, connection):
        event = (reading, connection, time.monotonic())
        try:
            events.put_nowait(event)
        except Full:
            # Keep the newest record if rendering is slower than the UART.
            try:
                events.get_nowait()
            except Empty:
                pass
            events.put_nowait(event)

    def close():
        stopped.set()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", close)
    if args.demo:
        demo_hz = A4_HZ + 0.8
        demo_cents = 1200 * math.log2(demo_hz / A4_HZ)
        window.update(Reading(A4_HZ, "IN_TUNE", "A4", demo_hz, A4_HZ, demo_cents), "Demo")
    else:
        Thread(target=receive_serial,
               args=(serial, list_ports, args.port, publish, stopped), daemon=True).start()

        def poll():
            try:
                reading, connection, received_at = events.get_nowait()
                window.update(reading, connection)
                poll.last_reading_at = received_at if reading else None
            except Empty:
                if (poll.last_reading_at is not None
                        and time.monotonic() - poll.last_reading_at > READING_TIMEOUT):
                    window.update(None, "Waiting for fresh UART data")
                    poll.last_reading_at = None
            root.after(30, poll)

        poll.last_reading_at = None
        poll()
    root.mainloop()


if __name__ == "__main__":
    main()
