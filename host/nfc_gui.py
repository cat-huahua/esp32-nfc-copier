#!/usr/bin/env python3
"""Tk GUI for the ESP32 NFC PC-BRIDGE protocol."""

import argparse
import json
import os
import queue
import threading
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

from serial.tools import list_ports

from nfc_host import find_port, open_bridge, send
from write_from_dump import is_trailer, parse_dump


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DUMP_DIR = Path(os.environ.get("NFC_DUMP_DIR", "~/Dumps")).expanduser()
CONFIG_FILE = Path("~/.config/esp32-nfc-copier/settings.json").expanduser()


class NfcGui(tk.Tk):
    def __init__(self, mode="bridge"):
        super().__init__()
        self.title("ESP32 NFC Bridge")
        self.geometry("1100x760")
        self.minsize(900, 650)
        self.events = queue.Queue()
        self.busy = False
        self.ser = None
        self.port = tk.StringVar()
        self.status = tk.StringVar(value="Select PC BRIDGE (USB) on the ESP32 OLED first.")
        settings = self._load_settings()
        self.dump_dir = tk.StringVar(value=settings.get("dump_dir", str(DEFAULT_DUMP_DIR)))
        self.write_block0 = tk.BooleanVar(value=False)
        self.write_trailers = tk.BooleanVar(value=False)
        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.port_box.bind("<<ComboboxSelected>>", lambda _event: self._port_changed())
        self.refresh_ports()
        self.after(80, self._poll_events)
        # Connect once at startup.  Opening FT232 may reset the ESP32 once, but
        # subsequent action buttons reuse this connection and do not reset it.
        self.after(450, self.ping)
        if mode == "dump":
            self.notebook.select(1)
        elif mode == "write":
            self.notebook.select(2)

    def _build_ui(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure(".", font=("Sans", 11))
        style.configure("TFrame", background="#f4f6fa")
        style.configure("Card.TFrame", background="#ffffff")
        style.configure("Header.TLabel", background="#173b65", foreground="white", font=("Sans", 28, "bold"))
        style.configure("SubHeader.TLabel", background="#173b65", foreground="#c9dbef", font=("Sans", 12))
        style.configure("Title.TLabel", background="#ffffff", foreground="#172033", font=("Sans", 17, "bold"))
        style.configure("Primary.TButton", font=("Sans", 12, "bold"), padding=(22, 13))
        style.configure("TButton", padding=(12, 8))
        style.configure("TCheckbutton", font=("Sans", 11), padding=(2, 4))
        style.configure("TNotebook", background="#f4f6fa", borderwidth=0)
        style.configure("TNotebook.Tab", padding=(26, 12), font=("Sans", 12, "bold"))
        self.configure(background="#f4f6fa")

        header = tk.Frame(self, bg="#173b65", height=116)
        header.pack(fill="x")
        header.pack_propagate(False)
        ttk.Label(header, text="ESP32 NFC Bridge", style="Header.TLabel").pack(anchor="w", padx=28, pady=(18, 0))
        ttk.Label(header, text="RC522 reader • Linux USB bridge • MIFARE Classic", style="SubHeader.TLabel").pack(anchor="w", padx=29)

        connection = ttk.Frame(self, style="Card.TFrame", padding=18)
        connection.pack(fill="x", padx=24, pady=(18, 12))
        ttk.Label(connection, text="Connection", style="Title.TLabel").pack(side="left", padx=(0, 18))
        self.port_box = ttk.Combobox(connection, textvariable=self.port, width=30, state="readonly", font=("Sans", 11))
        self.port_box.pack(side="left", padx=(0, 8))
        self.port_box.bind("<<ComboboxSelected>>", lambda _event: self._port_changed())
        ttk.Button(connection, text="Refresh", command=self.refresh_ports).pack(side="left")
        self.test_button = ttk.Button(connection, text="Connect", command=self.ping)
        self.test_button.pack(side="left", padx=8)
        self.connection_badge = tk.Label(connection, text="●  Not tested", bg="#ffffff", fg="#7a8491", font=("Sans", 12, "bold"))
        self.connection_badge.pack(side="right")

        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill="x", padx=24)
        reader = ttk.Frame(self.notebook, style="Card.TFrame", padding=22)
        dump = ttk.Frame(self.notebook, style="Card.TFrame", padding=22)
        write = ttk.Frame(self.notebook, style="Card.TFrame", padding=22)
        settings = ttk.Frame(self.notebook, style="Card.TFrame", padding=22)
        self.notebook.add(reader, text="Reader")
        self.notebook.add(dump, text="Dump")
        self.notebook.add(write, text="Write back")
        self.notebook.add(settings, text="Settings")

        ttk.Label(reader, text="Read a card", style="Title.TLabel").pack(anchor="w")
        ttk.Label(reader, text="Keep the card flat against the RC522 antenna while reading.", background="#ffffff").pack(anchor="w", pady=(2, 12))
        row = ttk.Frame(reader, style="Card.TFrame")
        row.pack(anchor="w")
        ttk.Button(row, text="Read UID", style="Primary.TButton", command=self.read_uid).pack(side="left", padx=(0, 8))
        ttk.Button(row, text="Card information", command=self.card_info).pack(side="left")

        ttk.Label(dump, text="Create a card dump", style="Title.TLabel").pack(anchor="w")
        ttk.Label(dump, text="Reads known sectors, discovered keys and block data into one validated file.", background="#ffffff").pack(anchor="w", pady=(2, 12))
        ttk.Button(dump, text="Choose destination and start…", style="Primary.TButton", command=self.dump_to_file).pack(anchor="w")

        ttk.Label(write, text="Write from a dump", style="Title.TLabel").pack(anchor="w")
        ttk.Label(write, text="Safe defaults skip UID block 0 and sector key trailers.", background="#ffffff").pack(anchor="w", pady=(2, 8))
        ttk.Checkbutton(write, text="Include block 0 (Gen2 magic card only)", variable=self.write_block0).pack(anchor="w")
        ttk.Checkbutton(write, text="Include sector trailers and keys", variable=self.write_trailers).pack(anchor="w")
        ttk.Button(write, text="Choose dump and write…", style="Primary.TButton", command=self.write_from_file).pack(anchor="w", pady=(10, 0))

        ttk.Label(settings, text="Application settings", style="Title.TLabel").pack(anchor="w")
        ttk.Label(settings, text="Default folder used by Dump and Write Back file dialogs.", background="#ffffff").pack(anchor="w", pady=(2, 12))
        path_row = ttk.Frame(settings, style="Card.TFrame")
        path_row.pack(fill="x")
        self.dump_dir_entry = ttk.Entry(path_row, textvariable=self.dump_dir, font=("Sans", 11))
        self.dump_dir_entry.pack(side="left", fill="x", expand=True, padx=(0, 10), ipady=5)
        ttk.Button(path_row, text="Choose folder…", command=self._choose_dump_dir).pack(side="left")
        settings_buttons = ttk.Frame(settings, style="Card.TFrame")
        settings_buttons.pack(anchor="w", pady=(12, 0))
        ttk.Button(settings_buttons, text="Save settings", style="Primary.TButton", command=self._save_settings).pack(side="left", padx=(0, 8))
        ttk.Button(settings_buttons, text="Reset to ~/Dumps", command=self._reset_dump_dir).pack(side="left")

        status_row = ttk.Frame(self, padding=(24, 14, 24, 4))
        status_row.pack(fill="x")
        ttk.Label(status_row, textvariable=self.status).pack(side="left")
        self.progress = ttk.Progressbar(status_row, mode="indeterminate", length=180)
        self.progress.pack(side="right")

        log_frame = ttk.LabelFrame(self, text="Activity", padding=(10, 8))
        log_frame.pack(fill="both", expand=True, padx=24, pady=(4, 20))
        log_frame.pack(fill="both", expand=True)
        self.log = tk.Text(log_frame, wrap="word", state="disabled", font=("monospace", 11),
                           bg="#101722", fg="#d8e2ef", insertbackground="white", relief="flat", padx=10, pady=8)
        scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        self.log.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")

    def _append(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text.rstrip() + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _load_settings(self):
        try:
            data = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def _save_settings(self):
        path = Path(self.dump_dir.get()).expanduser()
        try:
            path.mkdir(parents=True, exist_ok=True)
            CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
            CONFIG_FILE.write_text(json.dumps({"dump_dir": str(path)}, indent=2) + "\n", encoding="utf-8")
        except OSError as exc:
            messagebox.showerror("ESP32 NFC", f"Could not save settings:\n{exc}")
            return
        self.dump_dir.set(str(path))
        self.status.set(f"Default dump folder saved: {path}")
        self._append(f"Settings saved: dump folder = {path}")

    def _choose_dump_dir(self):
        initial = Path(self.dump_dir.get()).expanduser()
        chosen = filedialog.askdirectory(title="Choose default dump folder", initialdir=str(initial if initial.exists() else Path.home()))
        if chosen:
            self.dump_dir.set(chosen)

    def _reset_dump_dir(self):
        self.dump_dir.set(str(Path("~/Dumps").expanduser()))
        self._save_settings()

    def _current_dump_dir(self):
        path = Path(self.dump_dir.get()).expanduser()
        path.mkdir(parents=True, exist_ok=True)
        return path

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

    def _disconnect(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def _port_changed(self):
        self._disconnect()
        self.connection_badge.configure(text="●  Not connected", fg="#7a8491")

    def _on_close(self):
        self._disconnect()
        self.destroy()

    def _ensure_connection(self):
        if self.ser is not None and self.ser.is_open:
            return self.ser
        self.ser = open_bridge(self.port.get())
        return self.ser

    def _run(self, label, fn):
        if self.busy:
            messagebox.showinfo("ESP32 NFC", "Another NFC operation is still running.")
            return
        if not self.port.get():
            messagebox.showerror("ESP32 NFC", "No USB serial port is selected.")
            return
        self.busy = True
        self.status.set(label)
        self.progress.start(12)

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
                self.progress.stop()
                if kind == "error":
                    self.status.set("Operation failed.")
                    self.connection_badge.configure(text="●  Error", fg="#b42318")
                    self._append(f"ERROR: {value}")
                    messagebox.showerror("ESP32 NFC", value)
                else:
                    self.status.set("Ready.")
                    self.connection_badge.configure(text="●  Connected", fg="#16834a")
                    if value:
                        self._append(value)
        except queue.Empty:
            pass
        self.after(80, self._poll_events)

    def _command(self, command, timeout=None):
        try:
            lines = send(self._ensure_connection(), command, timeout=timeout)
        except Exception:
            self._disconnect()
            raise
        if not lines:
            raise RuntimeError("No response. Select PC BRIDGE (USB) on the OLED and close any serial monitor.")
        if lines[0].startswith("ERR"):
            raise RuntimeError("\n".join(lines))
        return lines

    def ping(self):
        self._run("Testing connection…", lambda: "Connected: " + "\n".join(self._command("PING")))

    def read_uid(self):
        self._run("Hold a card on the RC522…", lambda: "\n".join(self._command("UID")))

    def card_info(self):
        self._run("Hold a card on the RC522…", lambda: "\n".join(self._command("INFO")))

    def dump_to_file(self):
        dump_dir = self._current_dump_dir()
        suggested = dump_dir / f"card-{datetime.now():%Y%m%d-%H%M%S}.dump"
        path = filedialog.asksaveasfilename(
            title="Save NFC dump",
            initialdir=str(dump_dir),
            initialfile=suggested.name,
            defaultextension=".dump",
            filetypes=[("NFC dump", "*.dump"), ("All files", "*")],
        )
        if not path:
            return

        def work():
            lines = self._command("DUMP", timeout=120)
            if lines[-1] != "DONE":
                raise RuntimeError("Incomplete dump: ESP32 did not send DONE. No file was saved.")
            if not any(line.startswith("UID ") for line in lines):
                raise RuntimeError("Invalid dump: UID is missing. No file was saved.")
            block_count = sum(line.startswith("BLK ") for line in lines)
            if block_count == 0:
                raise RuntimeError("Invalid dump: no card blocks were received. No file was saved.")
            Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")
            return f"Dump complete: {block_count} blocks\nSaved to: {path}"

        self._run("Reading card and trying known keys…", work)

    def write_from_file(self):
        dump_dir = self._current_dump_dir()
        path = filedialog.askopenfilename(
            title="Choose NFC dump",
            initialdir=str(dump_dir),
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
            ok = fail = 0
            output = []
            try:
                ser = self._ensure_connection()
                for block in targets:
                    response = send(ser, f"WBLK {block} {blocks[block]}")
                    line = response[0] if response else "ERR NO RESPONSE"
                    output.append(f"Block {block}: {line}")
                    if line == "OK":
                        ok += 1
                    else:
                        fail += 1
            except Exception:
                self._disconnect()
                raise
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
