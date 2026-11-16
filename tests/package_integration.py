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
                 "include/StarDataParadiseDLLler.h", "include/StarDataParadiseDLLler.hpp",
                 "share/StarDataParadiseDLLler/COMPATIBILITY.md",
                 "share/StarDataParadiseDLLler/examples/cpp/CMakeLists.txt",
                 "lib/cmake/StarDataParadiseDLLler/StarDataParadiseDLLlerConfig.cmake"):
        require((sdk / path).is_file(), f"package missing {path}")
    source = Path(__file__).resolve().with_name("consumer")
    build = root / "consumer"
    flags = ["-DSDP_ADDRESS_SANITIZER=ON"] if args.asan else []
    run([args.cmake, "-S", str(source), "-B", str(build), "-G", "Visual Studio 18 2026",
         "-A", "x64,version=10.0.26100.0", "-T", "v145,host=x64", f"-DCMAKE_PREFIX_PATH={sdk}", *flags],
        timeout=100)
    run([args.cmake, "--build", str(build), "--config", "Release"], timeout=100)
    example = root / "example"
    run([args.cmake, "-S", str(sdk / "share/StarDataParadiseDLLler/examples/cpp"), "-B", str(example),
         "-G", "Visual Studio 18 2026", "-A", "x64,version=10.0.26100.0", "-T", "v145,host=x64",
         f"-DCMAKE_PREFIX_PATH={sdk}"], timeout=100)
    run([args.cmake, "--build", str(example), "--config", "Release"], timeout=100)
    return (str(build / "Release" / "sdp_native_consumer.exe"),
            str(example / "Release" / "native_example.exe"))


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
def server_instance(server, root, password, *, initialize=False, restart=False, routine_test=False, backup=False):
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
        if backup:
            (root / "archives").mkdir()
            config["backup"] = "archives"

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


def test_decision_consumer(args, consumer, root, password):
    fixture = root / "decision"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    with server_instance(args.server, fixture, password, initialize=True) as port:
        output = run([consumer, "decision", str(port), str(fixture / "ca.pem")],
                     input=password + "\n", timeout=40)
        seen = sql_evidence(output, "decision")
    require(seen["decision_validate"] == [{"valid": True}], "Decision validation did not confirm the rule")
    created = seen["decision_created"][0]["lsn"]
    appended = seen["decision_appended"][0]["lsn"]
    activated = seen["decision_activated"][0]["lsn"]
    require(int(created) < int(appended) < int(activated) and
            seen["decision_ledger"] == [{"status": "committed", "lsn": created}],
            "Decision creation/activation did not publish the expected revision and request outcome")
    for label in ("decision_why", "decision_whatif", "decision_recheck", "decision_historical"):
        inspection = seen[label][0]
        require(inspection["allowed"] is False and inspection["rules"] and inspection["computed"] == {},
                f"{label} did not explain its failing rule without computed values")
    require(seen["decision_historical"][0]["revision"] == created,
            "historical explanation did not use the requested revision")

    comparison = seen["decision_test"][0]
    [changed, unchanged] = comparison["cases"]
    require(comparison["changed"] == 1 and
            not changed["prior"]["allowed"] and changed["candidate"]["allowed"] and
            unchanged["prior"]["allowed"] and unchanged["candidate"]["allowed"],
            "Decision impact report ignored a threshold change or changed unaffected results")
    require(seen["decision_now"][0]["revision"] == activated and seen["decision_now"][0]["allowed"] is True,
            "new rule did not take effect atomically after activation")
    require(result_rows(seen["decision_before"]) == [] and result_rows(seen["decision_after"]) == [
        [{"t": "i64", "v": "2"}, {"t": "i64", "v": "3"}],
    ], "Decision simulation wrote rows or the activated rule did not permit the actual insert")
    print("decision_native_validate_explain_whatif_impact_recheck_and_activate passed", flush=True)


def compute_checksum(count):
    mask = (1 << 64) - 1
    checksum = 0x9E3779B97F4A7C15
    for number in range(1, count + 1):
        checksum ^= (number + 0x9E3779B97F4A7C15) & mask
        checksum = ((checksum ^ (checksum >> 30)) * 0xBF58476D1CE4E5B9) & mask
        checksum = ((checksum ^ (checksum >> 27)) * 0x94D049BB133111EB) & mask
        checksum ^= checksum >> 31
    return str(checksum)


def test_jobs_consumer(args, consumer, root, password):
    fixture = root / "jobs"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    guest_password = secrets.token_urlsafe(32)
    with server_instance(args.server, fixture, password, initialize=True) as port:
        output = run([consumer, "jobs", str(port), str(fixture / "ca.pem")],
                     input=f"{password}\n{guest_password}\n", timeout=45)
        seen = sql_evidence(output, "jobs")
    limits = seen["job_capabilities"][0]
    require("compute" in limits["kinds"] and limits["maximum_workers"] == 1 and limits["hard_memory_limit"],
            "Native Jobs capabilities did not report the actual isolated worker budgets")
    queued, done, status = (seen[label][0] for label in ("job_queued", "job_done", "job_status"))
    require(queued["state"] == "queued" and queued["result"] is None and
            seen["job_ledger"] == [{"status": "committed", "lsn": queued["lsn"]}],
            "Job submission did not commit the queued job and durable outcome together")
    replayed = seen["job_replayed"][0]
    require(replayed["job_id"] == queued["job_id"] and replayed["lsn"] == queued["lsn"]
            and seen["job_withdrawn"] == [{"status": "unknown"}],
            "Native retry changed the job identity or rolled back submission left an outcome")
    require(queued["job_id"] == done["job_id"] == status["job_id"] and done["state"] == status["state"] == "succeeded"
            and done["attempt"] == 1 and done["result"] == {"count": 500000, "checksum": compute_checksum(500000)}
            and [event["state"] for event in done["events"]] == ["queued", "starting", "running", "succeeded"],
            "reconnected SDK did not observe the same completed job and independently checked result")

    progress, requested, cancelled = (seen[label][0] for label in ("job_progress", "job_cancel_request", "job_cancelled"))
    require(progress["state"] == "running" and progress["worker_pid"] > 0 and
            progress["progress"]["total"] == 500000000 and 0 <= progress["progress"]["done"] <= 500000000 and
            requested["cancel_requested"] and cancelled["state"] == "cancelled" and cancelled["result"] is None and
            progress["job_id"] == requested["job_id"] == cancelled["job_id"],
            "Native progress or cancellation did not reflect the running worker lifecycle")
    schedule, observed, scheduled, stopped = (
        seen[label][0] for label in ("job_schedule", "job_schedule_observed", "job_schedule_done", "job_schedule_cancelled")
    )
    require(schedule["schedule_id"] == observed["schedule_id"] == stopped["schedule_id"] and schedule["enabled"]
            and observed["last_job_id"] == scheduled["job_id"] and scheduled["state"] == "succeeded"
            and scheduled["result"] == {"count": 300, "checksum": compute_checksum(300)} and not stopped["enabled"],
            "the durable schedule did not launch, expose and disable the expected controlled job")
    logs = b"".join(path.read_bytes() for path in fixture.rglob("*.log"))
    require(guest_password.encode("ascii") not in logs, "Job fixture credential leaked")
    print("jobs_native_submit_reconnect_progress_cancel_schedule_rollback_and_owner_guard passed", flush=True)


def test_scheduled_recipes_consumer(args, consumer, root, password):
    fixture = root / "scheduled-recipes"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    with server_instance(args.server, fixture, password, initialize=True) as port:
        output = run([consumer, "scheduled_recipes", str(port), str(fixture / "ca.pem")],
                     input=password + "\n", timeout=45)
    seen = sql_evidence(output, "scheduled_recipes")
    capabilities = seen["recipe_capabilities"][0]
    require(all({"sql_script", "retrain"}.issubset(capabilities[field]) for field in ("kinds", "schedule_kinds")),
            "Native job capabilities did not advertise both recipe kinds")
    sql_job = seen["recipe_sql_job"][0]
    require(sql_job["result"] == {"statements": 2, "affected_rows": 2, "selected_rows": 0} and
            result_rows(seen["recipe_sql_rows"]) == [[{"t": "i64", "v": "1"}, {"t": "i64", "v": "7"}]],
            "scheduled SQL did not commit both statements to the real table")
    model_job, status, evaluation = (seen[label][0] for label in
        ("recipe_model_job", "recipe_model_status", "recipe_model_evaluation"))
    require(status["active_version"] == 1 and [entry["version"] for entry in status["versions"]] == [1, 2] and
            model_job["result"]["version"] == 2 and model_job["job_id"] == evaluation["run_id"] and
            model_job["result"]["checksum"] == evaluation["checksum"] == status["versions"][1]["checksum"] and
            model_job["result"]["dataset_id"] == evaluation["dataset_id"] == seen["ml_dataset"][0]["dataset_id"],
            "scheduled retraining did not publish a dataset-bound artifact while preserving the active version")
    print("native_scheduled_sql_and_model_retrain_reconnect passed", flush=True)


def test_ml_consumer(args, consumer, root, password):
    fixture = root / "ml"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    with server_instance(args.server, fixture, password, initialize=True) as port:
        output = run([consumer, "ml", str(port), str(fixture / "ca.pem")],
                     input=password + "\n", timeout=45)
    seen = sql_evidence(output, "ml")
    devices = seen["ml_devices"][0]
    require(devices["cpu_available"] and isinstance(devices["devices"], list) and
            (not devices["gpu_available"] or devices["gpu_verified"]),
            "Native ML device report did not distinguish CPU and verified GPU")
    dataset = seen["ml_dataset"][0]
    materialized = seen["ml_materialized"][0]
    require(dataset["row_count"] == 10 and dataset["dataset_id"] == materialized["dataset_id"] and
            materialized["prepared"]["split"] == {"train": list(range(6)), "validation": [6, 7], "test": [8, 9]},
            "Native dataset did not preserve the predictable split")
    created, queued, done, status, evaluation = (seen[label][0] for label in
        ("ml_model", "ml_train", "ml_trained", "ml_status", "ml_evaluate"))
    require(created["active_version"] is None and created["versions"] == [] and
            queued["state"] == "queued" and done["state"] == "succeeded" and
            queued["job_id"] == done["job_id"] == evaluation["run_id"] and
            evaluation["dataset_id"] == dataset["dataset_id"] and
            status["versions"][0]["checksum"] == evaluation["checksum"],
            "Native model training did not publish the expected dataset-bound artifact")
    require(abs(seen["ml_predict"][0]["prediction"] - 40.0) < 1e-9 and
            seen["ml_activate"][0]["active_version"] == seen["ml_rollback"][0]["active_version"] == 1 and
            seen["ml_activate_second"][0]["active_version"] == 2 and
            seen["ml_compare"][0]["comparable"] and
            [item["version"] for item in seen["ml_compare"][0]["runs"]] == [1, 2] and
            seen["ml_delete_second"][0]["deleted"] and
            seen["ml_deactivate"][0]["active_version"] is None and
            seen["ml_deactivate_again"][0]["active_version"] is None and
            seen["ml_delete"][0]["deleted"] and seen["ml_status_after_delete"][0]["versions"] == [],
            "Native model prediction and version lifecycle returned unexpected results")
    print("native_ml_dataset_train_evaluate_activate_predict_rollback_delete passed", flush=True)

def test_backup_consumer(args, consumer, root, password):
    fixture = root / "backup"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    with server_instance(args.server, fixture, password, initialize=True, backup=True) as port:
        output = run([consumer, "backup", str(port), str(fixture / "ca.pem")],
                     input=password + "\n", timeout=60)
    seen = sql_evidence(output, "backup")
    queued, complete, status = (seen[label][0] for label in
        ("backup_queued", "backup_complete", "backup_status"))
    require(queued["state"] == "queued" and complete["state"] == status["state"] == "succeeded" and
            queued["job_id"] == complete["job_id"] == status["job_id"] and complete["result"] == status["result"],
            "Native backup did not expose the same completed durable job")
    catalog, verified, restored, deleted, final_catalog = (seen[label][0] for label in
        ("backup_catalog", "backup_verified", "backup_restored", "backup_deleted", "backup_final_catalog"))
    backup_id = complete["result"]["backup_id"]
    require(catalog["pitr"] is False and len(catalog["backups"]) == 1 and
            catalog["backups"][0]["backup_id"] == verified["backup_id"] == restored["backup_id"] ==
            deleted["backup_id"] == backup_id and verified["verified"] and restored["restored"] and
            restored["target_name"] == "sdk_copy" and deleted["deleted"] and final_catalog["backups"] == [],
            "Native backup catalog, verified restore or retention differed from the archive job")
    require(catalog["backups"][0]["manifest_sha256"] == verified["manifest_sha256"] == restored["manifest_sha256"],
            "Native restore referenced a different archive manifest")
    scheduled, stopped, occurrence = (seen[label][0] for label in
        ("backup_schedule", "backup_schedule_cancelled", "backup_scheduled_result"))
    require(scheduled["enabled"] and not stopped["enabled"] and
            scheduled["schedule_id"] == stopped["schedule_id"] and
            occurrence["state"] == "succeeded" and occurrence["result"]["verified"] and
            occurrence["result"]["backup_id"] != backup_id,
            "Native backup schedule failed to publish a distinct verified archive")
    generic = seen["backup_generic_result"][0]
    require(generic["state"] == "succeeded" and generic["result"]["verified"] and
            generic["result"]["backup_id"] != backup_id, "Native generic job API refused backup")
    target = fixture / "temp" / "restores" / restored["target_name"]
    require((target / "store.sdp").is_file() and not (target / "admin-v1.json").exists() and
            not (target / "server.key").exists(),
            "Native restore did not produce separate storage with only archive data")
    print("native_backup_submit_status_catalog_verify_restore_delete_and_schedule passed", flush=True)

def test_admin_consumer(args, consumer, root, password):
    fixture = root / "admin"
    fixture.mkdir()
    certificates(args.openssl, fixture)
    with server_instance(args.server, fixture, password, initialize=True) as port:
        output = run([consumer, "admin", str(port), str(fixture / "ca.pem")],
                     input=password + "\n", timeout=60)
    seen = sql_evidence(output, "admin")
    status, before, storage, after = (seen[label][0] for label in
        ("admin_status", "admin_health_before", "admin_storage", "admin_health_after"))
    require(status["instance_id"] == before["instance_id"] == after["instance_id"] and
            status["port"] == port and len(status["sessions"]) >= 1 and isinstance(status["limits"], dict),
            "Native instance status did not report the live listener and its limits")
    require(before["checks"]["storage"]["status"] == "UNKNOWN" and
            before["checks"]["storage"]["checked_at"] is None and storage["check"] == "storage" and
            storage["result"]["status"] == "OK" and isinstance(storage["result"]["checked_at"], int) and
            after["checks"]["storage"] == storage["result"],
            "Native Health status did not update from a measured storage check")
    preview, applied = (seen[label][0] for label in ("admin_config_preview", "admin_config_applied"))
    require(preview["valid"] and not preview["applied"] and preview["dynamic"]["timeout_ms"] == 8500 and
            preview["restart_required"] == {"port": 13393, "max_connections": 12} and
            applied["applied"] and applied["dynamic"]["timeout_ms"] == 8500 and not applied["restart_required"],
            "Native configuration preview and apply did not separate dynamic and restart fields")
    config_file = json.loads((fixture / "server.json").read_text(encoding="utf-8"))
    prior_name = applied["previous_config"]
    require(Path(prior_name).name == prior_name, "Native apply returned a non-local previous config name")
    previous = json.loads((fixture / prior_name).read_text(encoding="utf-8"))
    require(config_file == {**previous, "timeout_ms": 8500} and previous["timeout_ms"] == 10000,
            "Native configuration apply persisted unrelated changes")
    preview, created = (seen[label][0] for label in ("admin_support_preview", "admin_support_created"))
    filename = fixture / "temp" / "support" / "sdk_bundle" / "bundle.json"
    content = filename.read_text(encoding="utf-8")
    require(preview["secret_scan_passed"] and created["secret_scan_passed"] and
            created["target_name"] == "sdk_bundle" and password not in content and
            "tls_private_key" not in json.loads(content)["config"],
            "Native support bundle was not redacted and scanned")
    queued, done = (seen[label][0] for label in ("admin_maintenance_queued", "admin_maintenance_done"))
    require(queued["state"] == "queued" and queued["job_id"] == done["job_id"] and
            done["state"] == "succeeded" and done["result"] is not None,
            "Native maintenance submission did not complete through the shared job API")
    generic = seen["admin_generic_maintenance"][0]
    scheduled = seen["admin_scheduled_maintenance"][0]
    require(generic["state"] == scheduled["state"] == "succeeded" and
            generic["result"]["action"] == "integrity" and scheduled["result"]["action"] == "index",
            "Native job API refused direct or scheduled maintenance")
    print("native_admin_status_health_config_support_and_maintenance passed", flush=True)

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
    parser.add_argument("--decision", action="store_true", help="exercise Decision policy and simulations through the installed Native SDK")
    parser.add_argument("--jobs", action="store_true", help="exercise durable Jobs, progress, cancellation and scheduling")
    parser.add_argument("--scheduled-recipes", action="store_true", help="exercise scheduled SQL and retraining against a capable Server")
    parser.add_argument("--ml", action="store_true", help="exercise dataset and model operations through the installed SDK")
    parser.add_argument("--backup", action="store_true", help="exercise backup jobs and isolated restore through the installed SDK")
    parser.add_argument("--admin", action="store_true", help="exercise instance health, configuration, support and maintenance via the installed SDK")
    args = parser.parse_args()
    args.server = str(args.server.resolve(strict=True))
    args.openssl = str(args.openssl.resolve(strict=True))
    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-", dir=args.scratch.resolve()) as folder:
        root = Path(folder)
        consumer, example = build_consumer(args, root)
        print("independent installed-package build passed", flush=True)
        certificates(args.openssl, root)
        password = secrets.token_urlsafe(32)
        incorrect = secrets.token_urlsafe(32)
        ca = root / "ca.pem"

        with server_instance(args.server, root, password) as server_port:
            run_client(consumer, "real", server_port, ca, password)
            metadata, capabilities = map(json.loads, run([example, "127.0.0.1", str(server_port), "localhost",
                str(ca), "local-admin"], input=password + "\n", timeout=15).splitlines())
            require(len(metadata["session_id"]) == 32 and metadata["protocol_version"] == 1 and
                    metadata["product_version"] and capabilities["health"], "installed C++ example failed")
            print("installed_cxx_example_hello_and_capabilities passed", flush=True)
            run_client(consumer, "callback_close", server_port, ca, password)
            run_client(consumer, "invalid_abi", server_port, ca, password)
            run_client(consumer, "auth_reject", server_port, ca, incorrect)
            run_client(consumer, "tls_ip", server_port, ca, password)
            run_client(consumer, "tls_name", server_port, ca, password)
            run_client(consumer, "tls_untrusted", server_port, root / "other.pem", password)
            for mode in ("operation_timeout", "cancel", "connection_loss"):
                test_fault(consumer, mode, server_port, ca, password)
            run_client(consumer, "real", server_port, ca, password)
            print("explicit_read_after_reconnect passed", flush=True)

        raii = root / "raii"
        raii.mkdir()
        certificates(args.openssl, raii)
        with server_instance(args.server, raii, password, initialize=True) as port:
            run_client(consumer, "raii", port, raii / "ca.pem", password)

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

        if args.decision:
            test_decision_consumer(args, consumer, root, password)

        if args.jobs:
            test_jobs_consumer(args, consumer, root, password)

        if args.scheduled_recipes:
            test_scheduled_recipes_consumer(args, consumer, root, password)

        if args.ml:
            test_ml_consumer(args, consumer, root, password)

        if args.backup:
            test_backup_consumer(args, consumer, root, password)

        if args.admin:
            test_admin_consumer(args, consumer, root, password)

        logs = b"".join(path.read_bytes() for path in root.rglob("*.log"))
        require(password.encode("ascii") not in logs and incorrect.encode("ascii") not in logs,
                "password leaked in server logs")


if __name__ == "__main__":
    main()
