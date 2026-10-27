# MIB STD2 WirelessBridge

> [!WARNING]
> **Experimental software. Use entirely at your own risk.** WirelessBridge patches
> and runs modified copies of your head unit's own services and edits its startup and
> firewall configuration. It has had limited hardware testing and may leave the unit
> misbehaving or require recovery. It is provided as is, without warranty of any kind,
> and the authors accept no responsibility or liability for any damage to your unit,
> vehicle, phone or data, or for any other consequence of using it. Only proceed if
> you have a working recovery method and backups.

Wireless Android Auto for MIB2 STD2 (MST2) head units, built on the unit's original
WirelessAdapter and SAL services. Setup runs on your computer in Docker, patches copies
of those services for your exact firmware and installs them beside the originals.
Unknown firmware is never patched.

## Supported firmware

| Firmware | Variant | Status |
| --- | --- | --- |
| P0480T | cpu | Supported |

Other versions are refused. To ask for one, collect stock data from your unit as
described in [Adding a firmware version](docs/DEVELOPMENT.md#adding-a-firmware-version).

## Requirements

- Docker on your computer.
- Permanent Telnet and FTP access to the MIB, enabled with
  [mib-std2-pq-zr-toolbox](https://github.com/olli991/mib-std2-pq-zr-toolbox).
- A recovery path if the unit stops responding over the network: a DUB-E100 USB
  Ethernet adapter or a USB-to-TTL serial adapter on the Quadlock connector.

Details and wiring are in the [setup guide](docs/SETUP.md#before-you-start).

## Usage

```sh
./setup.sh
```

Choose an action and follow the prompts. Install, update, removal, diagnostics and
stock-data collection are explained in the [setup guide](docs/SETUP.md).

## Documentation

- [Setup guide](docs/SETUP.md): preparation, every setup action and what changes on the unit.
- [Development](docs/DEVELOPMENT.md): repository layout, offline builds and adding firmware.

## License

[MIT](LICENSE). Protocol references are credited in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

This project is not affiliated with or endorsed by Volkswagen Group, Google or the
head unit manufacturers.
