#!/usr/bin/env bash
# Instala o lado do uConsole do DashDeck e do DashDeck³: um só serviço (dashd, systemd --user) para os dois ecrãs.
#
#   setup/install.sh                    pergunta que aparelhos há
#   setup/install.sh --dashdeck         só o DashDeck (ESP32-C6 pela USB)
#   setup/install.sh --deck3            só o DashDeck³ (ESP32-S3 por Wi-Fi)
#   setup/install.sh --dashdeck --deck3 os dois
#
# Números de série (para as regras udev, fora do git; vê-los com
#   udevadm info -q property /dev/ttyACMx | grep SERIAL_SHORT):
#   DASHDECK_SERIAL=..  fixa /dev/esp-dash à placa do DashDeck (precisa, se o DashDeck³ também for à USB)
#   DECK3_SERIAL=..     cria /dev/deck3 para o DashDeck³ (provisionamento e firmware pela USB)
#
# Pode correr-se várias vezes. Não grava firmware (firmware/flash.sh, firmware/deck3-s3/flash.sh) nem configura
# o cubo (setup/deck3/provision.py). Se o serviço antigo do DashDeck³ (deckd) estiver instalado, desliga-o.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DASH=""; DECK=""
for a in "$@"; do
    case "$a" in
        --dashdeck) DASH=1 ;;
        --deck3) DECK=1 ;;
        *) echo "opção desconhecida: $a" >&2; exit 2 ;;
    esac
done
ask() { local r; read -r -p "$1 [s/N] " r; [[ "$r" =~ ^[sSyY] ]]; }
if [ -z "$DASH$DECK" ]; then
    ask "Há um DashDeck (ESP32-C6) ligado pela USB?" && DASH=1
    ask "Há um DashDeck³ (ESP32-S3, cubo) por Wi-Fi?" && DECK=1
fi
[ -n "$DASH$DECK" ] || { echo "Nenhum aparelho escolhido: nada a fazer."; exit 0; }

echo "== dependências Python do sistema (/usr/bin/python3)"
missing=()
/usr/bin/python3 -c "import serial" 2>/dev/null || missing+=(python3-serial)
/usr/bin/python3 -c "import PIL" 2>/dev/null || missing+=(python3-pil)
/usr/bin/python3 -c "import gi; gi.require_version('Gtk', '3.0')" 2>/dev/null || missing+=(python3-gi gir1.2-gtk-3.0)
if [ ${#missing[@]} -gt 0 ]; then
    sudo apt-get install -y "${missing[@]}"
fi
groups | grep -qw dialout || { sudo usermod -aG dialout "$USER"; echo "   (entre de novo na sessão para o grupo dialout valer)"; }

udev_reload() { sudo udevadm control --reload-rules; sudo udevadm trigger --subsystem-match=tty; }

if [ -n "$DASH" ]; then
    echo "== DashDeck: regra udev -> /dev/esp-dash"
    if [ -n "$DASHDECK_SERIAL" ]; then
        sed "s/ATTRS{idProduct}==\"1001\",/ATTRS{idProduct}==\"1001\", ATTRS{serial}==\"$DASHDECK_SERIAL\",/" \
            "$ROOT/setup/99-esp-dash.rules" | sudo tee /etc/udev/rules.d/99-esp-dash.rules >/dev/null
    elif [ -n "$DECK" ] && [ -f /etc/udev/rules.d/99-esp-dash.rules ]; then
        echo "   (mantida a que existe: com o DashDeck³ também na USB, ela deve ter o ATTRS{serial} do DashDeck)"
    else
        sudo install -m 644 "$ROOT/setup/99-esp-dash.rules" /etc/udev/rules.d/99-esp-dash.rules
        [ -n "$DECK" ] && echo "   aviso: sem DASHDECK_SERIAL, o DashDeck³ ligado à USB também apareceria como /dev/esp-dash"
    fi
    udev_reload
fi

if [ -n "$DECK" ]; then
    echo "== DashDeck³: anúncio mDNS _deck3._tcp (avahi)"
    sudo install -m 644 "$ROOT/setup/deck3/deck3.service.xml" /etc/avahi/services/deck3.service
    sudo systemctl reload avahi-daemon 2>/dev/null || sudo systemctl restart avahi-daemon
    echo "== DashDeck³: regra udev -> /dev/deck3"
    if [ -n "$DECK3_SERIAL" ]; then
        sed "s/SERIAL_DO_DECK3/$DECK3_SERIAL/" "$ROOT/setup/deck3/99-deck3.rules" | sudo tee /etc/udev/rules.d/99-deck3.rules >/dev/null
        udev_reload
    elif [ -f /etc/udev/rules.d/99-deck3.rules ]; then
        echo "   (já instalada)"
    else
        echo "   (sem DECK3_SERIAL: só é precisa para provisionar e gravar pela USB)"
    fi
    echo "== DashDeck³: firewall (porta 8765/tcp)"
    if command -v ufw >/dev/null && sudo ufw status | grep -q active; then sudo ufw allow 8765/tcp; fi
    echo "== DashDeck³: estado antigo (~/.config/deck3 -> ~/.config/dashd, cópia)"
    (cd "$ROOT/host" && /usr/bin/python3 -c "import paths; [print('  ', d) for d in paths.migrate_deck3()]")
fi

echo "== serviço antigo do DashDeck³ (deckd)"
if [ -e ~/.config/systemd/user/deckd.service ] || [ -e ~/.config/systemd/user/deck3-sonde.service ]; then
    systemctl --user disable --now deckd deck3-sonde 2>/dev/null || true
    rm -f ~/.config/systemd/user/deckd.service ~/.config/systemd/user/deck3-sonde.service
    echo "   desligado e removido (o dashd serve agora o DashDeck³)"
else
    echo "   não está instalado"
fi

echo "== serviço systemd --user (dashd)"
mkdir -p ~/.config/systemd/user/dashd.service.d
ln -sf "$ROOT/host/dashd.service" ~/.config/systemd/user/dashd.service
# receptor das Sondas (ligado e desligado pelo dashd, não arranca no boot)
ln -sf "$ROOT/host/dashd-sonde.service" ~/.config/systemd/user/dashd-sonde.service
args=""
[ -z "$DASH" ] && args="$args --no-serial"
[ -z "$DECK" ] && args="$args --no-deck"
printf '[Service]\nEnvironment=DASHD_ARGS=%s\n' "${args# }" > ~/.config/systemd/user/dashd.service.d/aparelhos.conf
echo "   aparelhos: ${DASH:+DashDeck }${DECK:+DashDeck³}"
systemctl --user daemon-reload
systemctl --user enable dashd
systemctl --user restart dashd
systemctl --user --no-pager status dashd | head -5
[ -n "$DECK" ] && echo "Token do DashDeck³: ~/.config/dashd/deck3-token (grave-o no cubo com setup/deck3/provision.py)"
exit 0
