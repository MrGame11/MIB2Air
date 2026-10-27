import re
import socket
import time
import uuid

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


class TelnetCodec:
    """Incremental TELNET parser; accept server ECHO/SGA, reject other options."""

    def __init__(self):
        self.state = "data"
        self.verb = None
        self.accepted = set()

    def feed(self, raw):
        data, reply = bytearray(), bytearray()
        for b in raw:
            if self.state == "data":
                if b == IAC:
                    self.state = "iac"
                else:
                    data.append(b)
            elif self.state == "iac":
                if b == IAC:
                    data.append(IAC)
                    self.state = "data"
                elif b in (DO, DONT, WILL, WONT):
                    self.verb = b
                    self.state = "option"
                elif b == SB:
                    self.state = "sub"
                else:
                    self.state = "data"
            elif self.state == "option":
                if self.verb == WILL:
                    if b in (1, 3):
                        if b not in self.accepted:
                            reply.extend((IAC, DO, b))
                            self.accepted.add(b)
                    else:
                        reply.extend((IAC, DONT, b))
                elif self.verb == DO:
                    reply.extend((IAC, WONT, b))
                elif self.verb == WONT:
                    self.accepted.discard(b)
                self.state = "data"
            elif self.state == "sub":
                if b == IAC:
                    self.state = "sub_iac"
            elif self.state == "sub_iac":
                self.state = "data" if b == SE else "sub"
        return bytes(data), bytes(reply)


class Session:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.codec = TelnetCodec()

    def close(self):
        self.sock.close()

    def send(self, text):
        self.sock.sendall(text.encode().replace(b"\xff", b"\xff\xff") + b"\r\n")

    def recv(self, timeout):
        self.sock.settimeout(timeout)
        raw = self.sock.recv(65536)
        if not raw:
            raise ConnectionError("Telnet connection closed")
        data, reply = self.codec.feed(raw)
        if reply:
            self.sock.sendall(reply)
        return data


def wait_prompt(session, needle):
    data = b""
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        try:
            data += session.recv(1)
        except socket.timeout:
            continue
        if needle in data:
            return
    raise TimeoutError("Login prompt not received: " + needle.decode())


def read_command(session, command):
    token = "APEND" + uuid.uuid4().hex[:12]
    # Short input avoids the long shell wrapper that timed out on the target.
    wrapped = command + "; echo " + token + "$?"
    session.send(wrapped)
    pattern = re.compile(rb"(?:^|\r?\n)" + token.encode() + rb"([0-9]+)\r?\n")
    data = bytearray()
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        try:
            chunk = session.recv(1)
            previous = len(data)
            data.extend(chunk)
        except socket.timeout:
            continue
        match = pattern.search(data, max(0, previous - 128))
        if match:
            body = (
                bytes(data[: match.start()])
                .replace(b"\r\x00", b"")
                .replace(b"\r\n", b"\n")
            )
            # Echo can be disabled or preceded by a delayed shell prompt.
            # Never discard the first actual result line merely by position.
            echoes = (wrapped.encode(), b"# " + wrapped.encode())
            prompt_echo = re.compile(
                rb"^[\w.@:/~ -]{0,128}[#$>] " + re.escape(wrapped.encode()) + rb"$"
            )
            return b"\n".join(
                line
                for line in body.split(b"\n")
                if line.strip() not in echoes
                and not prompt_echo.fullmatch(line.strip())
            ), int(match.group(1))
        if len(data) > 16 * 1024 * 1024:
            raise RuntimeError("Command output limit exceeded; capture stopped")
    raise TimeoutError("Command timed out: " + command)
