# Protocolo

O DashDeck³ fala o protocolo do DashDeck (um objeto JSON por mensagem, campo `t`), com extensões.
Os providers do DashDeck geram telas que o DashDeck³ já entende (`menu`, `list`, `kv`, `text`, `buttons`).

## Transportes

| Transporte | Formato | Uso |
|---|---|---|
| **Wi-Fi** (normal) | WebSocket `ws://<uConsole>:8765/deck?id=<mac>&token=<token>&fw=<versão>`, um JSON por frame de texto | Tudo |
| **USB** (reserva) | USB-Serial-JTAG, uma linha JSON por mensagem; logs do ESP como linhas `# ...` | Configurar (Wi-Fi, token, host) e depurar |

As respostas do aparelho vão pelo transporte de onde chegou a última mensagem do host.
Sem host em nenhum transporte, o aparelho envia `hello` pela USB a cada 3 s.

**Como o aparelho encontra o deckd** (por esta ordem): endereço fixo (`host`, se configurado) → mDNS
`_deck3._tcp` (o avahi do uConsole anuncia-o, `setup/deck3.service.xml`) → último endereço que
funcionou → gateway da rede na porta 8765 (o uConsole em modo hotspot).

**Segurança:** o token (32 hex, `~/.config/deck3/token`) vai no URL. O deckd recusa tokens errados
(403). A ligação não é cifrada: numa rede pública, use o hotspot do próprio uConsole.

## Host → aparelho

| `t` | Campos | Efeito |
|---|---|---|
| `screen` | `id`, `tpl`, `title`, `back`, `pages`, `page`, dados do template, `buttons` | Nova tela |
| `update` | `id` + os campos que mudaram | Junta à tela aberta (se o `id` for o mesmo) e redesenha mantendo o scroll |
| `toast` | `text`, `level` (`ok` `warn` `err` `info`) | Aviso em fila, um de cada vez (2,5 a 7 s, conforme o tamanho do texto); repetidos são ignorados e esperam enquanto houver uma notificação |
| `bg` | `key`, `off`, `d` (base64); no 1.º pedaço `w`, `h`, `pal` (16 ou 256 cores), `bpp` (8; senão 4); no último `last` | Imagem de fundo, como no DashDeck: os tiles dos mapas (4 bpp, usados pelo `plot` com o mesmo `bg`) e a paisagem do relógio (8 bpp, 240×240). Cada pedaço é confirmado com `{"t":"bgack","off":n,"ok":bool}`; fica na PSRAM (uma por profundidade) |
| `clkface` | `v` | Muda a face do relógio (testes e capturas; normalmente é o swipe) |
| `bootanim` | — | Repete a sequência de arranque e volta à tela atual (testes e capturas) |
| `pomo` | `show` (true/false), `z` (1 iniciar/pausar, 2 reiniciar, 3 pular), `left` (s) | Ecrã do Pomodoro sem girar o cubo (testes) |
| `clkzone` | `v` (zona `CZ_*` de `anim.h`) | Simula um toque no temporizador ou no alarme do cubo (testes) |
| `varanda` | `wx` (`rain` `storm` `snow` ou ""), `sun` ([nascer, pôr] em minutos), `planes` ([{`id`, `alt` m, `dist` km}]), `boats` ([{`name`, `dist` milhas}]), `reload` (imagens novas no cartão); testes: `show` (true/false), `mode` (`day` `dusk` `night` `storm` `auto`), `ev` (`knock` `shake` `clap` `noise` `charge` `pet`) | Varanda do Romeu (cubo em pé girado à esquerda). O dashd manda os dados a cada 8 s enquanto está aberta; o cubo avisa com `{"t":"varanda","open":"1"}` (`"need":"1"` se faltarem as imagens) e `"open":"0"` ao fechar |
| `perf` | — | Desempenho do ecrã desde o pedido anterior: responde `{"t":"perf","fps","refr_ms","draw_ms","wait_ms","secs"}` (testes) |
| `dbg` | — | Estado dos quadros desenhados em segundo plano e do trinco do `anim.c`; responde mesmo com o LVGL ocupado (diagnóstico) |
| `shot` | | Captura do ecrã: o aparelho redesenha tudo e responde com `shotpx` (`x`, `y`, `w`, `h`, `d` = RGB565 em base64, por faixas) e `shotend` (`w`, `h`); o dashd monta o PNG (`deckctl.py shot [ficheiro]`) |
| `notify` | `from`, `text`; `title`, `kind` (`alarm`, `squawk`), `sprite` (`mail` `clock` `plane`; sem ele, o do tipo), `dur` (s; sem ele, 6 a 20 s conforme o texto), `sound` (false = sem som) | Aviso por cima de tudo: sprite de 8 bits que cai e assenta e depois balança, abana ou pisca; som. Em fila: a seguinte só aparece quando a atual fecha. Um toque responde `{"t":"notify","act":"open"}` |
| `time` | `epoch` (UTC), `tz` (regra POSIX, ex.: `WET0WEST,M3.5.0/1,M10.5.0`) | Acerta o relógio (há também SNTP quando há Internet) |
| `bl` | `v` 0..100 | Brilho (fica guardado no aparelho); 0 desliga o ecrã até um toque, que o acende no brilho anterior e responde `{"t":"bl","v":n}` |
| `idle` | `dim`, `off` (s; 0 = nunca) | Escurece e depois apaga sem toques. Um toque, um botão ou agitar acorda (esse toque não abre nada) |
| `wake` | — | Acende o ecrã |
| `beep` | `f` (Hz), `ms`, `vol` | Tom no altifalante |
| `tones` | `seq` `[[f, ms], ...]` (f = 0 é pausa), `vol` | Sequência de tons |
| `vol` | `v` 0..100 | Volume (fica guardado no aparelho) |
| `bat?` | `n` (pontos do histórico, 120) | Pede `bat` |
| `bat_cfg` | `icc` (mA: 100, 200, 300, 400, 500) | Corrente de carga do AXP2101 (guardada; de fábrica 25 mA). Responde `bat` |
| `reboot` | — | Reinicia o aparelho |
| `about` | `lines` (créditos), `host` (versão do host: commit curto, "+" com alterações), `show` (abre já) | Cena "Sobre o DashDeck³" (toque no título da tela inicial): salto no hiperespaço até um planeta com anéis, com os créditos por cima (por CT7BRA, endereço do projeto, versões do firmware e do host e estas linhas), que aparecem depois do salto e somem ao fim de 7 s. O dashd envia-a no `hello` a partir de `[about]` |
| `sd?` | — | Pede `sd` |
| `ls` | `path` (relativo a `deck3/`) | Lista uma pasta do cartão; responde `ls` |
| `file` | `path`, `data` (base64), `append` | Escreve no cartão (`deckctl.py put` parte os ficheiros em pedaços de 3 KB); responde `file` |
| `rm` | `path` | Apaga um ficheiro ou pasta vazia; responde `rm` |
| `mic` | `hz` (0 = parar) | Liga o envio do nível do microfone |
| `imu` | `stream` (Hz, 0 = parar), `face?` (true) | IMU ao vivo / pede a face atual |
| `ota` | `url` | Atualiza o firmware por HTTP (o deckd serve `/fw/deck3.bin`) |
| `ping` | — | Responde `pong` |
| `hello?` | — | Pede um `hello` |
| `stats` | — | Pede `stats` |
| `rot` | `v` 0..3 | Rotação do painel (experimental: o toque ainda não acompanha) |
| `offline` | — | Mostra o ecrã de arranque |

### Configuração (USB; `wifi` também pela rede)

| `t` | Campos | Efeito |
|---|---|---|
| `wifi` | `ssid`, `pass` · `del` · `clear` · `scan` | Acrescenta/atualiza (até 8; sai a mais antiga), apaga uma ou todas; `scan: true` procura redes (com o Wi-Fi ligado) e responde `wifi_scan` |
| `token` | `v` | Token do deckd (só USB) |
| `host` | `addr` (`ip:porta`, `""` = automático) | Endereço fixo do deckd (só USB) |
| `cfg?` / `wifi?` | — | Responde `cfg` |

Resposta: `{"t":"cfg","ok":true,"nets":[...],"state":"...","ssid","ip","host","token":true,"id"}`.

### Templates

| `tpl` | Dados |
|---|---|
| `menu` | `items`: `id`, `label`, `sub`, `color` (da placa de reserva), `img` (ícone do cartão: `icons/<id>.bin`, imagem do LVGL ARGB8888 84×84 que o dashd envia uma vez com `file` e o aparelho guarda na PSRAM; ou um PNG/JPG posto à mão; se o ficheiro não existir, usa o modelo), `icon` (nome do DashDeck: o dashd troca-o por um emoji ou, no `mail`, por um envelope de 8 bits na cor da box, `pix:mail:<cor>`), `color` (tinge a cor 1 do modelo), `badge` (selo vermelho no canto, texto ou número: ex. mensagens não lidas), `value`, `value_color`, `sub_color`, `dot`, `bg`, `confirm`. Boxes 140×164 em 3 colunas |
| `list` | `items`: `id`, `label`, `sub`, `right`, `color`, `bg`, `confirm`; `empty` |
| `kv` | `rows`: `[chave, valor, cor?]` (cor `#rrggbb`, `ok`, `warn`, `err`, `sub`, `accent`) |
| `text` | `text` |
| `clock` | `clock`: o do DashDeck (`wx`, `icon`, `bg`, `anim`, `night`, `pet`). Ecrã inteiro com as faces do cubo: paisagem (a cena `bg` com chuva/neve e o cão), aurora boreal, o cubo, temporizador e alarme. Swipe troca a face (fica no cubo); um toque volta, menos nas zonas do temporizador e alarme. O temporizador e o alarme são só do cubo (guardados na NVS, contados mesmo sem uConsole e tocados na coluna do cubo); os do DashDeck tocam só no C6 e no uConsole |
| `panel` | só os extras abaixo |
| `image` | `src` (ficheiro do cartão, PNG ou JPG, ex. `scenes/praia.jpg`), `h` (altura, 320), `caption` |
| todos | `bg_img`: imagem de fundo do cartão (cobre o corpo da tela). `title` com " · " mostra a parte de depois mais pequena e a cinzento ("Config · Ecrã e som") |
| todos | `buttons`: `id`, `label`, `color`, `confirm` |
| todos | `sliders`: `id`, `label`, `v`, `min`, `max`, `step`, `unit`, `color`, `live` (`"bl"` = aplica o brilho enquanto se arrasta). Ao largar: `slider` |
| todos | `chart`: objeto ou lista de `{title, value, value_color, min, max, h, labels:[esq, dir], series:[{color, points}]}` (linhas; a série mais curta acaba à direita) |

Ordem no ecrã: sliders → gráficos → corpo do template → botões.

Ainda não portados do DashDeck: `plot` (mapas, céu do GPS, gráficos), `log`, `clock`, `jpg`, `bg`, `icon`, `emoji`, `dock`.

**Cartão SD:** montado em `/sdcard`; os ficheiros do DashDeck³ ficam em `deck3/`. Enviar um ficheiro:
`host/deckctl.py put <local> <caminho em deck3/>`.

**Pastas do cartão:** `icons/`, `scenes/` (imagens), `log/bat.bin` (bateria: 8 bytes por
minuto, `epoch` u32, `mV` u16, `%` i8, flags u8; os últimos 720 voltam a carregar no arranque),
`log/pendentes.jsonl` (eventos ainda por entregar).

**Fora de casa:** sem o deckd há 15 s (ou 30 s no arranque), o cubo mostra um relógio em pixel art com a data, a
bateria em blocos e os eventos guardados (a posição desloca-se a cada minuto). Os eventos `key`, `imu` (face e
agitar) e `bat_low` que não tenham quem os ouça ficam no cartão e seguem no `offline_log` quando voltar.

## Aparelho → host

| `t` | Campos | Quando |
|---|---|---|
| `hello` | `dev:"deck3"`, `fw`, `id`, `w`, `h`, `via`, `heap`, `psram`, `bat`, `chg`, `rssi`, `ip`, `bl`, `vol`, `caps` | Ao ligar o WebSocket, a pedido, ou pela USB sem host |
| `tap` | `id` (tela), `item` | Toque num item (depois da confirmação, se houver) |
| `btn` | `id`, `btn` | Botão da tela |
| `slider` | `id`, `item`, `v` | Slider largado |
| `back` | `id` | Botão ← |
| `swipe` | `id`, `dir` (`left`, `right`) | Gesto lateral |
| `key` | `k` (`boot`, `user`, `pwr`), `ev` (`short`, `long`, `double`) | Botões físicos (o deckd mapeia-os no `[keys]`) |
| `imu` | `face` (`+x`..`-z`) · `ev:"shake"` · `ax ay az gx gy gz` | Face do cubo para cima mudou · agitar · stream |
| `mic` | `db`, `peak` (dBFS) | Com `mic` ligado |
| `notify` | `act:"open"` | Toque no aviso |
| `ota` | `pct` · `ok` · `err` | Progresso da atualização |
| `stats` | `heap`, `largest`, `psram`, `uptime`, `rssi`, `bat`, `mv`, `usb` | A pedido |
| `cfg` | ver acima | Respostas de configuração |
| `bat` | `pct`, `mv`, `chg`, `usb`, `bat`, `stage`, `state` (texto), `vbus`, `vsys` (mV), `temp` (°C do PMU), `icc` (mA), `cv` (mV), `up` (s), `hist` `[[idade s, %, mV, flags], ...]` (1/min, até 12 h; flags: 1 a carregar, 2 USB) | Resposta a `bat?` |
| `offline_log` | `events` (os eventos guardados, cada um com `ts`), `last` | Ao voltar a ligar: o que aconteceu fora de casa (lotes de 40). O deckd grava em `~/.local/state/deck3/eventos-<id>.jsonl` e não executa as ações |
| `bat_low` | `pct` | A bateria desceu a 20%, 10% ou 5% sem carregar (o cubo também avisa com som) |
| `sd` | `ok`, `name`, `total_mb`, `free_mb`, `mhz` · `err` | Resposta a `sd?` |
| `ls` | `ok`, `path`, `items` `[[nome, bytes, pasta?], ...]` | Resposta a `ls` |
| `file` / `rm` | `ok`, `path`, `size` · `err` | Respostas |
| `wifi_scan` | `aps` `[{ssid, rssi, ch, lock, saved}]` · `err` | Resposta a `wifi` `scan` |
| `pong` | — | — |
