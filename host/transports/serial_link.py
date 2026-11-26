"""Transporte série do DashDeck: uma mensagem JSON por linha pela USB (ver firmware/main/ui.c)."""
from __future__ import annotations

import json
import logging
import queue
import threading
import time

import serial  # pyserial

log = logging.getLogger("dashd")


class SerialLink:
    """Ligação serial com reconexão automática (DashDeck, ESP32-C6 pela USB). Entrega eventos numa fila.

    prepare: adapta cada mensagem antes de a enviar (no dashd, o texto às fontes do ESP); raw=True no
    send() salta-o (dados base64, ex.: imagens da câmara)."""

    def __init__(self, port: str, events: queue.Queue, prepare=None):
        self.port = port
        self.prepare = prepare or (lambda m: m)
        self.events = events
        self.acks: queue.Queue = queue.Queue()   # confirmações "bgack" do ESP (fluxo do mapa)
        self.jpg_acks: queue.Queue = queue.Queue()   # "jpgack" (visor da câmara, noutra thread)
        self.ser: serial.Serial | None = None
        self.lock = threading.Lock()
        threading.Thread(target=self._run, daemon=True, name="link").start()

    def send(self, msg: dict, raw: bool = False) -> None:
        """raw=True: sem adaptar o texto à fonte do ESP (dados base64, ex.: imagens da câmara)."""
        data = (json.dumps(msg if raw else self.prepare(msg), ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        with self.lock:
            if not self.ser:
                return
            try:
                self.ser.write(data)
            except (serial.SerialException, OSError) as e:
                log.warning("erro ao escrever: %s", e)
                self._close()

    def _close(self) -> None:
        try:
            if self.ser:
                self.ser.close()
        except Exception:
            pass
        self.ser = None

    def _run(self) -> None:
        while True:
            if not self.ser:
                try:
                    s = serial.Serial(self.port, 115200, timeout=0.5)
                    time.sleep(0.2)
                    s.reset_input_buffer()  # descarta hellos/logs acumulados enquanto ninguém lia
                    with self.lock:
                        self.ser = s
                    log.info("ligado a %s", self.port)
                    self.events.put(("connected", None))
                except (serial.SerialException, OSError):
                    time.sleep(2)
                    continue
            try:
                line = self.ser.readline()
            except (serial.SerialException, OSError, TypeError) as e:
                log.warning("ligação perdida: %s", e)
                with self.lock:
                    self._close()
                self.events.put(("disconnected", None))
                time.sleep(1)
                continue
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").strip()
            if text.startswith("{"):
                try:
                    m = json.loads(text)
                    if m.get("t") in ("bgack", "iconack", "emojiack"):
                        self.acks.put(m)
                    elif m.get("t") == "jpgack":
                        self.jpg_acks.put(m)
                    else:
                        self.events.put(("msg", m))
                except json.JSONDecodeError:
                    log.debug("JSON inválido do ESP: %r", text)
            elif text:
                log.debug("esp: %s", text.lstrip("# "))
