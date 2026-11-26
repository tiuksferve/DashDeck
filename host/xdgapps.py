"""Lista de aplicações a partir dos arquivos .desktop (o mesmo que o menu do uConsole mostra).

O submenu "Acessórios" do Pi OS (lxde-pi-applications.menu) inclui Category=Utility e exclui
System e Accessibility. Arquivos em ~/.local/share/applications substituem os do sistema.
"""
from __future__ import annotations

import configparser
import os
import time
from pathlib import Path

DIRS = [Path("/usr/share/applications"), Path("/usr/local/share/applications"),
        Path.home() / ".local/share/applications"]   # os últimos substituem os primeiros

ICONS = [  # (categoria ou palavra no nome/exec, ícone)
    ("TextEditor", "edit"), ("FileManager", "folder"), ("Archiving", "drive"), ("Calculator", "list"), ("calc", "list"),
    ("TerminalEmulator", "keyboard"), ("meshtastic", "mail"), ("meshdeck", "mail"), ("sdr", "audio"),
    ("tar1090", "eye"), ("gps", "gps"), ("disk", "sd"), ("etcher", "sd"), ("flipper", "usb"),
    ("github", "upload"), ("reference", "file"), ("diagnos", "warning"), ("servi", "settings"),
    ("pi-apps", "download"),
]

_cache: dict[tuple, tuple[float, list[dict]]] = {}


def _localized(sec, key: str, langs) -> str:
    for lang in langs:
        if sec.get(f"{key}[{lang}]"):
            return sec[f"{key}[{lang}]"]
    return sec.get(key, "")


def _icon(name: str, cats: list[str], exe: str) -> str:
    hay = " ".join(cats + [name.lower(), exe.lower()])
    for key, icon in ICONS:
        if key in cats or key.lower() in hay:
            return icon
    return "play"


def apps(include: str = "Utility", exclude=("System", "Accessibility"), ttl: float = 30) -> list[dict]:
    """Itens de menu (type=launch, desktop=caminho) ordenados pelo nome."""
    key = (include, tuple(exclude))
    hit = _cache.get(key)
    if hit and time.monotonic() - hit[0] < ttl:
        return hit[1]
    files: dict[str, Path] = {}
    for d in DIRS:
        if d.is_dir():
            for f in d.glob("*.desktop"):
                files[f.name] = f
    desktops = set(filter(None, os.environ.get("XDG_CURRENT_DESKTOP", "LXDE:labwc:wlroots").split(":")))
    lang = os.environ.get("LANG", "").split(".")[0]
    langs = ["pt_PT", "pt", lang, lang.split("_")[0]]
    out = []
    for name, f in files.items():
        cp = configparser.ConfigParser(interpolation=None, strict=False)
        cp.optionxform = str
        try:
            cp.read(f, encoding="utf-8")
            sec = cp["Desktop Entry"]
        except Exception:
            continue
        cats = [c for c in sec.get("Categories", "").split(";") if c]
        if sec.get("Type", "Application") != "Application" or include not in cats or \
                any(c in cats for c in exclude):
            continue
        if sec.get("NoDisplay", "false") == "true" or sec.get("Hidden", "false") == "true":
            continue
        only = set(filter(None, sec.get("OnlyShowIn", "").split(";")))
        if only and not only & desktops:
            continue
        if set(filter(None, sec.get("NotShowIn", "").split(";"))) & desktops:
            continue
        label = _localized(sec, "Name", langs) or f.stem
        exe = sec.get("Exec", "")
        out.append({"id": "xdg_" + f.stem.replace(".", "_")[:28], "label": label,
                    "sub": _localized(sec, "Comment", langs) or _localized(sec, "GenericName", langs),
                    "icon": _icon(label, cats, exe), "type": "launch", "desktop": str(f),
                    "app_icon": sec.get("Icon", ""),
                    "exec": exe.split()[0].strip('"') if exe else ""})
    out.sort(key=lambda i: i["label"].lower())
    _cache[key] = (time.monotonic(), out)
    return out


_wm_cache: tuple[float, dict[str, dict]] | None = None


def by_app_id(app_id: str, ttl: float = 60) -> dict | None:
    """O .desktop de uma janela (app_id do Wayland): pelo nome do ficheiro, pelo StartupWMClass ou
    pelo executável. Devolve {"label", "icon"} ou None."""
    global _wm_cache
    if not app_id:
        return None
    if not _wm_cache or time.monotonic() - _wm_cache[0] > ttl:
        idx: dict[str, dict] = {}
        lang = os.environ.get("LANG", "").split(".")[0]
        langs = ["pt_PT", "pt", lang, lang.split("_")[0]]
        for d in DIRS:
            if not d.is_dir():
                continue
            for f in d.glob("*.desktop"):
                cp = configparser.ConfigParser(interpolation=None, strict=False)
                cp.optionxform = str
                try:
                    cp.read(f, encoding="utf-8")
                    sec = cp["Desktop Entry"]
                except Exception:
                    continue
                info = {"label": _localized(sec, "Name", langs) or f.stem, "icon": sec.get("Icon", "")}
                exe = Path((sec.get("Exec", "").split() or [""])[0].strip('"')).name
                for k in (f.stem, f.stem.rsplit(".", 1)[-1], sec.get("StartupWMClass", ""), exe):
                    if k:
                        idx.setdefault(k.lower(), info)
                idx[f.stem.lower()] = info   # o nome do ficheiro ganha
        _wm_cache = (time.monotonic(), idx)
    key = app_id.lower()
    return _wm_cache[1].get(key) or _wm_cache[1].get(key.rsplit(".", 1)[-1])
