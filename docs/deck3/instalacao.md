# Instalação e primeiro arranque

## O que já está pronto no uConsole

- **ESP-IDF v5.5.6** com o toolchain Xtensa do ESP32-S3 (em `~/.espressif`, partilhado com o DashDeck).
  Ativar com `. setup/deck3/idf-env.sh` (o `setup/idf-env.sh` do DashDeck ativa o RISC-V do C6).
- **Componentes** descarregados em `firmware/deck3-s3/managed_components` (BSP oficial da Waveshare,
  LVGL 9.6, WebSocket, mDNS, QMI8658). O firmware compila: `firmware/deck3-s3/flash.sh build`.
- **O serviço do cubo é o `dashd`** (o mesmo do DashDeck; secções `[deckd]`, `[keys]` e `[about]` do
  `host/config.toml`), testável com o aparelho falso (`host/fakedeck.py`).
- Exemplos da Waveshare (só referência, não compilados): `~/esp/ref/wsamoled216`, com o esquema
  elétrico em `schematic/`.

## Dia 1: quando a placa chegar

1. **Ver se a placa funciona** antes de gravar: ligue-a pela USB-C, deve arrancar a demo de fábrica.
2. **Número de série da USB** (o DashDeck tem o mesmo VID:PID 303a:1001):
   ```bash
   ls /dev/ttyACM*                     # a porta nova
   udevadm info -a /dev/ttyACM1 | grep '{serial}' | head -1
   ```
   Dê-o ao instalador (`DECK3_SERIAL=..`). Com o DashDeck também ligado, dê também o da placa C6
   (`DASHDECK_SERIAL=..`), para o `/dev/esp-dash` nunca ser a porta do DashDeck³.
3. **Instalar o lado do uConsole** (serviço, mDNS, udev):
   ```bash
   DECK3_SERIAL=.. setup/install.sh --deck3          # ou --dashdeck --deck3, com os dois
   ```
4. **Gravar o firmware** (a primeira vez tem de ser pela USB):
   ```bash
   DECK3_PORT=/dev/ttyACM1 firmware/deck3-s3/flash.sh   # ou só flash.sh com a regra udev instalada
   firmware/deck3-s3/flash.sh monitor                    # logs (Ctrl+] para sair)
   ```
   Se a gravação não começar: manter **BOOT**, carregar e largar **PWR**/reset, largar BOOT.
5. **Configurar o Wi-Fi e o token** pela USB:
   ```bash
   setup/deck3/provision.py --current               # a rede a que o uConsole está ligado
   setup/deck3/provision.py --nm "Hotspot uConsole" # outras redes guardadas no NetworkManager
   ```
   O token vem de `~/.config/dashd/deck3-token`. A partir daqui o cubo liga-se sozinho: aparece o menu.
6. **Daqui em diante, sem cabo**: `firmware/deck3-s3/flash.sh ota` compila e atualiza pelo Wi-Fi.

### Lista de verificação do hardware (coisas que só se confirmam com a placa)

| Item | Como testar | Se falhar |
|---|---|---|
| Ecrã e toque | Menu aparece; tocar nas boxes | Ver `touch_flags` em `bsp_display_start` (o BSP troca X/Y) |
| Brilho | Botão PWR alterna 20/50/80/100 | O brilho do CO5300 é o comando 0x51 (BSP) |
| Botões BOOT/IO18 | DashDeck³ › página 1 › "Botões" | GPIO0 / GPIO18 em `keys.c` |
| PWR | Toque curto = brilho | Registos 0x41/0x49 do AXP2101 em `power.c` |
| Bateria | Cabeçalho e DashDeck³ › Bateria | `REG_BAT_PCT` 0xA4; sem bateria mostra "sem bateria" |
| Som | DashDeck³ › Bip / Aviso | Volume, PA no GPIO46, `audio.c` |
| Microfone | DashDeck³ › página 2 › Mic on | Ganho (`esp_codec_dev_set_in_gain`) |
| IMU | DashDeck³ › página 2 › IMU on; rodar o cubo | Endereço 0x6B/0x6A; mapear os eixos para "ecrã para cima" |
| Agitar | Agitar com o ecrã apagado | Limiar 2,2 g em `imu.c` |
| Wi-Fi e mDNS | Liga-se sem `--host` | `avahi-browse -r _deck3._tcp` no uConsole; senão `provision.py --host IP:8765`. Se ficar em "fora de casa" com o Wi-Fi verde mas sem chegar a nada, `{"t":"reboot"}` pela USB (ver `firmware/deck3-s3/CLAUDE.md`) |
| OTA | `firmware/deck3-s3/flash.sh ota` | Firewall na porta 8765 |
| Cartão SD | (ainda não usado) | BSP: `bsp_sdcard_mount` |

## Comandos do dia a dia

```bash
systemctl --user status dashd               # estado (o mesmo serviço do DashDeck)
systemctl --user restart dashd              # depois de editar host/config.toml
systemctl --user stop dashd && /usr/bin/python3 host/dashd.py -v    # em primeiro plano
host/deckd.py --deck-port 18765             # só o cubo, noutra porta (testes)
host/fakedeck.py [--port 18765]             # aparelho falso no terminal (tap, btn, back, key ...)
host/deckctl.py status                      # aparelhos ligados
host/deckctl.py notify "Olá" --from uConsole
host/deckctl.py beep 1320 80
```
