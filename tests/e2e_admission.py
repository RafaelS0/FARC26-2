#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, "server")
PORT = 8080
REQUEST = b"GET /teste.html HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"

failures = 0
checks = 0


def check(cond, msg):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"  FALHA: {msg}")
    return cond


def start_server(args):
    proc = subprocess.Popen([SERVER] + args, cwd=ROOT,
                            stdin=subprocess.PIPE,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(100):
        if proc.poll() is not None:
            raise RuntimeError("servidor encerrou ao iniciar: " + proc.stderr.read().decode())
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.05)
    proc.kill()
    raise RuntimeError("servidor não abriu a porta")


def stop_server(proc):
    proc.kill()
    proc.wait()
    time.sleep(0.2)


def read_response(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.decode(errors="replace").split("\r\n")
    if not lines or not lines[0].startswith("HTTP/"):
        return None, {}
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    need = int(headers.get("content-length", 0))
    while len(body) < need:
        chunk = sock.recv(4096)
        if not chunk:
            break
        body += chunk
    return int(lines[0].split()[1]), headers


def connect_as(src_ip):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.bind((src_ip, 0))
    s.connect(("127.0.0.1", PORT))
    return s


def request_as(src_ip):
    s = connect_as(src_ip)
    s.sendall(REQUEST)
    status, headers = read_response(s)
    return s, status, headers


def wait_for_status(src_ip, expected, timeout=5.0):
    end = time.time() + timeout
    while True:
        s, status, headers = request_as(src_ip)
        if status == expected or time.time() > end:
            return s, status, headers
        s.close()
        time.sleep(0.1)
def test_invalid_parameters():
    print("[e2e] parâmetro de vazão incorreto: o servidor não deve iniciar")
    casos = [([], "informe exatamente um parâmetro"),
             (["abc"], "inválida"),
             (["-100"], "negativa"),
             (["0"], "maior que zero"),
             (["1.5"], "inválida"),
             (["99999999999"], "máximo")]
    for args, trecho in casos:
        r = subprocess.run([SERVER] + args, cwd=ROOT, capture_output=True, timeout=5, text=True)
        check(r.returncode == 2, f"{args}: código de saída {r.returncode} (esperado 2)")
        check(trecho in r.stderr, f"{args}: mensagem sem '{trecho}': {r.stderr.strip()[:80]!r}")
        check("Uso:" in r.stderr, f"{args}: sem texto de uso")

    r = subprocess.run([SERVER, "--help"], cwd=ROOT, capture_output=True, timeout=5, text=True)
    check(r.returncode == 0 and "Uso:" in r.stdout, "--help deve exibir o uso e sair com 0")


def test_reserve_share_release():
    print("[e2e] reserva, compartilhamento por IP e liberação (capacidade 2600 kbps)")

    proc = start_server(["2600"])
    abertos = []
    try:
        s1, st, _ = request_as("127.0.0.1"); abertos.append(s1)
        check(st == 200, f"127.0.0.1 (550) deveria ser admitido, veio {st}")
        s2, st, _ = request_as("127.0.0.2"); abertos.append(s2)
        check(st == 200, f"127.0.0.2 (1000) deveria ser admitido, veio {st}")
        s3, st, _ = request_as("127.0.0.3"); abertos.append(s3)
        check(st == 200, f"127.0.0.3 (1000) deveria ser admitido, veio {st}")


        s, st, h = request_as("127.0.0.4")
        check(st == 503, f"127.0.0.4 não cabe (2550+1000 > 2600), veio {st}")
        check("retry-after" in h, "503 deveria trazer Retry-After")
        s.close()


        s2b, st, _ = request_as("127.0.0.2"); abertos.append(s2b)
        check(st == 200, f"2ª conexão de 127.0.0.2 deveria compartilhar a reserva, veio {st}")


        s2.close(); abertos.remove(s2)
        time.sleep(0.5)
        s, st, _ = request_as("127.0.0.4")
        check(st == 503, f"reserva de 127.0.0.2 ainda deveria existir, veio {st}")
        s.close()


        s2b.close(); abertos.remove(s2b)
        s, st, _ = wait_for_status("127.0.0.4", 200)
        check(st == 200, f"capacidade deveria ter sido liberada, veio {st}")
        abertos.append(s)
    finally:
        for s in abertos:
            s.close()
        stop_server(proc)


def test_rate_above_capacity():
    print("[e2e] IP com taxa maior que a capacidade total é sempre recusado")
    proc = start_server(["500"])
    try:
        s, st, _ = request_as("127.0.0.1")
        check(st == 503, f"127.0.0.1 (550) com capacidade 500 deveria ser recusado, veio {st}")
        s.close()
    finally:
        stop_server(proc)


def test_concurrent_clients():
    print("[e2e] 40 clientes simultâneos disputando 5000 kbps (1000 kbps cada)")
    proc = start_server(["5000"])
    n_clientes = 40
    ips = [f"127.0.1.{i + 1}" for i in range(n_clientes)]
    try:
        for rodada in (1, 2):
            resultados = [None] * n_clientes
            socks = [None] * n_clientes
            largada = threading.Barrier(n_clientes)

            def cliente(i):
                try:
                    largada.wait()
                    s, st, _ = request_as(ips[i])
                    socks[i], resultados[i] = s, st
                except Exception as e:
                    resultados[i] = f"erro: {e}"

            ts = [threading.Thread(target=cliente, args=(i,)) for i in range(n_clientes)]
            [t.start() for t in ts]
            [t.join() for t in ts]

            admitidos = resultados.count(200)
            recusados = resultados.count(503)
            check(admitidos == 5, f"rodada {rodada}: {admitidos} admitidos (esperado exatamente 5)")
            check(recusados == n_clientes - 5, f"rodada {rodada}: {recusados} recusados (esperado {n_clientes - 5})")
            check(admitidos + recusados == n_clientes, f"rodada {rodada}: respostas inesperadas: {resultados}")
            print(f"        rodada {rodada}: {admitidos} admitidos, {recusados} recusados")


            for s in socks:
                if s:
                    s.close()
            time.sleep(1.0)
    finally:
        stop_server(proc)


def main():
    if not os.path.exists(SERVER):
        print("binário 'server' não encontrado; rode 'make' antes.")
        return 1
    test_invalid_parameters()
    test_reserve_share_release()
    test_rate_above_capacity()
    test_concurrent_clients()
    print(f"\n{checks} verificações, {failures} falha(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
