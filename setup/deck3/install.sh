#!/usr/bin/env bash
# Instala o lado do uConsole do Deck³: serviço deckd (systemd --user), anúncio mDNS (avahi) e regra udev.
# Pode correr-se várias vezes. Não grava o firmware (firmware/deck3-s3/flash.sh) nem configura o aparelho
# (setup/deck3/provision.py).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

echo "== dependências (/usr/bin/python3: só a biblioteca padrão + pyserial para o provision)"
# PIL: mapas e ícones; gi: ícones do tema GTK (Aplicações); serial: provision.py e o GPS por tty
/usr/bin/python3 -c "import serial, PIL, gi" 2>/dev/null || sudo apt-get install -y python3-serial python3-pil python3-gi

echo "== anúncio mDNS _deck3._tcp (avahi)"
sudo install -m 644 "$ROOT/setup/deck3/deck3.service.xml" /etc/avahi/services/deck3.service
sudo systemctl reload avahi-daemon 2>/dev/null || sudo systemctl restart avahi-daemon

echo "== regra udev -> /dev/deck3"
# O número de série fica fora do git: DECK3_SERIAL=AA:BB:CC:DD:EE:FF setup/deck3/install.sh
if [ -n "$DECK3_SERIAL" ]; then
    sed "s/SERIAL_DO_DECK3/$DECK3_SERIAL/" "$ROOT/setup/deck3/99-deck3.rules" | sudo tee /etc/udev/rules.d/99-deck3.rules >/dev/null
    sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=tty
elif [ -f /etc/udev/rules.d/99-deck3.rules ]; then
    echo "   (já instalada)"
else
    echo "   (sem DECK3_SERIAL: veja-o com  udevadm info -q property /dev/ttyACMx | grep SERIAL_SHORT)"
fi

echo "== firewall: o deckd ouve na porta 8765/tcp"
if command -v ufw >/dev/null && sudo ufw status | grep -q active; then sudo ufw allow 8765/tcp; fi

echo "== serviço systemd --user"
mkdir -p ~/.config/systemd/user
ln -sf "$ROOT/host/deckd.service" ~/.config/systemd/user/deckd.service
ln -sf "$ROOT/host/deck3-sonde.service" ~/.config/systemd/user/deck3-sonde.service   # receptor das Sondas (ligado pelo deckd)
systemctl --user daemon-reload
systemctl --user enable --now deckd
systemctl --user --no-pager status deckd | head -5
echo "Token do aparelho: ~/.config/deck3/token (gravado no Deck³ pelo setup/deck3/provision.py)"
