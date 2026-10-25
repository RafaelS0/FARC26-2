#!/usr/bin/env python3
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, "server")
LOG = os.path.join(ROOT, "server.log")
OBJECTS = os.path.join(ROOT, "objects")
PORT = 8080
CHUNK = 4096
FRAME_MARK = "=== Monitor do servidor ==="

SIZES = {"a": 60000, "b": 125000, "c": 68750, "d": 200000, "e": 300000}
RATES = {"127.0.0.1": 550, "default": 1000}

METRIC_RE = re.compile(
    r"\[metricas\] (\S+):(\d+) (\S+) (completo|interrompido) bytes=(\d+)/(\d+) "
    r"envio=([\d.]+) ms banda=(n/d|[\d.]+ kbps) rtt=(n/d|[\d.]+ ms) atraso=(n/d|[\d.]+ ms)"
)

failures = 0
checks = 0


def check(cond, msg):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"  FALHA: {msg}")
    return cond


def object_name(key):
    return f"_e2e_{key}_{SIZES[key]}.bin"


def create_objects():
    for key, size in SIZES.items():
        with open(os.path.join(OBJECTS, object_name(key)), "wb") as f:
            f.write(os.urandom(size))


def remove_objects():
    for key in SIZES:
        try:
            os.remove(os.path.join(OBJECTS, object_name(key)))
        except OSError:
            pass


class Server:
    def __init__(self, args):
        self.proc = subprocess.Popen([SERVER] + args, cwd=ROOT,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.lock = threading.Lock()
        self.out = ""
        self.reader = threading.Thread(target=self.pump, daemon=True)
        self.reader.start()
        for _ in range(100):
            if self.proc.poll() is not None:
                raise RuntimeError("servidor encerrou ao iniciar")
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        self.proc.kill()
        raise RuntimeError("servidor não abriu a porta")

    def pump(self):
        while True:
            data = os.read(self.proc.stdout.fileno(), 65536)
            if not data:
                return
            with self.lock:
                self.out += data.decode(errors="replace")

    def frames(self):
        with self.lock:
            text = self.out
        parts = text.split(FRAME_MARK)[1:]
        return [FRAME_MARK + p for p in parts if "Log detalhado" in p]

    def stop(self):
        self.proc.kill()
        self.proc.wait()
        time.sleep(0.3)


def parse_frame(frame):
    rows = {}
    for line in frame.split("\n"):
        m = re.match(r"^(\d+\.\d+\.\d+\.\d+)\s+(\d+)\s+(.*)$", line)
        if not m:
            continue
        tokens = line.split()
        rtt, delay, bandwidth, objects, last = tokens[-5:]
        rows[int(tokens[1])] = {
            "ip": tokens[0],
            "state": " ".join(tokens[2:-5]),
            "rtt": rtt,
            "delay": delay,
            "bandwidth": bandwidth,
            "objects": objects,
            "last": last,
        }
    return rows


def frame_capacity(frame):
    m = re.search(r"total (\d+) kbps \| reservada (\d+) kbps \| disponível (\d+) kbps \| clientes ativos: (\d+)", frame)
    return tuple(int(x) for x in m.groups()) if m else None


def wait_frame(server, predicate, timeout=6.0):
    end = time.time() + timeout
    while time.time() < end:
        for frame in reversed(server.frames()):
            rows = parse_frame(frame)
            if predicate(rows, frame):
                return rows, frame
        time.sleep(0.1)
    return None, None


def connect_as(src_ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(15)
    s.bind((src_ip, 0))
    s.connect(("127.0.0.1", PORT))
    return s


def request_bytes(path):
    return f"GET /{path} HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n".encode()


def read_headers(sock):
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            break
        buf += chunk
    head, _, body = buf.partition(b"\r\n\r\n")
    length = 0
    for line in head.decode(errors="replace").split("\r\n")[1:]:
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1])
    status = int(head.split(b" ", 2)[1]) if head else 0
    return status, length, body


def download(src_ip, path, sock=None):
    s = sock or connect_as(src_ip)
    port = s.getsockname()[1]
    t_req = time.monotonic()
    s.sendall(request_bytes(path))
    status, length, body = read_headers(s)
    t_hdr = time.monotonic()
    received = len(body)
    while received < length:
        chunk = s.recv(65536)
        if not chunk:
            break
        received += len(chunk)
    t_last = time.monotonic()
    return {"sock": s, "port": port, "status": status, "bytes": received,
            "delay_ms": (t_last - t_req) * 1000.0, "body_ms": (t_last - t_hdr) * 1000.0}


def wait_log(ip, port, path_part, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(LOG, encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = METRIC_RE.search(line)
                    if m and m.group(1) == ip and int(m.group(2)) == port and path_part in m.group(3):
                        return parse_metric(m)
        except OSError:
            pass
        time.sleep(0.1)
    return None


def num(text):
    return None if text == "n/d" else float(text.split()[0])


def parse_metric(m):
    return {"status": m.group(4), "sent": int(m.group(5)), "size": int(m.group(6)),
            "send_ms": float(m.group(7)), "bandwidth": num(m.group(8)),
            "rtt": num(m.group(9)), "delay": num(m.group(10))}


def last_chunk_bytes(size):
    return size - CHUNK * ((size - 1) // CHUNK)


def rate_of(ip):
    return RATES.get(ip, RATES["default"])


def test_known_sizes(server):
    print("[e2e] transferências de tamanho e taxa conhecidos")
    cases = [("127.0.0.2", "a"), ("127.0.0.3", "b"), ("127.0.0.1", "c")]
    results = {}

    def run(ip, key):
        results[(ip, key)] = download(ip, object_name(key))

    threads = [threading.Thread(target=run, args=c) for c in cases]
    [t.start() for t in threads]
    [t.join() for t in threads]

    for ip, key in cases:
        size = SIZES[key]
        rate = rate_of(ip)
        client = results[(ip, key)]
        label = f"{ip} {size} B a {rate} kbps"
        check(client["status"] == 200 and client["bytes"] == size, f"{label}: download incompleto")

        metric = wait_log(ip, client["port"], object_name(key))
        if not check(metric is not None, f"{label}: sem linha [metricas] no log"):
            client["sock"].close()
            continue

        expected_ms = size * 8.0 / rate
        tol = expected_ms * 0.04 + 10.0

        check(metric["status"] == "completo", f"{label}: status {metric['status']}")
        check(metric["sent"] == size and metric["size"] == size,
              f"{label}: bytes {metric['sent']}/{metric['size']} (esperado {size})")
        check(abs(metric["send_ms"] - expected_ms) <= tol,
              f"{label}: envio {metric['send_ms']:.1f} ms (esperado {expected_ms:.1f} ± {tol:.1f})")
        check(metric["bandwidth"] is not None and abs(metric["bandwidth"] - rate) <= rate * 0.04,
              f"{label}: banda {metric['bandwidth']} kbps (esperado {rate} ± 4%)")
        check(metric["rtt"] is not None and 0 < metric["rtt"] < 50, f"{label}: rtt {metric['rtt']} ms")
        check(metric["delay"] is not None and metric["rtt"] is not None
              and abs(metric["delay"] - (expected_ms + metric["rtt"])) <= tol + 5.0,
              f"{label}: atraso {metric['delay']} ms (esperado ≈ {expected_ms:.1f} + rtt)")
        check(metric["delay"] is not None and metric["delay"] >= metric["send_ms"],
              f"{label}: atraso menor que o tempo de envio")

        if metric["delay"] is not None:
            last_ms = last_chunk_bytes(size) * 8.0 / rate
            predicted_client_ms = metric["delay"] - last_ms
            check(abs(client["delay_ms"] - predicted_client_ms) <= 45.0,
                  f"{label}: atraso medido no cliente {client['delay_ms']:.1f} ms, "
                  f"previsto {predicted_client_ms:.1f} ms")

        print(f"        {label}: envio {metric['send_ms']:.1f} ms, banda {metric['bandwidth']} kbps, "
              f"rtt {metric['rtt']} ms, atraso {metric['delay']} ms "
              f"(cliente mediu {client['delay_ms']:.1f} ms)")
        client["sock"].close()


def test_interrupted(server):
    print("[e2e] transferência interrompida pelo cliente")
    ip = "127.0.0.5"
    key = "d"
    s = connect_as(ip)
    port = s.getsockname()[1]
    s.sendall(request_bytes(object_name(key)))
    status, length, body = read_headers(s)
    received = len(body)
    while received < 12000:
        received += len(s.recv(65536))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()

    metric = wait_log(ip, port, object_name(key))
    if not check(metric is not None, "sem linha [metricas] para a transferência interrompida"):
        return
    check(metric["status"] == "interrompido", f"status {metric['status']} (esperado interrompido)")
    check(CHUNK <= metric["sent"] < SIZES[key], f"bytes enviados {metric['sent']} de {SIZES[key]}")
    check(metric["size"] == SIZES[key], "tamanho do objeto deve ser o do arquivo")
    check(metric["delay"] is None, "atraso não deve ser estimado para transferência interrompida")
    check(metric["send_ms"] < SIZES[key] * 8.0 / RATES["default"], "tempo de envio deveria ser parcial")
    print(f"        {metric['sent']} de {metric['size']} bytes enviados antes da interrupção")


def test_monitor(server):
    print("[e2e] monitor em tempo real com vários clientes")
    start_frames = len(server.frames())

    idle_ip = "127.0.0.4"
    idle = connect_as(idle_ip)
    idle_port = idle.getsockname()[1]

    rows, _ = wait_frame(server, lambda r, f: idle_port in r)
    if not check(rows is not None, "cliente novo não apareceu no monitor"):
        idle.close()
        return
    row = rows[idle_port]
    check(row["ip"] == idle_ip, "IP do cliente novo incorreto no monitor")
    check(row["state"].startswith("ocioso"), f"estado do cliente novo: {row['state']}")
    check(row["delay"] == "n/d" and row["bandwidth"] == "n/d",
          f"métricas do cliente novo deveriam ser n/d: atraso={row['delay']} banda={row['bandwidth']}")
    check(row["objects"] == "0/0" and row["last"] == "n/d", "contadores do cliente novo deveriam ser 0/0 e n/d")

    ips = ["127.0.0.6", "127.0.0.7", "127.0.0.8"]
    results = {}

    def run(ip):
        results[ip] = download(ip, object_name("e"))

    threads = [threading.Thread(target=run, args=(ip,)) for ip in ips]
    started = time.monotonic()
    [t.start() for t in threads]
    time.sleep(0.4)

    quick = download("127.0.0.9", "teste.html")
    quick_ms = (time.monotonic() - started) * 1000.0 - 400.0
    check(quick["status"] == 200 and quick["bytes"] > 0, "requisição simples durante o monitoramento falhou")
    check(quick_ms < 1500.0, f"atendimento ficou lento durante o monitoramento ({quick_ms:.0f} ms)")
    quick["sock"].close()

    def serving(r, f):
        busy = [p for p, v in r.items() if v["ip"] in ips and v["state"] == "atendendo"]
        return len(busy) == len(ips)

    rows, frame = wait_frame(server, serving, timeout=2.0)
    check(rows is not None, "nenhum quadro do monitor mostrou os 3 clientes 'atendendo' simultaneamente")
    if frame is not None:
        cap = frame_capacity(frame)
        check(cap is not None and cap[1] >= 4000 and cap[0] - cap[1] == cap[2],
              f"capacidade exibida inconsistente: {cap}")

    [t.join() for t in threads]

    rows_done, _ = wait_frame(
        server,
        lambda r, f: all(
            results[ip]["port"] in r and r[results[ip]["port"]]["bandwidth"] != "n/d"
            and r[results[ip]["port"]]["state"].startswith("ocioso") for ip in ips),
        timeout=4.0)
    if check(rows_done is not None, "monitor não exibiu as métricas dos clientes após a transferência"):
        for ip in ips:
            v = rows_done[results[ip]["port"]]
            bandwidth = float(v["bandwidth"])
            check(abs(bandwidth - 1000.0) <= 60.0, f"{ip}: banda no monitor {bandwidth} kbps (esperado ≈ 1000)")
            check(v["delay"] != "n/d" and float(v["delay"]) > 0, f"{ip}: atraso no monitor {v['delay']}")
            check(v["objects"] == "1/0" and v["last"] == "completo", f"{ip}: contadores {v['objects']} {v['last']}")
            check(v["rtt"] != "n/d", f"{ip}: RTT indisponível no monitor")

    d = download(idle_ip, object_name("a"), sock=idle)
    check(d["status"] == 200, "cliente que estava ocioso não conseguiu baixar")
    rows_idle, _ = wait_frame(
        server, lambda r, f: idle_port in r and r[idle_port]["bandwidth"] != "n/d", timeout=4.0)
    check(rows_idle is not None, "métricas do cliente inicialmente indisponível nunca apareceram")
    if rows_idle is not None:
        check(rows_idle[idle_port]["objects"] == "1/0", "contador do cliente atualizado incorreto")
    idle.close()
    for t in results.values():
        t["sock"].close()

    new_frames = server.frames()[start_frames:]
    stamps = [re.search(r"=== (\d\d:\d\d:\d\d) ===", f) for f in new_frames]
    distinct = {m.group(1) for m in stamps if m}
    check(len(new_frames) >= 4 and len(distinct) >= 3,
          f"monitor deveria atualizar continuamente: {len(new_frames)} quadros, {len(distinct)} instantes")
    print(f"        {len(new_frames)} quadros em {len(distinct)} instantes distintos")


def main():
    if not os.path.exists(SERVER):
        print("binário 'server' não encontrado; rode 'make' antes.")
        return 1
    create_objects()
    server = None
    try:
        server = Server(["20000"])
        test_known_sizes(server)
        test_interrupted(server)
        test_monitor(server)
    finally:
        if server is not None:
            server.stop()
        remove_objects()
    print(f"\n{checks} verificações, {failures} falha(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
