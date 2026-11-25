#!/usr/bin/python3
"""Receptor de radiossondas RS41 sem janela, para a box Sondas do DashDeck (serviço dashd-sonde).

O mesmo fluxo do SondeDeck: rtl_fm (FM, 9 kHz) -> sox (WAV 48 kHz, passa-baixo 2,6 kHz) -> rs41mod
(rs1729/RS, --json). Cada pacote descodificado vai, uma linha JSON por pacote, para
~/.cache/dashd/sonde/<série>.jsonl; o estado do receptor fica em ~/.cache/dashd/sonde/status.json.
A lógica (rebentamento, estatísticas, mapa) está no dashd, que lê estes ficheiros.

Configuração por variáveis de ambiente (o dashd escreve ~/.config/dashd/sonde.env antes de ligar):
  SONDE_FREQ (MHz, 403.4), SONDE_GAIN (dB, 40; 0 = automático), SONDE_PPM (0), SONDE_DEVICE (0),
  SONDE_DECODER (caminho do rs41mod; por omissão o compilado no projeto SondeDeck),
  SONDE_IDLE_STOP (s sem pacotes, depois de ter recebido algum, até sair e libertar o SDR; 1800).

Teste sem rádio: sonderx.py --replay voo.jsonl [--speed 20] (linhas no formato do rs41mod --json).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

DIR = Path.home() / ".cache" / "dashd" / "sonde"
DECODERS = [Path.home() / "PycharmProjects/SondeDeck/decoders/rs1729/demod/mod/rs41mod",
            Path("/usr/local/bin/rs41mod"), Path("/usr/bin/rs41mod")]
KEYS = ("frame", "lat", "lon", "alt", "vel_h", "vel_v", "heading", "sats", "temp", "humidity", "pressure",
        "batt", "bt", "subtype", "freq", "tx_frequency")

_stop = False


def _sig(*_):
    global _stop
    _stop = True


def write_status(st: dict) -> None:
    DIR.mkdir(parents=True, exist_ok=True)
    tmp = DIR / "status.json.tmp"
    tmp.write_text(json.dumps(st))
    tmp.replace(DIR / "status.json")


def frame_out(d: dict) -> dict | None:
    """Pacote do rs41mod -> registo guardado (só os campos úteis, com a hora em segundos)."""
    sid = d.get("id") or d.get("serial")
    if not sid or d.get("lat") is None or d.get("lon") is None or d.get("alt") is None:
        return None
    try:
        t = datetime.fromisoformat(str(d["datetime"]).replace("Z", "+00:00")).timestamp()
    except (KeyError, ValueError):
        t = time.time()
    out = {"t": round(t, 1), "rx": round(time.time(), 1), "id": str(sid)}
    for k in KEYS:
        if d.get(k) is not None:
            out[k] = d[k]
    return out


def lines_radio(freq: float, gain: int, ppm: int, dev: str, decoder: str, procs: list):
    rtl = [shutil.which("rtl_fm") or "rtl_fm", "-M", "fm", "-f", str(int(round(freq * 1e6))), "-s", "9000",
           "-d", dev]
    if gain > 0:
        rtl += ["-g", str(gain)]
    if ppm:
        rtl += ["-p", str(ppm)]
    rtl += ["-"]
    sox = [shutil.which("sox") or "sox", "-t", "raw", "-r", "9000", "-e", "signed", "-b", "16", "-c", "1", "-",
           "-t", "wav", "-r", "48000", "-b", "16", "-", "lowpass", "2600"]
    p1 = subprocess.Popen(rtl, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    p2 = subprocess.Popen(sox, stdin=p1.stdout, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    p3 = subprocess.Popen([decoder, "--ecc2", "--crc", "--json", "--ptu"], stdin=p2.stdout,
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    p1.stdout.close()
    p2.stdout.close()
    procs += [p1, p2, p3]
    for line in p3.stdout:
        yield line
        if _stop:
            break


def lines_replay(path: str, speed: float):
    """Reproduz um voo gravado ao ritmo da hora dos pacotes (acelerado `speed` vezes)."""
    t_prev = None
    with open(path) as f:
        for line in f:
            if _stop:
                break
            try:
                t = datetime.fromisoformat(json.loads(line)["datetime"].replace("Z", "+00:00")).timestamp()
            except (ValueError, KeyError):
                t = None
            if t_prev is not None and t is not None and t > t_prev:
                time.sleep(min((t - t_prev) / speed, 5))
            t_prev = t if t is not None else t_prev
            yield line


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--replay", help="ficheiro com pacotes rs41mod --json (teste sem rádio)")
    ap.add_argument("--speed", type=float, default=1.0)
    a = ap.parse_args()
    signal.signal(signal.SIGTERM, _sig)
    signal.signal(signal.SIGINT, _sig)

    freq = float(os.environ.get("SONDE_FREQ", "403.4"))
    gain = int(float(os.environ.get("SONDE_GAIN", "40")))
    ppm = int(float(os.environ.get("SONDE_PPM", "0")))
    dev = os.environ.get("SONDE_DEVICE", "0")
    idle_stop = float(os.environ.get("SONDE_IDLE_STOP", "1800"))
    decoder = os.environ.get("SONDE_DECODER") or next((str(p) for p in DECODERS if p.exists()), "")
    st = {"freq": freq, "pid": os.getpid(), "since": time.time(), "frames": 0, "last": 0, "id": "",
          "state": "a receber", "error": "", "replay": bool(a.replay)}
    if not a.replay and not (decoder and os.access(decoder, os.X_OK)):
        st.update(state="erro", error="rs41mod não encontrado (compilar no SondeDeck: ./install.sh)")
        write_status(st)
        return 1
    write_status(st)

    procs: list = []
    seen: dict[str, int] = {}     # último frame guardado de cada sonda (o rs41mod pode repetir)
    files: dict = {}
    last_write = 0.0
    src = lines_replay(a.replay, a.speed) if a.replay else lines_radio(freq, gain, ppm, dev, decoder, procs)

    def watchdog():
        """Voo acabado (já houve pacotes e há `idle_stop` s que não chegam): sai e liberta o SDR.
        Numa thread, porque sem pacotes a leitura do rs41mod fica parada."""
        global _stop
        while not _stop:
            time.sleep(5)
            if idle_stop > 0 and st["last"] and time.time() - st["last"] > idle_stop:
                st["ended"] = "sem pacotes há muito: voo terminado"
                _stop = True
                for p in procs:
                    if p.poll() is None:
                        p.terminate()
    threading.Thread(target=watchdog, daemon=True).start()
    try:
        for line in src:
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                f = frame_out(json.loads(line))
            except ValueError:
                continue
            if not f or seen.get(f["id"]) == f.get("frame"):
                continue
            seen[f["id"]] = f.get("frame")
            if f["id"] not in files:
                DIR.mkdir(parents=True, exist_ok=True)
                files[f["id"]] = open(DIR / f"{f['id'].replace('/', '_')}.jsonl", "a", buffering=1)
            files[f["id"]].write(json.dumps(f) + "\n")
            st.update(frames=st["frames"] + 1, last=time.time(), id=f["id"])
            if time.monotonic() - last_write > 2:
                write_status(st)
                last_write = time.monotonic()
            if _stop:
                break
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
        for p in procs:
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
        for fh in files.values():
            fh.close()
    st.update(state="parado", pid=0)
    if not _stop and not a.replay:
        st.update(state="erro", error="o rtl_fm parou (SDR ocupado por outro programa?)")
    write_status(st)
    return 0


if __name__ == "__main__":
    sys.exit(main())
