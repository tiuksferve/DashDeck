"""Estado de um ecrã ligado ao serviço (uma "sessão"): o DashDeck pela USB e o DashDeck³ por Wi-Fi.

O motor (`dashd.App`) guarda aqui tudo o que é de um aparelho: o tipo, o transporte, a navegação (raiz do
menu, pilha de telas e páginas), o tamanho e a rotação, e o que já lhe foi enviado (fundos, ícones,
cabeçalho). Os providers são partilhados e ficam no `App`: dois ecrãs no ADS-B mostram o mesmo readsb.
O `App` expõe os campos da sessão atual como atributos seus (`app.stack`, `app.esp_w`, ...); cada evento
traz a sessão de onde veio e o motor muda `app.session` antes de o tratar.
"""
from __future__ import annotations

from dataclasses import dataclass, field, fields

import emojis as _emojis   # o campo "emojis" abaixo taparia o nome do módulo


@dataclass
class Session:
    kind: str = "dashdeck"              # "dashdeck" (C6, USB) ou "deck3" (S3, Wi-Fi)
    link: object = None                 # transporte: send(msg, raw=False), acks, jpg_acks
    root: dict = field(default_factory=dict)     # menu inicial deste ecrã
    layout: str = "grid"                # menus em "grid" (boxes) ou "cards"
    dock_screens: set = field(default_factory=set)   # telas que mostram o dock (só no DashDeck)
    screen_off: int = 0                 # s sem toques na tela inicial até apagar (0 = nunca)
    refresh: float = 2.0                # s entre atualizações da tela aberta
    emojis: bool = True                 # o ecrã recebe imagens de emojis
    emoji: object = field(default_factory=_emojis.Registry)   # que emojis este ecrã já tem (e em que código)
    emoji_px: int = _emojis.SIZE         # altura das imagens dos emojis (C6 14 px, DashDeck³ 18 px)
    dev: object = None                  # o aparelho, visto pelos providers do DashDeck³ (deck.Dev)
    last_ping: float = 0.0
    last_refresh: float = 0.0
    stack: list = field(default_factory=list)   # navegação: [raiz, ..., tela aberta]
    page: dict = field(default_factory=dict)     # página atual de cada tela com várias páginas (swipe)
    backlight: int = 80
    rotation: int = 1
    rot_sent: bool = False
    esp_ready: bool = False             # já respondeu ao hello (antes disso não se mandam telas)
    last_hello: float = 0.0
    esp_info: dict = field(default_factory=dict)   # último hello (versão, memória, cartão)
    esp_w: int = 172
    esp_h: int = 320
    esp_sd: bool = False
    last_sent: str = ""                 # última tela enviada (para mandar só "update" quando não muda)
    last_status: str = ""
    last_tpl: str = ""
    last_time: float = 0.0
    header_sent: int = -1
    bg_sent: str | None = None
    bg_hold: float = 0.0                # não reenviar o fundo antes disto
    dock_lost: bool = False             # o aparelho libertou os ícones e o dock
    icons_sent: set = field(default_factory=set)
    _icon_src: dict = field(default_factory=dict)
    shot_parts: list = field(default_factory=list)   # captura do ecrã em curso
    shot_path: str | None = None


SESSION_FIELDS = tuple(f.name for f in fields(Session))


def delegate_to_session(cls):
    """Expõe os campos da Session como atributos da classe (lidos e escritos em self.session)."""
    for name in SESSION_FIELDS:
        setattr(cls, name, property(lambda self, n=name: getattr(self.session, n),
                                    lambda self, v, n=name: setattr(self.session, n, v)))
    return cls


class SessionEvents:
    """A fila de eventos do motor vista por um transporte: cada evento sai marcado com a sessão."""

    def __init__(self, q, session: Session):
        self.q = q
        self.session = session

    def put(self, item) -> None:
        self.q.put((item[0], item[1], self.session))


class NullLink:
    """Transporte de uma sessão desligada (ex.: dashd sem o DashDeck): não envia nada."""

    def __init__(self):
        import queue
        self.acks = queue.Queue()
        self.jpg_acks = queue.Queue()

    def send(self, msg: dict, raw: bool = False) -> None:
        pass
