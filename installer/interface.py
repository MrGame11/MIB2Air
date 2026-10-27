"""Inline keyboard menu, header and outcome lines, with plain prompts without a terminal."""

import os
import select
import sys
import termios
import tty

from rich.live import Live
from rich.padding import Padding
from rich.text import Text

from installer.progress import ACCENT, DIM, ERROR, OK, WARN, fit, terminal, width

ACTIONS = (
    ("install", "Install", "Patch WA and SAL, verify, enable on next boot"),
    ("remove", "Remove", "Restore originals or finish a removal"),
    ("diagnose", "Diagnostics", "Save logs and state for troubleshooting"),
    ("collect-stock", "Collect stock data", "Save files for a new firmware profile"),
)
NAMES = {action: label.lower() for action, label, _ in ACTIONS}
AFTER_INSTALL = (
    "Enable the MIB Wi-Fi access point in the stock menu (WPA2 personal)",
    "Unplug the phone's USB cable and turn off the phone's hotspot",
    "Connect the phone to the car over Bluetooth",
)


def header(action):
    line = Text(" ", no_wrap=True)
    line.append("wirelessbridge", "bold")
    line.append(" › ", DIM)
    line.append(action)
    return line


def menu_frame(selected, total, marker=True):
    rows = [header("setup"), Text(""), Text(" What do you want to do?"), Text("")]
    for index, (_, label, description) in enumerate(ACTIONS):
        chosen = marker and index == selected
        line = Text(" ", no_wrap=True)
        line.append("›" if chosen else " ", ACCENT)
        line.append(f" {index + 1}  ", DIM)
        line.append_text(fit(label, 20, "bold" if chosen else ""))
        line.append_text(fit(description, total - 26, "" if chosen else DIM))
        rows.append(line)
    rows.append(Text(""))
    if marker:
        rows.append(
            Text(
                f" ↑↓ select · enter start · 1–{len(ACTIONS)} jump · esc quit",
                style=DIM,
            )
        )
    return Padding(Text("\n").join(rows), (1, 0, 0, 0))


def read_key(fd):
    key = os.read(fd, 1)
    if key == b"\x1b":
        while select.select([fd], [], [], 0.06)[0]:
            key += os.read(fd, 1)
            if key.endswith((b"A", b"B", b"H", b"F", b"~")):
                break
    return key


def choose_action():
    console = terminal()
    choices = tuple(str(i) for i in range(1, len(ACTIONS) + 1))
    if not (
        console.is_terminal and sys.stdin.isatty() and not console.is_dumb_terminal
    ):
        console.print(menu_frame(0, width(console), marker=False))
        while True:
            choice = (
                input("Choose [" + "/".join((*choices, "0")) + "]: ").strip().lower()
            )
            if choice in ("0", "q", "quit"):
                return None
            if choice in choices:
                return ACTIONS[int(choice) - 1][0]
            print("Choose a listed number, or 0 to exit.")
    fd = sys.stdin.fileno()
    original = termios.tcgetattr(fd)
    selected = 0
    try:
        tty.setcbreak(fd)
        with Live(
            menu_frame(selected, width(console)),
            console=console,
            auto_refresh=False,
            transient=True,
        ) as live:
            while True:
                key = read_key(fd)
                if key in (b"", b"0", b"q", b"Q", b"\x1b", b"\x04"):
                    return None
                if key == b"\x03":
                    raise KeyboardInterrupt
                if key in tuple(choice.encode() for choice in choices):
                    return ACTIONS[int(key) - 1][0]
                if key in (b"\r", b"\n"):
                    return ACTIONS[selected][0]
                if key in (b"\x1b[A", b"\x1bOA", b"k"):
                    selected = (selected - 1) % len(ACTIONS)
                elif key in (b"\x1b[B", b"\x1bOB", b"j"):
                    selected = (selected + 1) % len(ACTIONS)
                elif key in (b"\x1b[H", b"\x1bOH"):
                    selected = 0
                elif key in (b"\x1b[F", b"\x1bOF"):
                    selected = len(ACTIONS) - 1
                live.update(menu_frame(selected, width(console)), refresh=True)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, original)


def connection_header(action):
    console = terminal()
    console.print()
    console.print(header(NAMES[action]))
    console.print()
    paragraph(
        console,
        "Needs permanent Telnet and FTP on the MIB (mib-std2-pq-zr-toolbox).",
        indent=1,
        style=DIM,
    )
    console.print()


def paragraph(console, text, indent=3, style=""):
    console.print(
        Padding(Text(text, style=style), (0, 0, 0, indent), expand=False),
        width=width(console),
    )


def outcome(glyph, style, headline):
    line = Text("\n ")
    line.append(glyph, style)
    line.append(" ")
    line.append(headline, "bold")
    return line


def show_result(result):
    console = terminal()
    warning = result.status in (
        "recovery_pending",
        "diagnostics_partial",
        "unsupported_firmware",
    )
    console.print(
        outcome("!" if warning else "✓", WARN if warning else OK, result.headline)
    )
    for text in result.paragraphs:
        paragraph(console, text)
    if result.status == "installed":
        console.print(Text("\n After the reboot", style="bold"))
        for number, text in enumerate(AFTER_INSTALL, 1):
            line = Text("   ")
            line.append(str(number), ACCENT)
            line.append("  " + text)
            console.print(line, width=width(console))


def show_saved(directory, root, logs=()):
    console = terminal()
    total = width(console)
    line = Text("\n ")
    line.append("Saved   ", DIM)
    line.append_text(fit(str(directory.relative_to(root)) + "/", total - 9))
    console.print(line, no_wrap=True)
    if logs:
        console.print(" " * 9 + " · ".join(logs), style=DIM, width=total)


def show_error(error):
    console = terminal(stderr=True)
    console.print(outcome("✗", ERROR, str(error)))


def show_interrupted():
    console = terminal(stderr=True)
    console.print(outcome("!", WARN, "Operation interrupted."))
    paragraph(console, "Installation or removal may be incomplete.", style=DIM)
