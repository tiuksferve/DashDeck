"""WebSocket mínimo (RFC 6455) sobre asyncio, só com a biblioteca padrão.

O /usr/bin/python3 do uConsole não tem `websockets`; o DashDeck³ só precisa de mensagens de texto,
ping/pong e close, por isso isto chega e evita dependências.
"""
from __future__ import annotations

import asyncio
import base64
import hashlib
import os
import struct

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_MSG = 4 * 1024 * 1024


class Closed(Exception):
    pass


def accept_key(key: str) -> str:
    return base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()


async def read_http_head(reader: asyncio.StreamReader) -> tuple[str, str, dict[str, str]]:
    """Lê "MÉTODO caminho HTTP/1.1" e os cabeçalhos (nomes em minúsculas)."""
    head = await reader.readuntil(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    method, path, _ = (lines[0].split(" ", 2) + ["", ""])[:3]
    headers = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    return method, path, headers


class WebSocket:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, client: bool = False):
        self.r = reader
        self.w = writer
        self.client = client          # o cliente tem de mascarar o que envia
        self._wlock = asyncio.Lock()
        self.closed = False

    async def _send_frame(self, opcode: int, data: bytes) -> None:
        if self.closed:
            raise Closed()
        hdr = bytearray([0x80 | opcode])
        mask = 0x80 if self.client else 0
        n = len(data)
        if n < 126:
            hdr.append(mask | n)
        elif n < 65536:
            hdr.append(mask | 126)
            hdr += struct.pack(">H", n)
        else:
            hdr.append(mask | 127)
            hdr += struct.pack(">Q", n)
        if self.client:
            key = os.urandom(4)
            hdr += key
            data = bytes(b ^ key[i % 4] for i, b in enumerate(data))
        async with self._wlock:
            self.w.write(bytes(hdr) + data)
            await self.w.drain()

    async def send(self, text: str) -> None:
        await self._send_frame(0x1, text.encode())

    async def ping(self) -> None:
        await self._send_frame(0x9, b"")

    async def close(self) -> None:
        if not self.closed:
            try:
                await self._send_frame(0x8, b"")
            except Exception:
                pass
            self.closed = True
        try:
            self.w.close()
        except Exception:
            pass

    async def recv(self) -> str:
        """Próxima mensagem de texto (junta fragmentos; responde a pings)."""
        parts: list[bytes] = []
        while True:
            b0, b1 = await self.r.readexactly(2)
            fin, op = b0 & 0x80, b0 & 0x0F
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", await self.r.readexactly(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", await self.r.readexactly(8))[0]
            if n > MAX_MSG:
                raise Closed("mensagem demasiado grande")
            key = await self.r.readexactly(4) if b1 & 0x80 else None
            data = await self.r.readexactly(n)
            if key:
                data = bytes(b ^ key[i % 4] for i, b in enumerate(data))
            if op == 0x8:
                self.closed = True
                raise Closed()
            if op == 0x9:
                await self._send_frame(0xA, data)
                continue
            if op == 0xA:
                continue
            parts.append(data)
            if fin:
                return b"".join(parts).decode("utf-8", "replace")


async def connect(host: str, port: int, path: str) -> WebSocket:
    """Cliente (para o fakedeck e testes)."""
    reader, writer = await asyncio.open_connection(host, port)
    key = base64.b64encode(os.urandom(16)).decode()
    writer.write((f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
                  f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    await writer.drain()
    status = (await reader.readuntil(b"\r\n\r\n")).decode("latin-1").split("\r\n")[0]
    if " 101 " not in status + " ":
        writer.close()
        raise ConnectionError(status)
    return WebSocket(reader, writer, client=True)
