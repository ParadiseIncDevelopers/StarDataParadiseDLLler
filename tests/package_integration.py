"""Verify the installed native package against an isolated Windows TLS Server."""

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import queue
import secrets
import select
import socket
import subprocess
import tempfile
import threading
import time
import zipfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(args, *, timeout=120, **kwargs):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", timeout=timeout, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{Path(args[0]).name} exit {result.returncode}\n{result.stdout}")
    return result.stdout


def build_consumer(args, root):
    sdk = root / "sdk"
    with zipfile.ZipFile(args.package) as package:
        package.extractall(sdk)
    for path in ("bin/StarDataParadiseDLLler.dll", "lib/StarDataParadiseDLLler.lib",
                 "include/StarDataParadiseDLLler.h",
                 "lib/cmake/StarDataParadiseDLLler/StarDataParadiseDLLlerConfig.cmake"):
        require((sdk / path).is_file(), f"package missing {path}")
    source = Path(__file__).resolve().with_name("consumer")
    build = root / "consumer"
    flags = ["-DSDP_ADDRESS_SANITIZER=ON"] if args.asan else []
    run([args.cmake, "-S", str(source), "-B", str(build), "-G", "Visual Studio 18 2026",
         "-A", "x64,version=10.0.26100.0", "-T", "v145,host=x64", f"-DCMAKE_PREFIX_PATH={sdk}", *flags],
        timeout=100)
    run([args.cmake, "--build", str(build), "--config", "Release"], timeout=100)
    return str(build / "Release" / "sdp_native_consumer.exe")


def certificates(openssl, root):
    (root / "openssl.cnf").write_text("[ req ]\ndistinguished_name = dn\n[ dn ]\n", encoding="ascii")
    for name in ("ca", "other"):
        run([openssl, "req", "-config", "openssl.cnf", "-x509", "-newkey", "rsa:2048", "-nodes",
             "-keyout", name + ".key", "-out", name + ".pem", "-days", "2", "-subj", f"/CN=SDP {name}",
             "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign"],
            cwd=root, timeout=20)
    run([openssl, "req", "-config", "openssl.cnf", "-new", "-newkey", "rsa:2048", "-nodes",
         "-keyout", "server.key", "-out", "server.csr", "-subj", "/CN=localhost"], cwd=root, timeout=20)
    (root / "server.ext").write_text(
        "subjectAltName=DNS:localhost,IP:127.0.0.1\n"
        "extendedKeyUsage=serverAuth\nbasicConstraints=critical,CA:FALSE\n", encoding="ascii",
    )
    run([openssl, "x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
         "-CAcreateserial", "-out", "server.pem", "-days", "2", "-extfile", "server.ext"],
        cwd=root, timeout=20)


def read_line(process, timeout=10):
    result = queue.Queue(maxsize=1)
    reader = threading.Thread(target=lambda: result.put(process.stdout.readline()), daemon=True)
    reader.start()
    try:
        return result.get(timeout=timeout).strip()
    except queue.Empty as error:
        raise RuntimeError("native test process did not report readiness") from error


@contextmanager
def server_instance(server, root, password):
    for directory in ("data", "log", "temp"):
        (root / directory).mkdir()
    path = root / "server.json"
    path.write_text(json.dumps(dict(data_dir="data", log_dir="log", temp_dir="temp",
                                    tls_certificate="server.pem", tls_private_key="server.key",
                                    port=0, timeout_ms=10000, max_connections=16)), encoding="ascii")
    command = [server, "--config", str(path)]
    run([*command, "--bootstrap-admin", "local-admin"], input=(password + "\n") * 2, timeout=10)
    server_process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                      encoding="utf-8", errors="replace", creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        message = read_line(server_process)
        fields = message.split()
        require(len(fields) == 3 and fields[:2] == ["LISTENING", "127.0.0.1"], "server did not bind loopback")
        yield int(fields[2])
    finally:
        if server_process.poll() is None:
            server_process.terminate()
        server_process.communicate(timeout=8)


def local_listener():
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    return listener


class Relay:
    """Forward only encrypted TLS bytes between two loopback sockets."""

    def __init__(self, upstream_port):
        self.listener = local_listener()
        self.listener.settimeout(10)
        self.port = self.listener.getsockname()[1]
        self.upstream_port = upstream_port
        self.pause = threading.Event()
        self.stop = threading.Event()
        self.forwarded = threading.Event()
        self.disconnected = threading.Event()
        self.thread = threading.Thread(target=self.forward, daemon=True)

    def forward(self):
        try:
            client, _ = self.listener.accept()
            with client, socket.create_connection(("127.0.0.1", self.upstream_port), timeout=5) as upstream:
                client.settimeout(5)
                while not self.stop.is_set():
                    ready, _, _ = select.select([client, upstream], [], [], 0.05)
                    for source in ready:
                        data = source.recv(65536)
                        if not data:
                            return
                        if source is client:
                            upstream.sendall(data)
                            if self.pause.is_set():
                                self.forwarded.set()
                        elif not self.pause.is_set():
                            client.sendall(data)
        except OSError:
            pass
        finally:
            self.disconnected.set()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exception):
        self.stop.set()
        self.listener.close()
        self.thread.join(timeout=10)
        require(not self.thread.is_alive(), "TLS relay did not stop")


def run_client(consumer, mode, port, ca, password):
    output = run([consumer, mode, str(port), str(ca)], input=password + "\n", timeout=15)
    require(output.strip() == f"{mode} passed", f"unexpected native output for {mode}")
    print(output.strip(), flush=True)


def test_fault(consumer, mode, server_port, ca, password):
    with Relay(server_port) as relay:
        command = [consumer, mode, str(relay.port), str(ca)]
        with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, encoding="utf-8", errors="replace") as client:
            try:
                client.stdin.write(password + "\n")
                client.stdin.flush()
                require(read_line(client) == "CONNECTED", f"{mode}: test connection failed")
                relay.pause.set()
                if mode == "connection_loss":
                    relay.stop.set()
                    require(relay.disconnected.wait(5), "loopback relay did not disconnect")
                client.stdin.write("go\n")
                client.stdin.flush()
                if mode == "cancel":
                    require(read_line(client) == "PENDING" and relay.forwarded.wait(2), "cancel had no in-flight request")
                    client.stdin.write("go\n")
                    client.stdin.flush()
                output, error = client.communicate(timeout=15)
                require(client.returncode == 0 and output.strip() == f"{mode} passed" and not error,
                        f"{mode}: native consumer failed (exit {client.returncode})\n{error}")
                print(output.strip(), flush=True)
            finally:
                if client.poll() is None:
                    client.kill()
                    client.communicate(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--openssl", required=True, type=Path)
    parser.add_argument("--scratch", required=True, type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--asan", action="store_true")
    args = parser.parse_args()
    args.server = str(args.server.resolve(strict=True))
    args.openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-", dir=args.scratch.resolve()) as folder:
        root = Path(folder)
        consumer = build_consumer(args, root)
        print("independent installed-package build passed", flush=True)
        certificates(args.openssl, root)
        password = secrets.token_urlsafe(32)
        incorrect = secrets.token_urlsafe(32)
        ca = root / "ca.pem"

        with server_instance(args.server, root, password) as server_port:
            run_client(consumer, "real", server_port, ca, password)
            run_client(consumer, "callback_close", server_port, ca, password)
            run_client(consumer, "invalid_abi", server_port, ca, password)
            run_client(consumer, "auth_reject", server_port, ca, incorrect)
            run_client(consumer, "tls_ip", server_port, ca, password)
            run_client(consumer, "tls_name", server_port, ca, password)
            run_client(consumer, "tls_untrusted", server_port, root / "other.pem", password)
            for mode in ("operation_timeout", "cancel", "connection_loss"):
                test_fault(consumer, mode, server_port, ca, password)

        with local_listener() as listener:
            begin = time.monotonic()
            run_client(consumer, "handshake_timeout", listener.getsockname()[1], ca, password)
            require(time.monotonic() - begin < 5, "native handshake timeout exceeded its bound")

        logs = b"".join(path.read_bytes() for path in (root / "log").iterdir())
        require(password.encode("ascii") not in logs and incorrect.encode("ascii") not in logs,
                "password leaked in server logs")


if __name__ == "__main__":
    main()
