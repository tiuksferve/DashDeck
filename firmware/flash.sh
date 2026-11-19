#!/usr/bin/env bash
# Compila e grava o DashDeck. Pára o dashd enquanto grava (ele ocupa a porta).
set -e
cd "$(dirname "$0")"
. ~/esp/idf-env.sh   # cópia em setup/idf-env.sh
BUILD=/dev/shm/dashdeck-build          # build na RAM: poupa o cartão (é refeito após reboot)
systemctl --user stop dashd 2>/dev/null || true
trap 'systemctl --user start dashd 2>/dev/null || true' EXIT
if [ $# -gt 0 ]; then
    idf.py -B "$BUILD" -p /dev/esp-dash "$@"
    exit
fi
# Gravação normal; se a USB cortar dados ("Packet content transfer stopped"), repete sem o stub e a
# 115200 baud, que é mais lento mas aguenta melhor uma ligação instável.
idf.py -B "$BUILD" -p /dev/esp-dash flash && exit 0
echo "Gravação normal falhou; a repetir no modo seguro (sem stub, 115200)..."
cd "$BUILD"
for i in 1 2 3 4 5; do
    esptool.py --chip esp32c6 -p /dev/esp-dash -b 115200 --no-stub --before default_reset --after hard_reset \
        write_flash @flash_args && exit 0
    sleep 3
done
exit 1
