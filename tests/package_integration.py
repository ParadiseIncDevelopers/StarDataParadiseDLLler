"""Verify the installed native package against an isolated Windows TLS Server."""

import argparse
from contextlib import contextmanager
import hashlib
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
def server_instance(server, root, password, *, initialize=False, restart=False, routine_test=False):
    path = root / "server.json"
    command = [server, "--config", str(path)]
    if not restart:
        for directory in ("data", "log", "temp"):
            (root / directory).mkdir()
        config = dict(data_dir="data", log_dir="log", temp_dir="temp",
                      tls_certificate="server.pem", tls_private_key="server.key",
                      port=0, timeout_ms=10000, max_connections=16)
        if routine_test:
            config["routine_test_instance"] = True
        path.write_text(json.dumps(config), encoding="ascii")
        run([*command, "--bootstrap-admin", "local-admin"], input=(password + "\n") * 2, timeout=10)
        if initialize:
            run([*command, "--initialize-storage"], timeout=10)
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
                gate = "COMMIT_READY" if mode == "sql_lost_commit" else "CONNECTED"
                require(read_line(client) == gate, f"{mode}: test connection failed")
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
                if mode == "sql_lost_commit":
                    require(relay.forwarded.wait(2), "commit request did not reach the relay")
                output, error = client.communicate(timeout=15)
                require(client.returncode == 0 and output.strip() == f"{mode} passed" and not error,
                        f"{mode}: native consumer failed (exit {client.returncode})\n{error}")
                print(output.strip(), flush=True)
            finally:
                if client.poll() is None:
                    client.kill()
                    client.communicate(timeout=5)


def sql_evidence(output, mode):
    lines = output.splitlines()
    require(lines and lines[-1] == f"{mode} passed", f"{mode}: missing completion marker")
    batches = {}
    for line in lines[:-1]:
        label, value = line.split(" ", 1)
        batches.setdefault(label, []).append(json.loads(value))
    return batches


def result_rows(items):
    rows = []
    for index, item in enumerate(items):
        batch = item["batch"]
        require(batch["batch_index"] == index and batch["result_set"] == 0, "incorrect native batch order")
        require(("columns" in batch) == (index == 0), "unexpected column metadata")
        final = index + 1 == len(items)
        require(batch["end_of_set"] == batch["end_of_results"] == final, "incorrect batch end")
        rows.extend(batch["rows"])
    return rows


def test_sql_consumer(args, consumer, root, password):
    fixture = root / "sql"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    ca = fixture / "ca.pem"

    with server_instance(args.server, fixture, password, initialize=True) as port:
        seen = sql_evidence(run([consumer, "sql", str(port), str(ca)], input=password + "\n",
                                timeout=30), "sql")
        items = next((obj for obj in seen["metadata"][0]["objects"]
                      if obj["qualified_name"] == ["app", "data", "items"]), None)
        require(items and [column["name"] for column in items["columns"]]
                == ["id", "note", "price", "flags", "stamp"], "catalog did not expose SQL-created table")
        require(seen["inserted"][0]["affected_rows"] == seen["updated"][0]["affected_rows"] == 1,
                "native writes did not affect one row")
        [row] = result_rows(seen["typed"])
        require(row == [
            {"t": "i64", "v": "-9223372036854775808"},
            {"t": "text", "v": "İstanbul '); SELECT 9; --"},
            {"t": "decimal", "unscaled": "12345678901234567890123456789012345678", "scale": 4},
            {"t": "binary", "hex": "00ff"},
            {"t": "timestamp", "seconds": "0", "nanos": 123456789, "offset_minutes": 0},
        ], "typed storage round trip changed a value")
        [nullable] = result_rows(seen["nullable"])
        require(nullable == [row[0], {"t": "null"}, *row[2:]], "typed NULL update did not persist")
        require(result_rows(seen["rolled_back"]) == [], "rollback/savepoint/delete left a row behind")
        require(seen["ledger"] == seen["committed"] and seen["ledger"][0]["status"] == "committed"
                and seen["unknown"] == [{"status": "unknown"}], "native commit outcome mismatch")
        require(len(seen["stream"]) > 1 and all(len(item["batch"]["rows"]) <= 29 for item in seen["stream"]),
                "native result ignored requested batch bound")
        require(sorted(int(record[0]["v"]) for record in result_rows(seen["stream"])) == list(range(1, 321)),
                "bulk result had missing or duplicate rows")
        print("sql_crud_typed_and_bounded_stream passed", flush=True)
        test_fault(consumer, "sql_lost_commit", port, ca, password)

    with server_instance(args.server, fixture, password, restart=True) as port:
        seen = sql_evidence(run([consumer, "sql_restart", str(port), str(ca)], input=password + "\n",
                                timeout=15), "sql_restart")
        require(seen["ledger"][0]["status"] == "committed", "lost reply transaction was not found after restart")
        [persisted] = result_rows(seen["persisted"])
        require(persisted == [row[0], {"t": "text", "v": "committed through lost reply"}, *row[2:]],
                "recovered typed row did not match committed values")
        print("sql_post_restart_ledger passed", flush=True)

    check = run([args.server, "--config", str(fixture / "server.json"), "--check-storage"], timeout=15)
    require(check.startswith("Storage OK:"), "restart fixture failed disk and WAL integrity check")


def test_security_consumer(args, consumer, root, password):
    fixture = root / "security"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    alice, replacement = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
    with server_instance(args.server, fixture, password, initialize=True) as port:
        output = run([consumer, "security", str(port), str(fixture / "ca.pem")],
                     input=f"{password}\n{alice}\n{replacement}\n", timeout=45)
        require(output.strip() == "security passed", "unexpected native security output")
        print("security_grant_revoke_pool_reset_and_credentials passed", flush=True)

    logs = b"".join(path.read_bytes() for path in fixture.rglob("*.log"))
    require(all(secret.encode("ascii") not in logs for secret in (password, alice, replacement)),
            "credential leaked in Server logs")


def test_plsql_consumer(args, consumer, root, password):
    fixture = root / "plsql"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    with server_instance(args.server, fixture, password, initialize=True) as port:
        output = run([consumer, "plsql", str(port), str(fixture / "ca.pem")],
                     input=password + "\n", timeout=40)
        seen = sql_evidence(output, "plsql")

    sets = []
    names = []
    index = 0
    finished = True
    for item in seen["pl_sets"]:
        batch = item["batch"]
        if batch["batch_index"] == 0:
            require(finished and batch["result_set"] == len(sets), "result-set sequence changed")
            sets.append([])
            names.append(batch["set_name"])
            require(bool(batch["columns"]), "named set was missing its column metadata")
            index = 0
        require(batch["result_set"] == len(sets) - 1 and batch["batch_index"] == index and
                len(batch["rows"]) <= 1, "result set violated requested batch bound or order")
        index += 1
        sets[-1].extend(batch["rows"])
        finished = batch["end_of_set"]
    require(finished and seen["pl_sets"][-1]["batch"]["end_of_results"], "procedure result did not finish")
    require(names == ["out", "found", "all_rows"], "named sets did not follow OUT/REFCURSOR order")
    require(sets[:2] == [[[{"t": "i64", "v": "7"}]], [[{"t": "i64", "v": "7"}]]] and
            sorted(row[0]["v"] for row in sets[2]) == ["3", "7", "9"], "procedure out/cursor rows were wrong")

    [roundtrip] = result_rows(seen["pl_compound"])
    require(seen["pl_compound"][0]["batch"]["set_name"] == "out", "compound OUT set name missing")
    require(roundtrip == [
        {"t": "record", "fields": [
            {"name": "önce", "value": {"t": "i64", "v": "-9223372036854775808"}},
            {"name": "nested", "value": {"t": "collection", "items": [
                {"t": "null"},
                {"t": "decimal", "unscaled": "12345678901234567890123456789012345678", "scale": 4},
                {"t": "record", "fields": [
                    {"name": "a", "value": {"t": "text", "v": "İstanbul\0"}},
                    {"name": "day", "value": {"t": "date", "v": "2024-02-29"}},
                ]},
            ]}},
        ]},
        {"t": "collection", "items": [
            {"t": "binary", "hex": "00ff"},
            {"t": "timestamp", "seconds": "0", "nanos": 123456789, "offset_minutes": 180},
        ]},
    ], "compound type or scalar precision lost on native round trip")
    print("plsql_named_result_sets_and_compound_roundtrip passed", flush=True)


def test_debugger_consumer(args, consumer, root, password):
    fixture = root / "debugger"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    guest_password = secrets.token_urlsafe(32)
    with server_instance(args.server, fixture, password, initialize=True, routine_test=True) as port:
        output = run([consumer, "debugger", str(port), str(fixture / "ca.pem")],
                     input=f"{password}\n{guest_password}\n", timeout=45)
        seen = sql_evidence(output, "debugger")

    first, second = seen["debug_before"][0], seen["debug_after"][0]
    require(first["state"] == second["state"] == "paused" and first["line"] == 4 and second["line"] == 5,
            "debugger breakpoint/step did not match routine source")
    watched = {"name": "local_value", "type": "BIGINT", "available": True}
    require(seen["watch_before"] == [{**watched, "value": {"t": "i64", "v": "5"}}] and
            seen["watch_after"] == [{**watched, "value": {"t": "i64", "v": "7"}}],
            "debugger watch did not reflect the live frame")
    live = first["stack"][-1]
    require(live["routine"][-1] == "debug_work" and live["revision"] == 1 and
            any(item["name"] == "local_value" and item["available"] and
                item["value"] == {"t": "i64", "v": "5"} for item in live["locals"]),
            "debugger stack locals or pinned revision mismatch")
    require(result_rows(seen["debug_output"]) == [[{"t": "i64", "v": "7"}]], "resumed procedure OUT changed")
    samples = seen["profile"][0]["samples"]
    require(any(sample["routine"][-1] == "debug_work" and sample["revision"] == 1
                and sample["instructions"] > 0 and 0 < sample["sql_us"] <= sample["inclusive_us"]
                and sample["exclusive_us"] <= sample["inclusive_us"] for sample in samples),
            "profiler did not record runtime and SQL wall time")
    require(seen["fixture"] == [{"exception": None, "passed": True, "rolled_back": True, "sets": []}],
            "rollback fixture result did not match")
    require(result_rows(seen["rollback_probe"]) == [], "routine fixture or target disconnect leaked committed data")
    logs = b"".join(path.read_bytes() for path in fixture.rglob("*.log"))
    require(guest_password.encode("ascii") not in logs, "debugger fixture secret leaked")
    print("plsql_debug_stack_watch_step_profiler_and_disconnect passed", flush=True)


def test_history_consumer(args, consumer, root, password):
    fixture = root / "history"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    guest_password = secrets.token_urlsafe(32)
    with server_instance(args.server, fixture, password, initialize=True) as port:
        output = run([consumer, "history", str(port), str(fixture / "ca.pem")],
                     input=f"{password}\n{guest_password}\n", timeout=40)
        seen = sql_evidence(output, "history")

    rev = seen["history_revisions"][0]
    log = seen["history_log"][0]
    rows = [entry for entry in log["entries"] if entry["kind"] == "row"]
    require(len(rows) == 3, "rollback-only or missing entry in History")
    records = {entry["revision"]: entry for entry in rows}
    first, change, last = (records[rev[key]] for key in ("inserted", "changed", "deleted"))
    require(first["row_id"] == change["row_id"] == last["row_id"] and first["actor"] == "local-admin",
            "primary-key update lost the committed row identity or actor")
    require(first["after"]["values"]["id"] == {"t": "i64", "v": "11"} and
            change["after"]["values"]["id"] == {"t": "i64", "v": "22"} and
            change["after"]["values"]["note"] == {"t": "text", "v": "değişti"} and last["after"] is None,
            "insert/update/delete image drifted")
    require(change["after"]["values"]["payload"] == {
        "type": "BLOB", "size": 3, "sha256": hashlib.sha256(bytes.fromhex("00ff10")).hexdigest(), "scope": "value",
    }, "History BLOB surfaced data instead of a binary summary")
    stored = seen["history_read"][0]
    require(stored["revision"] == rev["inserted"] and len(stored["rows"]) == 1 and
            stored["rows"][0]["row_id"] == first["row_id"] and
            stored["rows"][0]["values"]["id"] == {"t": "i64", "v": "11"},
            "historical snapshot did not retain its row")
    diff = [entry for entry in seen["history_diff"][0]["entries"] if entry["kind"] == "row"]
    require(len(diff) == 1 and diff[0]["row_id"] == first["row_id"] and
            diff[0]["after"]["values"]["id"] == {"t": "i64", "v": "22"}, "History DIFF failed the key change")
    require(seen["history_verify"][0]["verified"] is True and
            seen["history_verify"][0]["current_revision"] == rev["deleted"],
            "History VERIFY did not attest the current revision")
    require(seen["history_pinned"][0]["pins"] == [
        {"kind": "snapshot", "name": "retained_fixture", "revision": rev["inserted"]},
    ], "History pin did not retain its owner scope")
    for name in ("history_retained", "history_purged"):
        require(seen[name][0]["earliest_revision"] == rev["deleted"] and not seen[name][0]["pins"],
                "History policy floor did not persist after unpin")
    logs = b"".join(path.read_bytes() for path in fixture.rglob("*.log"))
    require(guest_password.encode("ascii") not in logs, "History fixture credential leaked")
    print("history_native_log_diff_snapshot_pins_and_permission passed", flush=True)


def test_restore_consumer(args, consumer, root, password):
    fixture = root / "restore"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    with server_instance(args.server, fixture, password, initialize=True) as port:
        output = run([consumer, "restore", str(port), str(fixture / "ca.pem")],
                     input=password + "\n", timeout=40)
        seen = sql_evidence(output, "restore")
    preview = seen["restore_preview"][0]
    applied, replayed, ledger = (seen[key][0] for key in ("restore_apply", "restore_replay", "restore_ledger"))
    [original] = seen["restore_original"][0]["rows"]
    effects = preview["effects"]
    require(preview["mode"] == "restore" and
            [effects[key] for key in ("inserted", "updated", "deleted", "catalog")] == [1, 0, 0, 0] and
            effects["rows"] == [{"row_id": original["row_id"], "operation": "insert", "fields": []}],
            "Wrong row restore impact")
    require(applied == {"status": "committed", "lsn": applied["lsn"], "replayed": False} and
            replayed == {"status": "committed", "lsn": applied["lsn"], "replayed": True} and
            ledger == {"status": "committed", "lsn": applied["lsn"]}, "Fresh connection did not replay the durable restore")
    [restored] = seen["restore_read"][0]["rows"]
    require(restored["row_id"] == original["row_id"] and restored["values"] == original["values"],
            "Restore lost original values or stable row identity")
    require(int(applied["lsn"]) > int(preview["current_revision"]), "Restore did not create a new revision")
    inverse = seen["revert_preview"][0]
    back = seen["revert_apply"][0]
    effects = inverse["effects"]
    require(inverse["mode"] == "revert" and
            [effects[key] for key in ("inserted", "updated", "deleted", "catalog")] == [0, 1, 0, 0] and
            effects["rows"] == [{"row_id": inverse["row_id"], "operation": "update", "fields": ["note"]}] and
            back["status"] == "committed" and back["replayed"] is False and
            int(back["lsn"]) > int(applied["lsn"]), "Field revert did not create the expected new revision")
    rows = [[value["v"] for value in row] for row in result_rows(seen["restore_rows"])]
    require(rows == [["11", "original", "first"], ["22", "second", "preserve"], ["33", "independent", "untouched"]],
            "Restore/revert changed a later independent field or duplicated a row")
    print("history_native_restore_stale_replay_and_field_revert passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--server", required=True, type=Path)
    parser.add_argument("--openssl", required=True, type=Path)
    parser.add_argument("--scratch", required=True, type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--asan", action="store_true")
    parser.add_argument("--sql", action="store_true", help="also exercise SQL on initialized storage and restart")
    parser.add_argument("--security", action="store_true", help="exercise authenticated roles and session control")
    parser.add_argument("--plsql", action="store_true", help="exercise typed PL/SQL named result sets")
    parser.add_argument("--debugger", action="store_true", help="exercise two-connection routine debugger and rollback fixture")
    parser.add_argument("--history", action="store_true", help="exercise History through the installed Native SDK")
    parser.add_argument("--restore", action="store_true", help="exercise History restore, revert and outcome replay")
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

        if args.sql:
            test_sql_consumer(args, consumer, root, password)

        if args.security:
            test_security_consumer(args, consumer, root, password)

        if args.plsql:
            test_plsql_consumer(args, consumer, root, password)

        if args.debugger:
            test_debugger_consumer(args, consumer, root, password)

        if args.history:
            test_history_consumer(args, consumer, root, password)

        if args.restore:
            test_restore_consumer(args, consumer, root, password)

        logs = b"".join(path.read_bytes() for path in root.rglob("*.log"))
        require(password.encode("ascii") not in logs and incorrect.encode("ascii") not in logs,
                "password leaked in server logs")


if __name__ == "__main__":
    main()
