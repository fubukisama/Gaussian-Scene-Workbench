"""Owned loopback HTTPS update fixture. Never used by a production client.

Run only in remote acceptance. Its generated CA/key is test-only and remains in
the explicitly selected fixture directory; it is not a production trust root.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import socket
import ssl
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID


def certificates(root: Path) -> tuple[Path, Path, Path]:
    now = dt.datetime.now(dt.timezone.utc)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "GSW owned test-only CA")])
    ca = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name)
          .public_key(ca_key.public_key()).serial_number(x509.random_serial_number())
          .not_valid_before(now - dt.timedelta(minutes=5)).not_valid_after(now + dt.timedelta(days=2))
          .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
          .add_extension(x509.KeyUsage(True, False, False, False, False, True, True, False, False), critical=True)
          .sign(ca_key, hashes.SHA256()))
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(ca_name)
            .public_key(key.public_key()).serial_number(x509.random_serial_number())
            .not_valid_before(now - dt.timedelta(minutes=5)).not_valid_after(now + dt.timedelta(days=1))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.SubjectAlternativeName([
                x509.DNSName("localhost"), x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]), critical=False)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .sign(ca_key, hashes.SHA256()))
    ca_path, cert_path, key_path = root / "fixture-ca.pem", root / "fixture-server.pem", root / "fixture-server.key"
    ca_path.write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                         serialization.PrivateFormat.PKCS8,
                                         serialization.NoEncryption()))
    try:
        key_path.chmod(0o600)
    except OSError:
        pass  # Windows fixture directory belongs to the test runner account.
    return ca_path, cert_path, key_path


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass  # Do not print request URLs/headers or TLS credentials.

    def serve_owned_file(self, path, raw_range, raw_if_range, visit):
        """Shared static TUF/immutable-asset transport for the full upgrade test."""
        try:
            control = {}
            if self.server.control_file.exists():
                if self.server.control_file.stat().st_size > 8192:
                    raise ValueError("control size")
                control = json.loads(self.server.control_file.read_text(encoding="utf-8"))
            variant = control.get("repository", "A")
            if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", variant):
                raise ValueError("variant")
            if path.startswith("/repository/"):
                base = self.server.content_root / variant
                relative = unquote(path[len("/repository/"):])
            else:
                base = self.server.content_root / "downloads"
                relative = unquote(path[len("/downloads/"):])
            parts = relative.split("/")
            if any(not p or p in (".", "..") or p.endswith((".", " ")) or
                   any(c in p for c in ("\\", ":", "\x00")) for p in parts):
                raise ValueError("path")
            source = base.joinpath(*parts)
            if not source.resolve().is_relative_to(base.resolve()):
                raise ValueError("outside")
            for cursor in (source,) + tuple(source.parents):
                if cursor.is_symlink() or getattr(cursor, "is_junction", lambda: False)():
                    raise ValueError("link")
                if cursor == self.server.content_root:
                    break
            if not source.is_file():
                self.send_error(404)
                return
            length = source.stat().st_size
            with source.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            tag = '"sha256-' + digest + '"'
            mode = control.get("networkMode", "normal") if control.get("faultPath") == path else "normal"
            match = re.fullmatch(r"bytes=(\d+)-", raw_range)
            start = int(match[1]) if match else 0
            partial = bool(match) and mode != "range-ignored" and (
                raw_if_range == tag or mode in ("changed-etag", "bad-range"))
            if start >= length:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{length}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if not partial:
                start = 0
            self.send_response(206 if partial else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("ETag", '"changed-test-validator"' if mode == "changed-etag" and partial else tag)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(length - start))
            if mode == "encoding":
                self.send_header("Content-Encoding", "gzip")
            if partial:
                advertised = start + 1 if mode == "bad-range" else start
                self.send_header("Content-Range", f"bytes {advertised}-{length-1}/{length}")
            self.end_headers()
            with source.open("rb") as stream:
                stream.seek(start)
                sent = 0
                while data := stream.read(8192):
                    if mode == "short":
                        data = data[:max(0, max(1, (length-start)//2) - sent)]
                        if not data:
                            self.close_connection = True
                            self.connection.shutdown(socket.SHUT_RDWR)
                            self.connection.close()
                            return
                    if mode in ("changed-etag", "bad-range") and partial:
                        data = b"X" * len(data)
                    if mode == "bad-sha" and sent == 0:
                        data = bytes([data[0] ^ 1]) + data[1:]
                    self.wfile.write(data)
                    self.wfile.flush()
                    sent += len(data)
                    if (mode == "drop-once" and visit == 1 and sent >= 32768 or
                            mode == "short" and sent >= max(1, (length-start)//2)):
                        self.close_connection = True
                        self.connection.shutdown(socket.SHUT_RDWR)
                        self.connection.close()
                        return
        except (ValueError, OSError, json.JSONDecodeError):
            if not self.wfile.closed:
                self.close_connection = True

    def do_GET(self):
        path = urlsplit(self.path).path[:128]
        raw_range = self.headers.get("Range", "")
        raw_if_range = self.headers.get("If-Range", "")
        with self.server.guard:
            self.server.counts[path] = self.server.counts.get(path, 0) + 1
            visit = self.server.counts[path]
            record = {"path": path, "range": raw_range[:128], "if_range": raw_if_range[:128],
                      "cookie_present": bool(self.headers.get("Cookie")),
                      "authorization_present": bool(self.headers.get("Authorization")),
                      "accept_encoding": self.headers.get("Accept-Encoding", "")[:64]}
            with self.server.log_path.open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(record) + "\n")
        if path.startswith(("/repository/", "/downloads/")):
            self.serve_owned_file(path, raw_range, raw_if_range, visit)
            return
        payload = self.server.payload
        tag = '"owned-payload-a"'
        if path == "/timeout":
            time.sleep(3)
        if path in ("/redirect", "/forbidden-host", "/downgrade"):
            location = "/payload" if path == "/redirect" else (
                "https://example.invalid/payload" if path == "/forbidden-host" else
                f"http://127.0.0.1:{self.server.server_port}/payload")
            self.send_response(302)
            self.send_header("Location", location)
            self.send_header("Set-Cookie", "fixture_cookie=never_forward; Path=/")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path == "/service-unavailable-once" and visit == 1:
            self.send_response(503)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path == "/error429":
            self.send_response(429)
            self.send_header("Retry-After", "86400")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        match = re.fullmatch(r"bytes=(\d+)-", raw_range)
        start = int(match[1]) if match else 0
        if start >= len(payload):
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{len(payload)}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        partial = bool(match) and path != "/range-ignored"
        if path == "/weak-etag":
            tag = "W/" + tag
        if path == "/changed-etag":
            tag = '"owned-payload-b"'
        body = payload[start:] if partial else payload
        if path == "/bad-sha":
            body = bytes([body[0] ^ 1]) + body[1:]
        if partial and path in ("/changed-etag", "/bad-range"):
            body = b"X" * len(body)  # Must be discarded before a safe full retry.
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("ETag", tag)
        self.send_header("Accept-Ranges", "bytes")
        if path == "/encoding":
            self.send_header("Content-Encoding", "gzip")
        length = len(body) + (1 if path == "/oversize" else 0)
        self.send_header("Content-Length", str(length))
        if partial:
            advertised_start = start + 1 if path == "/bad-range" else start
            self.send_header("Content-Range", f"bytes {advertised_start}-{len(payload)-1}/{len(payload)}")
        self.end_headers()
        try:
            if path == "/drop-once" and visit == 1:
                self.wfile.write(body[:32768])
                self.wfile.flush()
                self.close_connection = True
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            for offset in range(0, len(body), 8192):
                self.wfile.write(body[offset:offset + 8192])
                self.wfile.flush()
                if path == "/slow":
                    time.sleep(.015)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError, OSError):
            pass  # Cancellation and injected disconnect are fixture behavior.


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--ready", type=Path, required=True)
    parser.add_argument("--content-root", type=Path)
    parser.add_argument("--control-file", type=Path)
    args = parser.parse_args()
    args.root.mkdir(parents=True, exist_ok=True)
    ca, cert, key = certificates(args.root)
    payload = bytes(range(256)) * 4096
    payload_path = args.root / "payload.bin"
    payload_path.write_bytes(payload)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    server.guard = threading.Lock()
    server.counts = {}
    server.payload = payload
    server.log_path = args.root / "requests.jsonl"
    server.content_root = args.content_root or args.root / "served"
    server.control_file = args.control_file or args.root / "control.json"
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    ready = {"schema": "gsw.update_network_fixture/1", "testOnly": True,
             "baseUrl": f"https://127.0.0.1:{server.server_port}", "caPath": str(ca),
             "payloadPath": str(payload_path), "length": len(payload),
             "sha256": hashlib.sha256(payload).hexdigest(), "requestLog": str(server.log_path),
             "contentRoot": str(server.content_root), "controlFile": str(server.control_file)}
    temporary = args.ready.with_suffix(args.ready.suffix + ".tmp")
    temporary.write_text(json.dumps(ready), encoding="utf-8")
    temporary.replace(args.ready)
    server.serve_forever(poll_interval=.1)


if __name__ == "__main__":
    main()
