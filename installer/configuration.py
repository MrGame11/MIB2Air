"""Small reversible edits to the actual device configuration, never OEM templates."""

import re
from ipaddress import IPv4Address

from installer.elf import require
from installer.paths import BASE, CURRENT, RELEASE_ID, RELEASES, REMOTE, release_dir

BEGIN = b"# BEGIN wireless-aa\n"
END = b"# END wireless-aa\n"
# The release is read once; only that release's own cpu.conf is accepted.
START_BLOCK = (
    BEGIN
    + (
        '  if [ "$CFG" = /tsd/etc/system/cpu.conf ]; then\n'
        "    WAA_REL=$(cat " + CURRENT + " 2>/dev/null)\n"
        '    case "$WAA_REL" in\n'
        "      *[!0-9a-f]*) ;;\n"
        "      " + "?" * 16 + ")\n"
        "        WAA_CFG=$(/bin/sh " + RELEASES + '/$WAA_REL/select.sh "$WAA_REL")\n'
        '        case "$WAA_CFG" in\n'
        "          " + RELEASES + "/$WAA_REL/cpu.conf) CFG=$WAA_CFG ;;\n"
        "        esac\n"
        "        ;;\n"
        "    esac\n"
        "  fi\n"
    ).encode()
    + END
)
PF_RULE = "pass in quick on wlan1 inet proto tcp from (wlan1:network) to (wlan1) port 5288 flags S/SA keep state"
PF_BLOCK = BEGIN + PF_RULE.encode() + b"\n" + END


def clean(data):
    require(b"\x00" not in data and b"\r" not in data, "Unsupported config encoding")
    require(
        b"wireless-aa" not in data,
        "Existing Wireless AA configuration; remove it first",
    )


def startup(data):
    clean(data)
    require(
        len(re.findall(rb"(?m)^[ \t]*CFG=/tsd/etc/system/cpu\.conf[ \t]*$", data)) == 1,
        "Expected one normal CPU configuration assignment",
    )
    lines = data.splitlines(keepends=True)
    # Accept only this shell grammar; do not interpret arbitrary shell text.
    command = re.compile(
        rb"[ \t]+if[ \t]+on[ \t]+-p(?P<priority>[0-9]+)[ \t]+"
        rb"/tsd/bin/root/tsd\.mibstd2\.cpu\.root[ \t]+"
        rb'-main=(?P<main>[0-9.]+)[ \t]+"-file=\$\{CFG\}"[ \t]+'
        rb"-swdlfile=/tsd/etc/system/swdl/cpu_swdl\.conf[ \t]+"
        rb"-reset=/tsd/var/root/reset\.count\.cpu[ \t]+;[ \t]+then[ \t]*\n"
    )
    matches = []
    for i, line in enumerate(lines):
        match = command.fullmatch(line)
        if match:
            try:
                IPv4Address(match["main"].decode("ascii"))
            except ValueError:
                continue
            if 1 <= int(match["priority"]) <= 255:
                matches.append(i)
    require(len(matches) == 1, "Unsupported CPU supervisor startup command")
    # Refuse an additional, differently formatted invocation rather than patching partly.
    active = [line for line in lines if not line.lstrip().startswith(b"#")]
    require(
        b"".join(active).count(b"/tsd/bin/root/tsd.mibstd2.cpu.root") == 1,
        "Ambiguous CPU supervisor startup command",
    )
    lines.insert(matches[0], START_BLOCK)
    return b"".join(lines)


def cpu(data, release):
    require(re.fullmatch(RELEASE_ID, release) is not None, "Invalid release id")
    clean(data)
    run = (release_dir(release) + "/run.sh ").encode()
    for role in ("wa", "sal"):
        pattern = (
            rb"(?m)^([ \t]*command[ \t]+)"
            + re.escape(REMOTE[role].encode())
            + rb"(?=[ \t\n]|$)"
        )
        data, count = re.subn(
            pattern,
            lambda m: m[1] + b"/bin/sh " + run + role.encode(),
            data,
        )
        require(count == 1, "Expected one CPU command for " + role)
    return data


def firewall(data):
    clean(data)
    require(PF_RULE.encode() not in data, "Unowned Wireless AA firewall rule")
    anchor = b"anchor port_rules\n"
    require(data.splitlines(keepends=True).count(anchor) == 1, "Unsupported PF anchor")
    return data.replace(anchor, PF_BLOCK + anchor, 1)


def undo(name, current, original):
    """Keep independent edits outside our exact block; reject edited/missing blocks."""
    if current == original:
        return original
    block = START_BLOCK if name == "startup" else PF_BLOCK
    require(
        current.count(BEGIN) == 1
        and current.count(END) == 1
        and current.count(block) == 1,
        "Configuration changed inside our block: " + name,
    )
    result = current.replace(block, b"", 1)
    require(
        BASE.encode() not in result and b"wireless-aa" not in result,
        "Unrecognized remaining references: " + name,
    )
    return result
