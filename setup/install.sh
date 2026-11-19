#!/usr/bin/env bash
# Instala o lado do uConsole do DashDeck: regra udev (/dev/esp-dash) e serviço systemd do utilizador.
# Pode correr-se várias vezes. Não grava o firmware (ver firmware/flash.sh).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "== dependências Python do sistema (/usr/bin/python3)"
missing=()
/usr/bin/python3 -c "import serial" 2>/dev/null || missing+=(python3-serial)
/usr/bin/python3 -c "import PIL" 2>/dev/null || missing+=(python3-pil)
/usr/bin/python3 -c "import gi; gi.require_version('Gtk', '3.0')" 2>/dev/null || missing+=(python3-gi gir1.2-gtk-3.0)
if [ ${#missing[@]} -gt 0 ]; then
    sudo apt-get install -y "${missing[@]}"
fi

echo "== regra udev -> /dev/esp-dash"
sudo install -m 644 "$ROOT/setup/99-esp-dash.rules" /etc/udev/rules.d/99-esp-dash.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=tty
groups | grep -qw dialout || { sudo usermod -aG dialout "$USER"; echo "   (entre de novo na sessão para o grupo dialout valer)"; }

echo "== serviço systemd --user"
mkdir -p ~/.config/systemd/user
ln -sf "$ROOT/host/dashd.service" ~/.config/systemd/user/dashd.service
systemctl --user daemon-reload
systemctl --user enable --now dashd
systemctl --user --no-pager status dashd | head -5
