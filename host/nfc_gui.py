#!/usr/bin/env python3
"""Tk GUI for the ESP32 NFC PC-BRIDGE protocol."""

import argparse
import os
import queue
import threading
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from serial.tools import list_ports

from nfc_host import find_port, open_serial, send
from write_from_dump import is_trailer, parse_dump


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DUMP_DIR = Path(os.environ.get("NFC_DUMP_DIR", ROOT / "dumps")).expanduser()


class NfcGui(tk.Tk):
    def __init__(self, mode="bridge"):
        super().__init__()
        self.title("ESP32 NFC Bridge")
        self.geometry("760x560")
        self.minsize(650, 470)
        self.events = queue.Queue()
        self.busy = False
        self.port = tk.StringVar()
        self.status = tk.StringVar(value="Select PC BRIDGE (USB) on the ESP32 OLED first.")
        self.write_block0 = tk.BooleanVar(value=False)
        self.write_trailers = tk.BooleanVar(value=False)
        self._build_ui()
        self.refresh_ports()
        self.after(80, self._poll_events)
        if mode == "dump":
            self.after(250, self.dump_to_file)
        elif mode == "write":
            self.after(250, self.write_from_file)

    def _build_ui(self):
        top = ttk.Frame(self, padding=12)
        top.pack(fill="x")
        ttk.Label(top, text="Serial port:").pack(side="left")
        self.port_box = ttk.Combobox(top, textvariable=self.port, width=28, state="readonly")
        self.port_box.pack(side="left", padx=8)
        ttk.Button(top, text="Refresh", command=self.refresh_ports).pack(side="left")
        ttk.Button(top, text="Test connection", command=self.ping).pack(side="left", padx=8)

        actions = ttk.LabelFrame(self, text="Card actions", padding=12)
        actions.pack(fill="x", padx=12, pady=(0, 10))
        ttk.Button(actions, text="Read UID", command=self.read_uid).grid(row=0, column=0, padx=5, pady=5)
        ttk.Button(actions, text="Card info", command=self.card_info).grid(row=0, column=1, padx=5, pady=5)
        ttk.Button(actions, text="Dump to file…", command=self.dump_to_file).grid(row=0, column=2, padx=5, pady=5)
        ttk.Button(actions, text="Write from dump…", command=self.write_from_file).grid(row=0, column=3, padx=5, pady=5)
        ttk.Checkbutton(actions, text="Include block 0 (Gen2 only)", variable=self.write_block0).grid(row=1, column=0, columnspan=2, sticky="w", padx=5)
        ttk.Checkbutton(actions, text="Include sector trailers / keys", variable=self.write_trailers).grid(row=1, column=2, columnspan=2, sticky="w", padx=5)

        ttk.Label(self, textvariable=self.status, padding=(12, 2)).pack(fill="x")
        log_frame = ttk.Frame(self, padding=(12, 4, 12, 12))
        log_frame.pack(fill="both", expand=True)
        self.log = tk.Text(log_frame, wrap="word", state="disabled", font=("monospace", 10))
        scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        self.log.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")

    def _append(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text.rstrip() + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def refresh_ports(self):
        ports = [p.device for p in list_ports.comports() if "ttyUSB" in p.device or "ttyACM" in p.device]
        self.port_box["values"] = ports
        preferred = find_port()
        if preferred in ports:
            self.port.set(preferred)
        elif ports and self.port.get() not in ports:
            self.port.set(ports[0])
        elif not ports:
            self.port.set("")
        self.status.set(f"Found {len(ports)} USB serial port(s)." if ports else "No ESP32 USB serial port found.")

    def _run(self, label, fn):
        if self.busy:
            messagebox.showinfo("ESP32 NFC", "Another NFC operation is still running.")
            return
        if not self.port.get():
            messagebox.showerror("ESP32 NFC", "No USB serial port is selected.")
            return
        self.busy = True
        self.status.set(label)

        def worker():
            try:
                result = fn()
                self.events.put(("ok", result))
            except Exception as exc:
                self.events.put(("error", str(exc)))

        threading.Thread(target=worker, daemon=True).start()

    def _poll_events(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                self.busy = False
                if kind == "error":
                    self.status.set("Operation failed.")
                    self._append(f"ERROR: {value}")
                    messagebox.showerror("ESP32 NFC", value)
                else:
                    self.status.set("Ready.")
                    if value:
                        self._append(value)
        except queue.Empty:
            pass
        self.after(80, self._poll_events)

    def _command(self, command, timeout=None):
        ser = open_serial(self.port.get())
        try:
            lines = send(ser, command, timeout=timeout)
        finally:
            ser.close()
        if not lines:
            raise RuntimeError("No response. Select PC BRIDGE (USB) on the OLED and close any serial monitor.")
        if lines[0].startswith("ERR"):
            raise RuntimeError("\n".join(lines))
        return lines

    def ping(self):
        self._run("Testing connection…", lambda: "\n".join(self._command("PING")))

    def read_uid(self):
        self._run("Hold a card on the RC522…", lambda: "\n".join(self._command("UID")))

    def card_info(self):
        self._run("Hold a card on the RC522…", lambda: "\n".join(self._command("INFO")))

    def dump_to_file(self):
        DEFAULT_DUMP_DIR.mkdir(parents=True, exist_ok=True)
        suggested = DEFAULT_DUMP_DIR / f"card-{datetime.now():%Y%m%d-%H%M%S}.dump"
        path = filedialog.asksaveasfilename(
            title="Save NFC dump",
            initialdir=str(DEFAULT_DUMP_DIR),
            initialfile=suggested.name,
            defaultextension=".dump",
            filetypes=[("NFC dump", "*.dump"), ("All files", "*")],
        )
        if not path:
            return

        def work():
            lines = self._command("DUMP", timeout=120)
            Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")
            return f"Saved {len(lines)} lines to:\n{path}"

        self._run("Reading card and trying known keys…", work)

    def write_from_file(self):
        DEFAULT_DUMP_DIR.mkdir(parents=True, exist_ok=True)
        path = filedialog.askopenfilename(
            title="Choose NFC dump",
            initialdir=str(DEFAULT_DUMP_DIR),
            filetypes=[("NFC dump", "*.dump"), ("All files", "*")],
        )
        if not path:
            return
        blocks, keys = parse_dump(path)
        if not blocks:
            messagebox.showerror("ESP32 NFC", "The selected file contains no valid card blocks.")
            return
        include_block0 = self.write_block0.get()
        include_trailers = self.write_trailers.get()
        if include_trailers:
            for block in list(blocks):
                if is_trailer(block):
                    sector = block // 4 if block < 128 else 32 + (block - 128) // 16
                    if sector in keys:
                        blocks[block] = keys[sector][1] + blocks[block][12:]
        targets = [b for b in sorted(blocks)
                   if (b != 0 or include_block0) and (not is_trailer(b) or include_trailers)]
        warning = f"Write {len(targets)} blocks from\n{path}\n\nto the card currently on the RC522?"
        if include_block0 or include_trailers:
            warning += "\n\nBlock 0 and/or card keys are enabled. Use only a compatible blank card."
        if not messagebox.askyesno("Confirm card write", warning, icon="warning"):
            return

        def work():
            ser = open_serial(self.port.get())
            ok = fail = 0
            output = []
            try:
                pong = send(ser, "PING")
                if not any("PONG" in line for line in pong):
                    raise RuntimeError("ESP32 is not in PC BRIDGE (USB) mode.")
                for block in targets:
                    response = send(ser, f"WBLK {block} {blocks[block]}")
                    line = response[0] if response else "ERR NO RESPONSE"
                    output.append(f"Block {block}: {line}")
                    if line == "OK":
                        ok += 1
                    else:
                        fail += 1
            finally:
                ser.close()
            output.append(f"Finished: {ok} OK, {fail} failed")
            return "\n".join(output)

        self._run("Writing card… keep it on the RC522.", work)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("bridge", "dump", "write"), default="bridge")
    args = parser.parse_args()
    NfcGui(args.mode).mainloop()


if __name__ == "__main__":
    main()
