"""Estado de um ecrã ligado ao serviço (uma "sessão").

Hoje há uma só (o DashDeck pela USB). O motor (`dashd.App`) guarda aqui tudo o que é de um aparelho:
o transporte, a navegação (pilha de telas e páginas), o tamanho e a rotação, e o que já lhe foi enviado
(fundos, ícones, cabeçalho). Os providers são partilhados e ficam no `App`.
Durante a união com o Deck³ (docs/deck3/unificacao.md, fase 2) o `App` continua a expor estes campos como
atributos seus (`app.stack`, `app.esp_w`, ...), para o comportamento e os providers não mudarem.
"""
from __future__ import annotations

from dataclasses import dataclass, field, fields


@dataclass
class Session:
    link: object = None                 # transporte: send(msg, raw=False), acks, jpg_acks
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
