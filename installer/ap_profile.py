"""Read the native AP XML without logging credentials."""

import re
import xml.etree.ElementTree as ET

AP_PATH = "/tsd/var/networking/wlanapconfig.xml"
MODE_PATH = "/tsd/var/networking/wlanlastmode.conf"
# WlanRole values as NetMgr logs them; the runtime needs the access point (1 or 3).
ROLE_AP, ROLE_CLIENT, ROLE_COMBINED = 1, 2, 3
COMBINED_WARNING = (
    "MIB Wi-Fi is set to access point + client. While its client network is out of "
    "range, the MIB leaves the access point's channel every 30 s to look for it, "
    "and the phone drops Android Auto. Switch MIB Wi-Fi to the access point only."
)


def role_from_lastmode(document):
    """Active native Wi-Fi role; 0 when Wi-Fi is off, None when unrecognised."""
    fields = dict(re.findall(rb"(?m)^(WlanRole|Active)=(\S*)\s*$", document))
    if fields.get(b"Active") == b"false":
        return 0
    if fields.get(b"Active") != b"true" or fields.get(b"WlanRole") not in (
        b"1",
        b"2",
        b"3",
    ):
        return None
    return int(fields[b"WlanRole"])


def wifi_role(device):
    if not device.exists(MODE_PATH):
        return None
    try:
        return role_from_lastmode(device.read(MODE_PATH))
    except ValueError:
        return None


def profile_from_xml(document):
    if isinstance(document, str):
        document = document.encode("utf-8")
    if not 0 < len(document) < 2048 or b"<!" in document:
        raise ValueError("Unsupported AP XML size or declaration")
    # Match the bounded native parser's syntax as well as XML semantics.
    # ElementTree otherwise accepts attributes, processing instructions and
    # alternate encodings which the on-device reader deliberately rejects.
    try:
        document.decode("utf-8")
    except UnicodeError:
        raise ValueError("Native AP XML must be UTF-8") from None
    names = {"SSID", "Key", "Security", "HideSSID", "Channel"}
    tags = {name.encode() for name in names | {"WlanApConfig"}}
    allowed = {b"<" + name + b">" for name in tags} | {
        b"</" + name + b">" for name in tags
    }
    for tag in re.findall(rb"<[^>]*>", document):
        if tag.startswith(b"<?xml ") and tag.endswith(b"?>"):
            encoding = re.search(rb"encoding\s*=\s*['\"]([^'\"]+)['\"]", tag)
            if encoding and encoding[1].lower() != b"utf-8":
                raise ValueError("Native AP XML must declare UTF-8")
        elif tag not in allowed:
            raise ValueError("Unsupported native AP XML syntax")
    try:
        root = ET.fromstring(document)
    except ET.ParseError:
        raise ValueError("Malformed native AP XML") from None
    if (
        root.tag != "WlanApConfig"
        or root.attrib
        or len(root) != len(names)
        or (root.text or "").strip()
        or {child.tag for child in root} != names
        or any(
            child.attrib or len(child) or (child.tail or "").strip() for child in root
        )
    ):
        raise ValueError("Unsupported native AP XML schema")
    fields = {child.tag: child.text or "" for child in root}
    if fields["Security"] != "3":
        raise ValueError("Only native WPA2 personal AP security is supported")
    if (
        fields["HideSSID"] not in ("true", "false")
        or not re.fullmatch(r"[0-9]{1,3}", fields["Channel"])
        or int(fields["Channel"]) > 196
    ):
        raise ValueError("Invalid native AP visibility or channel")
    ssid, key = fields["SSID"], fields["Key"]
    if (
        not 1 <= len(ssid.encode("utf-8")) <= 32
        or not 8 <= len(key.encode("utf-8")) <= 63
        or any(ord(c) < 32 or ord(c) == 127 for c in ssid + key)
    ):
        raise ValueError("Invalid native AP SSID/key")
    return dict(ssid=ssid, key=key, security=8, ap_type=0)
