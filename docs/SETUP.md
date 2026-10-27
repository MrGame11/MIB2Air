# Setup guide

> [!WARNING]
> Experimental software, used entirely at your own risk. Read the
> [disclaimer in the README](../README.md) before you start.

## Before you start

- **Permanent Telnet and FTP access to the MIB.** Telnet is disabled on stock units.
  Install [mib-std2-pq-zr-toolbox](https://github.com/olli991/mib-std2-pq-zr-toolbox),
  then in its **Network** menu choose **Activate permanent telnet and console access**
  and reboot. This starts `inetd` at boot, which serves both Telnet and FTP; setup
  connects over both and logs in as `root`. Open remote root access is a risk:
  disable it in the same menu when you no longer need it.
- **A way back in if the unit stops responding over the network.** Keep one of these at hand:
  - a D-Link **DUB-E100** USB Ethernet adapter, to reach Telnet through the
    unit's USB port when Wi-Fi is unavailable;
  - a **USB-to-TTL serial adapter** (for example FT232RL) wired to the Quadlock
    connector behind the unit, for a direct root console. **Set the adapter's
    logic-level jumper to 5 V.**

    | Quadlock pin | Signal | USB-TTL adapter |
    | --- | --- | --- |
    | A11 | ESO C3_TX | RXD |
    | A12 | ESO C3_RX | TXD |
    | B7 | Mic1− (ground reference) | GND |

    Use 115200 baud, 8N1. Do not connect the adapter's VCC.

## Install

Start Docker, then launch the interactive setup from the repository root:

```sh
./setup.sh
```

Choose **Install**, **Remove**, **Diagnostics**, or **Collect stock data** from the menu
(arrow keys and Enter, or 1–4).
You can also skip the menu with `./setup.sh install`, `./setup.sh remove`, or
`./setup.sh diagnose`.
Setup prepares its Docker environment automatically, then asks for the MIB IPv4
address (the last one is remembered) and root password. The computer must be
able to reach the unit over Telnet and FTP. Inputs are captured from
your unit and compiled locally in Docker. Keep `local/`: it contains private
device files, installation backups and build logs. `local/cache/` keeps the large
originals of recognised firmware so later runs (updates, reinstalls) only download
them again if the unit's copy no longer matches.

Reboot the MIB manually when instructed. Enable its native Wi-Fi access point
in the stock menu, disconnect the phone's USB cable, disable the phone's
hotspot and connect Bluetooth to the vehicle. The runtime selects the first valid
inbound SPP phone connection and keeps that identity until session cleanup.
Native WPA2 personal (`Security=3`) on a /24 network is required. The runtime waits for the AP;
it does not enable it or change Wi-Fi modes. Turning the AP off stops wireless
AA and pauses connection attempts until it is enabled again.

## Update

Pull the new version of this repository and choose **Install** again. Setup checks the
installed version, rebuilds from the originals on the unit and compares the result:

- If nothing changed, it reports that the unit is already up to date and writes nothing.
- Otherwise it uploads the new version beside the running one and switches `current`
  as the final step. The new version starts after one manual reboot; no Bluetooth
  recovery or removal is needed. An interrupted update is completed or rolled back
  the next time you run Install or Remove.
- If the new version would change the startup or firewall blocks, or the unit's
  firmware has changed since installation, setup stops and asks you to run Remove,
  then Install.

## Remove

```sh
./setup.sh
```

Select **Remove** and follow the instructions. If Bluetooth
recovery is scheduled, reboot MIB, launch setup again and select the same option.
After removal, reboot once more to use the original services.

Removal uses the on-device journal and
backups and can resume after interruption or from another computer.

## Diagnostics

Run `./setup.sh` and select **Diagnostics**. Setup immediately saves one
snapshot; there is no observation timer. Collect after a problem and before rebooting,
while its system log is still available. Keep the computer connected to MIB during capture.

Results are saved in `local/debug-<timestamp>/`, with captures under `snapshot/`: full system logs, AA/network
events, processes, threads, memory, descriptors, interface/ARP state, effective
firewall rules and states, startup/AP configuration, executable SHA-256 hashes,
and installation/recovery state. Missing utilities retain their output and exit
status in the bundle. Large logs are transferred compressed using temporary files
that setup removes. Phone-side state and packet payloads require separate captures. `manifest.json`
records timestamps, checksums and missing data. An interruption preserves completed files.
No services are restarted and no configuration is changed. The AP summary omits credentials.

## Collect stock data

Choose **Collect stock data** or run `./setup.sh collect-stock`. This collects
WirelessAdapter, SAL, the GAL receiver library, QNX libc, startup, cpu.conf and
both firewall configurations, plus the formats a new profile must be checked
against: the native AP configuration with its SSID, key and any other unexpected
values replaced by `REDACTED`, the stored AP mode and the mount table. An existing Wireless AA installation must be removed first.
No AP configuration or supported firmware profile is required. Setup does not
install patches, change configuration, or reboot the unit during collection.

The ZIP is saved as `local/stock-<timestamp>/stock-data.zip`. If installation detects
an unknown WA/SAL pair or an unverified libc/GAL version, it stops and creates the
same archive inside that installation's local directory, reusing the captured files.
The install command still exits with status 1 because no installation was completed.
Other errors (connection failures, damaged transfers or invalid profiles) remain errors.

AP credentials are redacted before anything is written to disk; Bluetooth pairing data
and full system logs are not collected. The archive contains a manifest with paths,
attributes, sizes and SHA-256 hashes. A failed or interrupted capture keeps completed
local files, but does not create an incomplete ZIP.

## What changes on the MIB

Installation puts the WirelessBridge files in **`/tsd/var/wireless-aa/`**.
They start after the next manual reboot; setup does not restart the running
WA/SAL services during installation. Python, Docker and the installer source
remain on your computer.

Paths in this table are relative to `/tsd/var/wireless-aa/`:

| File or directory | Purpose |
| --- | --- |
| `releases/<id>/` | One installed version. After an update the previous version is kept beside it until the next update. |
| `releases/<id>/wirelessadapter.waa` | Patched copy of the unit's WirelessAdapter service, used for wireless AA. |
| `releases/<id>/sal.waa` | Patched copy of the unit's SAL service, used for wireless AA. |
| `releases/<id>/wirelessadapter.restore` | Helper that restores the Bluetooth service UUID configuration during removal. This is not a full firmware recovery image. |
| `releases/<id>/cpu.conf` | A copy of the unit's service configuration that launches WA and SAL through that version's `run.sh`. Other service commands and their arguments are preserved. |
| `releases/<id>/select.sh`, `run.sh` | Boot selection and service launch scripts. A supervisor retry after the patched service's first launch falls back to the original service. |
| `current` | The `<id>` of the version used at the next boot. Switching it is the last step of an update. |
| `install.json` | Installation/removal journal with hashes and original file attributes, used to verify and resume operations. |
| `backup/startup`, `backup/pf.conf`, `backup/pf_wlan.conf` | Copies of the three system files before installation, used during removal. |
| `mode` | Selects normal wireless AA operation or the removal/recovery sequence. |
| `run/` | Working directory. During operation/removal it holds communication pipes, one-use launch permits and recovery confirmation files. |

Setup also edits **three existing system files**, adding marked
`# BEGIN wireless-aa` / `# END wireless-aa` blocks:

| System file | Change |
| --- | --- |
| `/tsd/bin/system/startup` | During normal boot, reads `current` and calls that version's `select.sh` so the supervisor can use its `cpu.conf`. |
| `/tsd/etc/networking/pf.conf` | Allows incoming TCP connections to port **5288** on `wlan1`, from that interface's local subnet. |
| `/tsd/etc/networking/pf_wlan.conf` | Adds the same wireless AA firewall rule. |

The original `/tsd/etc/system/cpu.conf` stays in place unchanged. The original
service binaries at
`/tsd/bin/wirelessadapter/tsd.mibstd2.system.wirelessadapter.main` and
`/tsd/bin/sal/tsd.mibstd2.sal.main` are also preserved. QNX libc and the GAL
receiver library are read for compatibility checks and are not replaced.
The root filesystem is temporarily made writable as needed for the three
configuration edits, then its original mount settings are restored.

At boot, `/dev/shmem/wireless-aa.boot` prevents repeated boot selection within
the same boot. During use, WirelessBridge manages the Bluetooth service UUID
advertisement needed for wireless AA. It reads the native Wi-Fi AP settings;
it does not change the SSID/password or enable the AP for you.

Transfers and integrity checks may create temporary compressed files and a
SHA-256 helper under `/tsd/var/`, and diagnostic captures use temporary files
under `/dev/shmem/`. Setup normally removes these temporary files; an interrupted
connection can leave some behind.

**Use Remove rather than deleting `/tsd/var/wireless-aa/` manually.** Removal
uses the journal and backups, performs Bluetooth UUID recovery when required,
removes the added startup/firewall blocks and deletes the installation directory.
Unrelated edits outside those blocks are preserved; changed or missing blocks
cause removal to stop for inspection. Reboot when instructed to return to the
original services. Captures and backups in your computer's `local/` remain available.
