#!/usr/bin/env python3
"""Install, remove, diagnose or collect stock data for Wireless Android Auto."""

import argparse
import datetime
import getpass
import ipaddress
import os

from installer.device import Device
from installer.diagnostics import collect
from installer.interface import (
    ACTIONS,
    choose_action,
    connection_header,
    show_error,
    show_interrupted,
    show_result,
    show_saved,
)
from installer.paths import ROOT
from installer.progress import reporting, step, summary
from installer.stock import collect as collect_stock
from installer.workflow import install, remove

LAST_HOST = ROOT / "local" / "last-host"


def remembered_host():
    try:
        return str(ipaddress.IPv4Address(LAST_HOST.read_text().strip()))
    except (OSError, ValueError):
        return None


def ask_host():
    last = remembered_host()
    prompt = f" MIB address [{last}]: " if last else " MIB address: "
    while True:
        value = input(prompt).strip() or (last or "")
        try:
            return str(ipaddress.IPv4Address(value))
        except ValueError:
            print(" Enter an IPv4 address, for example 192.0.2.1.")


def remember_host(host):
    try:
        LAST_HOST.write_text(host + "\n")
    except OSError:
        pass


def main():
    parser = argparse.ArgumentParser(
        prog="./setup.sh",
        description="Wireless Android Auto: install, remove, diagnose and collect stock data",
    )
    parser.add_argument(
        "action",
        nargs="?",
        choices=tuple(action for action, _, _ in ACTIONS),
        help="Skip the menu and start this action",
    )
    args = parser.parse_args()
    action = args.action or choose_action()
    if action is None:
        return
    os.umask(0o077)
    connection_header(action)
    host = ask_host()
    password = getpass.getpass(" Root password: ")
    print()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    # Captures and compiler outputs must live in the persistent private volume.
    prefix = {"diagnose": "debug-", "collect-stock": "stock-"}.get(action, "")
    directory = ROOT / "local" / (prefix + stamp)
    os.environ["WAA_WORKDIR"] = str(directory)
    try:
        with reporting():
            step("Connect to MIB")
            try:
                device = Device(host, password)
            finally:
                del password
            summary(host)
            remember_host(host)
            try:
                operation = {
                    "install": install,
                    "remove": remove,
                    "diagnose": collect,
                    "collect-stock": collect_stock,
                }[action]
                result = operation(device, directory)
            finally:
                device.close()
        show_result(result)
        return 1 if result.status == "unsupported_firmware" else 0
    finally:
        if directory.exists():
            logs = ["build.log"] if (directory / "build.log").exists() else []
            show_saved(directory, ROOT, logs)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, RuntimeError, OSError) as error:
        show_error(error)
        raise SystemExit(1)
    except (KeyboardInterrupt, EOFError):
        show_interrupted()
        raise SystemExit(130)
