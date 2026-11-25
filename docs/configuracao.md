# Configuração (`host/config.toml`)

Tudo o que aparece na tela vem deste arquivo. Depois de editá-lo:

```bash
systemctl --user restart dashd
```

Não é preciso regravar o ESP. Se o TOML tiver um erro, o serviço não arranca; veja a causa com
`systemctl --user status dashd`.

## `[dashd]`

| Chave | Padrão | Descrição |
|---|---|---|
| `port` | `/dev/esp-dash` | Porta serial do ESP |
| `title` | `DashDeck` | Título do menu principal |
| `refresh` | `2.0` | Segundos entre atualizações da tela aberta (só é enviado o que mudou) |
| `backlight` | `80` | Brilho inicial, de 0 a 100 |
| `layout` | `grid` | Menus em `grid` (boxes) ou `cards` (lista de cartões). Cada submenu aceita o seu `layout` |
| `rotation` | `1` | 0 retrato, 1 horizontal, 2 retrato invertido, 3 horizontal invertido. O ESP grava o valor e reinicia sozinho. A rotação escolhida no menu (Sistema > DashDeck) fica em `~/.config/dashd/state.json` e passa à frente desta; apague esse ficheiro para voltar a usar a do config |
| `screen_off` | `0` | Apaga o ecrã após N segundos sem toques na tela inicial (no config: 180). Um toque ou o BOOT liga-o, sem ativar nada. `0` = nunca |
| `dock_screens` | `["home"]` | Ids das telas que mostram o dock (no config: `["home", "apps"]`: tela inicial e Aplicações) |

## Itens de menu: `[[menu]]` e `[[menu.items]]`

Campos comuns:

| Campo | Descrição |
|---|---|
| `id` | Identificador único (obrigatório) |
| `label` | Texto do box |
| `sub` | Subtítulo fixo. Os providers e serviços põem aqui o seu estado |
| `icon` | Símbolo: `home settings wifi gps play stop pause list bell power refresh eye charge battery usb bluetooth sd folder file mail audio video image warning bars keyboard call upload download loop edit trash ok close plus drive shuffle right volume` |
| `img` | Imagem no lugar do símbolo: `"emoji:✈"`, um nome do tema de ícones (ex.: `"utilities-terminal"`) ou um caminho `.png` ou `.svg` |
| `color` | Cor do símbolo, `"#RRGGBB"` |
| `type` | Um dos tipos abaixo |

| `type` | Campos | O que faz |
|---|---|---|
| `submenu` | `items`, `layout`, `xdg_category`, `xdg_exclude` | Abre outro menu. Com `xdg_category`, junta as aplicações dos `.desktop` dessa categoria |
| `launch` | `cmd`, `app_icon` | Abre um programa no uConsole. O ícone vem de `app_icon` ou do nome do comando |
| `shell` | `cmd`, `done` | Corre um comando em segundo plano e mostra `done` (ou o código de erro) |
| `service` | `unit`, `group`, `user`, `restart_only`, `confirm_restart`, `note` | Tela de um serviço systemd. Com `group`, ligar um serviço pára os outros do mesmo grupo (e não há "Auto no boot"). `user = true`: serviço do utilizador (`systemctl --user`, sem sudo). `restart_only = true`: só Reiniciar (ex.: o próprio dashd). `note`: linha de texto no detalhe |
| `provider` | `provider` e as opções do provider | Tela com dados ao vivo (ver abaixo) |
| `text` | `text` | Tela de texto fixo |
| `home` | — | Volta ao menu principal (útil no dock) |
| `backlight` | `levels` | Alterna o brilho pelos níveis indicados |

### Aplicações de Acessórios

```toml
[[menu]]
id = "apps"
type = "submenu"
xdg_category = "Utility"                    # categoria dos .desktop
xdg_exclude = ["System", "Accessibility"]   # igual ao menu Acessórios do Pi OS
# xdg_after = true                          # pôr os itens fixos antes das apps
```

Os itens fixos (`[[menu.items]]`) cujo comando já esteja num `.desktop` não aparecem repetidos.
Os itens `provider` (ex.: **Abertas**) ficam sempre à frente dos programas.

## Providers

### `system`
Não tem opções. A segunda página (DashDeck) muda a rotação do ESP.

### `windows`
Programas abertos no uConsole (ver [uso.md](uso.md#aplicações)), no box **Abertas** das Aplicações. Usa o
protocolo `wlr-foreign-toplevel-management` do compositor (labwc e outros wlroots), sem programas extra.
O nome e o ícone vêm do `.desktop` com o mesmo `app_id` (nome do ficheiro, `StartupWMClass` ou executável).

| Opção | Padrão | Descrição |
|---|---|---|
| `icons` | `{}` | Imagem para programas sem `.desktop`: `{ "app_id" = "nome do tema, caminho ou emoji:…" }` |
| `hide` | `[]` | `app_id` que não aparecem na lista |

### `gps`
| Opção | Padrão | Descrição |
|---|---|---|
| `tty` | `/dev/ttyAMA0` | Porta NMEA, usada quando o meshtasticd não está usando o GPS |
| `baud` | `9600` | Velocidade da porta |
| `gpsmux` | `127.0.0.1:10110` | NMEA partilhado pelo gpsmux do MeshDeckWeb; tem prioridade sobre o `tty` quando responde (`""` = usar sempre o tty) |

### `adsb`
| Opção | Padrão | Descrição |
|---|---|---|
| `unit` | `readsb` | Serviço que o card do topo liga e desliga |
| `group` | `sdr` | Grupo exclusivo usado ao ligar |
| `keep_running` | `false` | `true` mantém o serviço ligado ao sair da tela. Por omissão, o que o DashDeck ligou é desligado ao sair, para libertar o SDR |
| `json` | (procura) | Caminho do `aircraft.json`. Sem ele, tenta o readsb e o dump1090 em `/run` |
| `map` | `true` | `false` desliga o fundo do mapa |
| `map_style` | `simples` | `simples` (Esri Dark Gray), `satelite` (Esri com nomes) ou `osm` (OpenStreetMap escurecido) |
| `map_url` | — | Outro servidor de tiles `https://.../{z}/{x}/{y}.png` (substitui o estilo) |
| `track_minutes` | `30` | Minutos de trajeto guardados por aeronave |
| `squawk_alert` | `true` | Mostra o cartão **Monitor de emergências** (aviso animado em qualquer tela com 7500, 7600 ou 7700; o readsb fica ligado enquanto o monitor estiver ligado). `false` esconde-o |
| `squawk_secs` | `12` | Segundos que o aviso de emergência fica no ecrã |
| `routes` | `true` | Origem e destino de cada voo pelo indicativo no adsbdb.com (e no hexdb.io, se faltar; só o indicativo sai do uConsole; cache em `~/.cache/dashd/routes.json`). `false` desliga |

### `ais`
| Opção | Padrão | Descrição |
|---|---|---|
| `unit` | `ais-catcher` | Serviço que o card do topo liga e desliga |
| `group` | `sdr` | Grupo exclusivo usado ao ligar |
| `keep_running` | `false` | `true` mantém o serviço ligado ao sair da tela. Por omissão, o que o DashDeck ligou é desligado ao sair, para libertar o SDR |
| `url` | `http://127.0.0.1:8119` | Servidor web do AIS-catcher (`/api/ships.json` e `/api/allpath.json`) |
| `map`, `map_style`, `map_url` | | Iguais aos do `adsb` |

### `sonde`
Radiossondas RS41 (ver [uso.md](uso.md#sondas)).

| Opção | Padrão | Descrição |
|---|---|---|
| `unit` | `dashd-sonde` | Serviço `--user` do receptor (`host/dashd-sonde.service`, que corre `host/sonderx.py`) |
| `group` | `sdr` | Serviços do sistema parados ao ligar (readsb, ais-catcher) |
| `gain`, `ppm` | `40`, `0` | Ganho do RTL-SDR em dB (0 = automático) e correção de frequência |
| `idle_stop` | `1800` | Segundos sem pacotes, depois de um voo, até o receptor sair e libertar o SDR |
| `decoder` | o `rs41mod` do SondeDeck | Caminho do descodificador |
| `keep_running` | `false` | `true` mantém o receptor ligado ao sair da tela |
| `freqs` | Lisboa | Lista `{ mhz, name }` mostrada na primeira página, pela ordem dada |
| `map`, `map_style`, `map_url` | | Iguais aos do `adsb` |

O dashd escreve a frequência e as opções em `~/.config/dashd/sonde.env` antes de ligar o serviço.

### `camera`
Câmara do CamDeck (ver [uso.md](uso.md#câmara)). O dashd fala com o `camd` pela API local e entra com o
token de `~/.config/camdeck/local-token` (criado pelo camd, só aceite em ligações de 127.0.0.1); não
precisa da palavra-passe.

| Opção | Padrão | Descrição |
|---|---|---|
| `url` | `http://127.0.0.1:8090` | Endereço do camd |
| `quality` | `70` | Qualidade JPEG do visor enviado ao ESP (o quadro já vai reduzido ao tamanho do canvas, ~4-8 KB) |

### `stocks`
Cotações de ações (ver [uso.md](uso.md#stocks)).

| Opção | Padrão | Descrição |
|---|---|---|
| `symbols` | — | Lista de `{ sym = "AAPL", name = "Apple" }` com o símbolo do Yahoo Finance (ex.: `SIE.DE` na Xetra, `TSLA` na Nasdaq) e o nome no box. `logo_crop` (0 a 1) guarda só essa fração da largura do logotipo, da esquerda (a SpaceX usa 0.65: o X com a curva encurtada) |
| `refresh` | `60` | Segundos entre atualizações com a tela aberta (só com o box do menu: 5 min) |

### `sdr`
Monitor do RTL-SDR (ver [uso.md](uso.md#sdr)). Sem opções. Usa `sudo -n fuser` para ver quem tem o dongle aberto e, no "Libertar SDR", `sudo -n systemctl stop` ou `sudo -n kill`.

### `aio`
Interruptores da placa AIO v2 pelo `aiov2_ctl` (a aplicação AIO v2 Controller): o estado vem de
`aiov2_ctl --status` e cada toque corre `aiov2_ctl <GPS|LORA|SDR|USB> on|off`.

| Opção | Padrão | Descrição |
|---|---|---|
| `cmd` | `/usr/local/bin/aiov2_ctl` | Caminho do aiov2_ctl. O utilizador precisa de estar no grupo `gpio` (o `pinctrl` não usa sudo); o LoRa usa `sudo -n systemctl` para o meshtasticd |
| `profiles` | Meshtastic, ADS-B, AIS, Poupança | Perfis de uso, em `[[menu.profiles]]` (ver abaixo) |

Cada perfil:

```toml
  [[menu.profiles]]
  id = "adsb"
  label = "ADS-B"
  img = "emoji:✈"
  on = ["GPS", "SDR"]        # recursos a ligar (GPS LORA SDR USB)
  off = []                   # recursos a desligar
  start = ["readsb"]         # unidades systemd a ligar (com grupo, param as outras do grupo)
  stop = []                  # unidades systemd a parar
```

Ao aplicar, a ordem é: parar serviços, desligar, ligar e, por fim, ligar os serviços (com 3 s de espera
depois de dar energia ao SDR ou ao USB, para o dongle aparecer). O que já está como no perfil não é mexido.

### `net`
Rede pelo NetworkManager (`nmcli`; ver [uso.md](uso.md#rede)). Sem opções. O utilizador precisa de
poder controlar a rede sem palavra-passe (no Raspberry Pi OS, os grupos `netdev` ou `sudo` já podem;
confirme com `nmcli general permissions`). O Tailscale entra se o comando `tailscale` existir.

### `shares`
Box Partilhas (ver [uso.md](uso.md#partilhas)). Opções: `units` (serviços ligados e desligados juntos,
por omissão `["smbd", "wsdd"]`) e `host` (nome mostrado no endereço, o `netbios name` do Samba).

A partilha em si configura-se fora do DashDeck:

1. `sudo apt install samba wsdd` (o `wsdd` mostra o uConsole na "Rede" do Windows).
2. Em `/etc/samba/smb.conf`: no `[global]`, `server min protocol = SMB2_10`, `map to guest = never`,
   `hosts allow = 127. 192.168.1. fe80::/10` e `hosts deny = ALL` (só a rede de casa, adaptar à rede);
   uma secção por pasta com `path`, `valid users = <utilizador>` e `read only = no`.
3. `sudo smbpasswd -a <utilizador>`: a palavra-passe de rede (separada da do login).
4. Opcional: `/etc/avahi/services/smb.service` para o Mac e o Linux verem o uConsole na rede.

O dashd usa `sudo -n` para `systemctl enable/disable --now` e para `smbstatus` (aparelhos ligados).

### `console`
Volume e brilho do ecrã do uConsole (ver [uso.md](uso.md#som)); no config de origem está no dock.

| Opção | Padrão | Descrição |
|---|---|---|
| `volume_step` | `5` | % por toque no - e no + da página Som (`wpctl` no PipeWire) |
| `volume_max` | `100` | Limite do + |
| `backlight` | o primeiro de `/sys/class/backlight` | Pasta do backlight. A escrita precisa do grupo `video`; sem permissão, tenta o `brightnessctl` |

### `clock`
Relógio em ecrã inteiro (ver [uso.md](uso.md#relógio)). Pode ser um `[[menu]]` ou um `[[dock]]`; no config de origem está no dock, no lugar do Terminal.

| Opção | Padrão | Descrição |
|---|---|---|
| `face` | `paisagem` | Mostrador inicial: `paisagem`, `retro`, `previsao`, `cidade` ou `temporizador`. Depois de um swipe, vale o último usado (guardado em `~/.config/dashd/clock.json`, com o brilho, a cor, o temporizador e o alarme) |
| `lcd_color` | `ambar` | Cor inicial do digital retro: `ambar`, `verde` ou `ciano` (depois, o toque longo muda-a) |
| `brightness` | `50` | Brilho inicial no relógio (10 a 100). O brilho normal volta ao sair |
| `dim_after` | `300` | Segundos sem toques até o relógio escurecer (`0` = nunca). Um toque repõe o brilho |
| `dim_brightness` | `10` | Brilho quando escurecido (%) |
| `weather` | `true` | Previsão do Open-Meteo (sem chave de API). `false` desliga os pedidos à internet |
| `winter_snow` | `true` | Paisagem e cidade com neve no chão de dezembro a fevereiro |
| `alarm_sound` | `true` | Bip no uConsole quando o alarme ou o temporizador tocam (`pw-play`, `paplay` ou `aplay`; o som fica em `~/.cache/dashd/alarme.wav`) |
| `lat`, `lon` | — | Posição fixa da previsão. Sem elas, usa o GPS ou o nó Meshtastic e, sem posição, a última da cache |

As coordenadas são arredondadas a 0,01° (cerca de 1 km) antes de serem enviadas ao Open-Meteo. A
previsão fica em `~/.cache/dashd/weather.json`.

### `mesh`
| Opção | Padrão | Descrição |
|---|---|---|
| `channel` | `0` | Canal inicial das respostas rápidas (o destino troca-se na tela) |
| `quick` | `["OK", "A caminho", "Cheguei", "Preciso de ajuda"]` | Frases do Responder, do Escrever e da Mensagem direta, até 12. `{pos}` é trocado pela posição do nó |
| `start_cmd` | — | Comando do botão "Iniciar MeshDeckWeb" quando a ponte não responde |

## Dock: `[[dock]]`

Até 6 botões na parte de baixo do menu principal. Aceitam os mesmos campos e tipos dos itens de menu:

```toml
[[dock]]
id = "shot"
label = "Captura"
icon = "image"
type = "shell"
cmd = "grim \"$HOME/Pictures/captura-$(date +%Y%m%d-%H%M%S).png\""
done = "Captura em ~/Pictures"
```

## Exemplos

Um serviço novo no grupo do SDR:

```toml
  [[menu.items]]
  id = "svc_rtltcp"
  label = "rtl_tcp"
  icon = "audio"
  type = "service"
  unit = "rtl_tcp"
  group = "sdr"
```

Um comando com confirmação de sucesso:

```toml
  [[menu.items]]
  id = "wifi_off"
  label = "Wi-Fi off"
  icon = "wifi"
  type = "shell"
  cmd = "nmcli radio wifi off"
  done = "Wi-Fi desligado"
```
