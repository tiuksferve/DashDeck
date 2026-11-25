# O que falta, ideias e limitações

## Por fazer

| Item | Notas |
|---|---|
| **Tela secundária de aplicações** | Uma página de informação para alguns programas abertos no uConsole (SDR++ e Gqrx: frequência e modo; PyGPSClient; etc.). É preciso escolher os programas e a forma de ler o estado de cada um |

## Ideias para depois
- **Persistir o histórico de mensagens no MeshDeckWeb:** hoje fica só na memória e perde-se quando o MeshDeckWeb reinicia. É uma mudança no MeshDeckWeb.
- **Bateria própria do ESP (LiPo no pino VBAT):** ler o ADC do GPIO0 e mostrar o nível. Só faz sentido se a tela ficar solta do uConsole.
- **Cartão SD do ESP:**
  - **Atualizar o firmware pelo cartão:** copiar o `.bin` para o cartão (com um leitor de cartões) e o ESP grava-o na partição `ota_1` ao arrancar, sem depender da USB. É preciso gravar esta funcionalidade uma vez pela USB.
  - **Ícones no cartão** (0.17.0), sem reenvio entre arranques (0.19.0).
- **OTA pela USB:** as partições `ota_0` e `ota_1` já existem; faltaria o envio do firmware pelo protocolo, sem `idf.py`.
- **Filtro por altitude** no ADS-B e por tipo no AIS.
- **Guardar os trajetos do ADS-B** entre arranques do dashd (hoje ficam na memória).

## Limitações conhecidas
- **Céu do GPS com o meshtasticd:** só com o gpsmux do MeshDeckWeb instalado (`sudo gpsmux/install.sh`), que partilha o GPS e o põe ao vivo na API local. Sem ele, o Meshtastic não repassa os dados dos satélites.
- **Posição pelo Meshtastic:** depende de o nó ter fix (`has_position`). Sem fix, a tela GPS mostra "sem posição".
- **USB partilhada:** o ESP, o RTL-SDR e o SSD estão no mesmo barramento USB (controlador dwc2). Com a **alimentação USB da AIO v2 ligada** (interruptor USB), a ligação ao ESP perde dados: a gravação falha ("Packet content transfer stopped") e o ESP chega a deixar de ser reconhecido (erro -71 no `dmesg`). Desligar o USB da AIO resolveu (19/11). Uma cópia grande para um disco USB piora o problema. O box USB do AIO pede confirmação antes de ligar, e o `flash.sh` repete a gravação em modo seguro (sem stub, 115200) se a normal falhar.
- **SDR ocupado:** só um programa usa o RTL-SDR de cada vez. O ADS-B e o AIS ligados pelo DashDeck desligam-se ao sair da tela; um serviço ligado de outra forma continua a ocupar o dongle até ser parado.
- **Lançadores manuais do SDR:** o "AIS Tracker" do desktop corre o AIS-catcher com o servidor web na porta 8080, a mesma do MeshDeckWeb; use o serviço `ais-catcher` (porta 8119) pelo DashDeck. O `tar1090.sh` liga o readsb e desliga-o ao fechar o browser.
- **ADS-B sem aeronaves no teste:** o readsb arrancou e abriu o SDR, mas não recebeu nada em 45 s (antena de AIS ligada?). Os trajetos do ADS-B foram testados com dados simulados.
- **Mapas na primeira vez:** cada área e zoom precisam de internet uma vez; depois ficam em cache. A CARTO deixou de servir sem chave de API, por isso os estilos usam a Esri e o OSM.
- **Texto:** as fontes têm ASCII e Latin-1 (português completo). Os emojis aparecem como imagens (até 48 diferentes ao mesmo tempo); outros alfabetos (grego, cirílico…) são removidos ou trocados.
- **Relógio:** as nuvens, as estrelas e o reflexo do lago só mudam quando a cena é refeita (a cada 5 min); só a chuva, a neve e os relâmpagos são animados no ESP. Os gestos do relógio são medidos do toque ao soltar (24 px), sem o detetor do LVGL.
- **Uma linha JSON tem até 8 KB.** Por isso a lista de nós mostra 30.
- **Perfis da AIO:** aplicar um perfil só foi testado no caminho "já está ativo"; a sequência completa (parar, desligar, ligar, arrancar serviços) não foi corrida no hardware.
- **Rotas dos voos:** vêm do adsbdb.com e, quando falta, do hexdb.io, bases feitas por voluntários; algumas faltam e as do hexdb podem ser antigas (mostradas a cinza). O AIS não transmite o porto de saída, só o destino escrito pela tripulação.
- **Abertas:** precisa de um compositor com `wlr-foreign-toplevel-management` (labwc, wayfire, sway). Num ambiente X11 puro a lista fica vazia com o erro.
- **Rede:** ligar a uma rede nova com palavra-passe é no uConsole (sem teclado no DashDeck). A página VPN só foi vista vazia (não há VPN configurada no uConsole).
- **Mensagens:** o envio pela tela só foi testado com a interface falsa dos testes do MeshDeckWeb, não pela rede real.
- **Segurança da ponte:** com `--local-api on`, qualquer processo do uConsole lê e envia mensagens pelo nó, sem login.
- **`sudo -n`:** os botões de serviços precisam de sudo sem senha para o `systemctl`, e o ADS-B e o AIS também para o `fuser` e o `kill` (libertar o SDR).
- **Espaço em disco:** o cartão do uConsole tem pouco espaço livre, por isso o build fica em `/dev/shm` e o ESP-IDF tem só o toolchain RISC-V.
- **Controle de versões:** o DashDeck está em git (branch `main`, remoto `github.com/tiuksferve/DashDeck`). As mudanças do MeshDeckWeb (API local, campo `transport`, nome DashDeck) estão no repositório dele.

## Histórico de versões do firmware

| Versão | Mudanças |
|---|---|
| 0.1.0 | Esqueleto: templates menu, list, kv, log e text, toasts, confirmação, botão BOOT |
| 0.2.0 | Páginas com swipe e template `plot` (céu do GPS, radar do ADS-B) |
| 0.3.0 | Modo horizontal, fontes com acentos, canvas de tamanho variável, fundo de mapa |
| 0.3.1 | Buffer RX de 16 KB e envio confirmado do mapa (corrige o mapa pela metade) |
| 0.4.0 | Ícones reais das aplicações |
| 0.5.0 | Menus em boxes e dock |
| 0.6.0 | Boxes com scroll lateral e ícones emoji |
| 0.7.0 | Selo de não lidas (Meshtastic) |
| 0.8.0 | Fundo por card nas listas (`bg`) e confirmação por item (`confirm`) |
| 0.9.0 | Trajetos no plot (`tracks`) e até 80 pontos |
| 0.10.0 | Botões sobre o canvas (`ctrl`): zoom nos cantos e setas para deslocar o mapa |
| 0.11.0 | Modo relógio (`clock`): paisagem e digital retro em pixel art, hora local no ESP (`time`), fundo de 8 bpp, gestos de mostrador e brilho, libertação dos ícones quando falta memória |
| 0.11.1 | Relógio: gestos medidos pelo próprio firmware (o swipe vertical saía do relógio) e confirmação com 2.º toque para sair |
| 0.12.0 | Ecrã apagado por inatividade na tela inicial (`idle`); o dock só aparece nas telas que o pedem |
| 0.12.1 | Relógio escurece para 10% após 5 min sem toques; um toque repõe o brilho |
| 0.13.0 | Emojis a cores no texto (`emoji`, imgfont); confirmações copiadas sem cortar caracteres UTF-8 |
| 0.13.1 | Confirmações e avisos com a fonte acentuada (o "ã" de "Não" aparecia como um quadrado) |
| 0.14.0 | Boxes com `bg` e `sub_color` (interruptores do AIO v2) |
| 0.15.0 | Animação de arranque: Pixels ou Nascer do sol, ao acaso |
| 0.15.1 | A animação é a tela de carregamento: barra de progresso, fica até o uConsole responder; sem "BOM DIA" |
| 0.15.2 | Frase "TELA PEQUENA, GRANDES POSSIBILIDADES!" na animação Pixels (fonte com `!` e `,`) |
| 0.15.3 | Tela de carregamento com 5 s no mínimo (a animação estica-se) |
| 0.15.4 | A frase também no Nascer do sol |
| — | (só host) Box Sondas: radiossondas RS41 com o descodificador do SondeDeck (receptor sem janela `sonderx.py`, serviço `dashd-sonde`); frequências europeias com Lisboa no topo, mapa com rebentamento e pouso previsto, dados, altitude e estatísticas |
| — | (só host) Box Stocks: cotações do Yahoo Finance, logotipos em cache local, gráfico do dia |
| — | (só host) Serviços: 16 serviços (DashDeck, CamDeck, SSH, VNC, tar1090, GPS mux...), serviços do utilizador, arranque no boot, estado lido numa só chamada |
| — | (só host) ADS-B e AIS com três páginas: Controlo (ligar/desligar, monitor, resumo), Lista e Mapa |
| 0.25.13 | Boxes da tela inicial com o ícone e os textos centrados: o nome só ocupa as linhas que tem (até 2) e margens iguais em cima e em baixo |
| 0.25.15 | Listas paradas durante o toque e o scroll: com o dedo no ecrã ou a tela a rolar (também na inércia e até 1,5 s depois de parar), o `update` do host fica guardado (só o mais recente) e é aplicado depois; um toque simples aplica-o ao soltar. Antes, a lista do AIS reordenava-se debaixo do dedo |
| 0.25.14 | Captura do ecrã (`{"t":"shot"}`): o ESP redesenha o ecrã todo e envia os pixels ao host (`shotpx`, `shotend`); o dashd grava um PNG com `kill -USR2` |
| 0.25.12 | Avisos (mensagem direta, alarme, emergência ADS-B) desenham primeiro na arena do ecrã quando está livre; antes dependiam de 46 KB seguidos no heap, que ao fim de horas podiam faltar (ficava só a faixa e o escurecimento). Último recurso: a arena do plot da tela, redesenhado no fim (`redraw`). Resposta `ntfack` registada pelo dashd |
| 0.25.11 | (diagnóstico, substituído pela 0.25.12) `ntfack` com o resultado da memória do aviso |
| 0.25.10 | Plot com textos soltos (`"texts":[[x,y,"#cor","texto","l\|c\|r"]]`, ex.: eixos); setas das boxes maiores e grossas (fonte própria `dash_arrows`, DejaVu Sans Bold 22 px, só ↑ ↓). No host: gráfico das ações com horas, valores e dia da sessão; antes da abertura mostra a sessão anterior no sítio certo (os pontos ficavam espremidos no início) |
| 0.25.9 | Cantos das boxes da grade: `"dot":"#cor"` (ponto no canto superior direito) e `"mark":"up|down"` + `"mark_color"` (seta ↑/↓ no canto superior esquerdo); fontes com ↑ ↓. No host: Stocks com o ponto verde/vermelho da bolsa aberta/fechada e a seta da variação; o subtítulo fica só com a percentagem |
| 0.25.8 | Boxes da grade com valor em destaque (`"value"`, `"value_color"`): letra maior entre o nome e o subtítulo. No host: cotação em destaque nas boxes do Stocks; detalhe de uma ação em duas páginas (gráfico na tela toda e informações), com páginas também nos detalhes dinâmicos (`detail_pages`) |
| 0.25.7 | Fontes com o € (U+20AC); antes o dashd tirava-o do texto. No host: cotações com o símbolo da moeda à frente, logotipo da SpaceX maior (`logo_crop`) |
| 0.25.6 | Título do cabeçalho visível com o avião do monitor: largura fixa até antes do avião (na 0.25.5 ficava só "...") |
| 0.25.5 | Cabeçalho com o avião do monitor: a hora, a bateria e a ligação ficam à direita (um espaçador ocupa o lugar que o título deixa; na 0.25.4 deslizavam para cima do avião) |
| 0.25.4 | Avião no cabeçalho (`{"t":"hdr","plane"}`), laranja com o monitor de emergências ligado e vermelho depois de uma emergência. No host: o aviso de emergência passa a ser opcional, pelo cartão **Monitor de emergências** no ADS-B (mantém o readsb ligado fora da tela; estado em `state.json`) |
| 0.25.3 | Aviso de emergência do ADS-B (`notify` com `kind:"squawk"`): avião vermelho em pixel art que pisca, faixa vermelha. No host: o dashd verifica o readsb a cada 3 s em qualquer tela e avisa uma vez por avião e código (7500, 7600, 7700) a cada 30 min; o toque abre o detalhe do avião |
| 0.25.2 | Aplicações fluidas também depois de muito uso: os ícones voltam do cartão para a RAM enquanto sobrarem 20 KB (eram 36 KB, e os do fim da lista ficavam no cartão, com bloqueios de ~400 ms no scroll); o fundo do mapa é libertado ao abrir uma tela que não é mapa. Aplicações a 44-45 quadros/s depois de relógio, mapa e câmara |
| 0.25.1 | Inércia do scroll mais longa (`scroll_throw` 4 em vez de 10: o toque é lido ~100 vezes/s e o scroll parava logo ao soltar); `stats` traz `heap`, `largest`, `icons_ram`, `icons_sd`, `emojis` e `emoji_kb` |
| 0.25.0 | Imagens JPEG no canvas (`{"t":"jpg"}`, TJpgDec da ROM, ~40 ms por quadro de 242×136, resposta `jpgack`), plot `live` (o update não apaga a imagem), botões `ctrl` com largura do rótulo e cor. No host: box **Câmara** (CamDeck) com visor ao vivo, Foto e Gravar; visor a ~16 quadros/s quando a câmara os dá |
| 0.24.2 | Sombra do relógio ainda mais leve (`SCRIM_A` 85). No host: dock com ícones a cores (emoji) em Início, Captura, Brilho e Bloquear; Aplicações continuam a 44 quadros/s |
| 0.24.1 | Sombra atrás do texto do relógio mais leve (opacidade 100 em vez de 165, `SCRIM_A` em `clockface.c`) |
| 0.24.0 | Texto do relógio mais legível sobre as cenas: sombra translúcida esbatida atrás da hora, da data e do tempo, contorno de 2 px com sombra projetada nos números grandes; `stats` com o tempo de desenho do relógio (~14 ms por quadro, com a animação a 8 quadros/s) |
| 0.23.1 | `bench` rola na vertical quando a tela não tem scroll lateral (retrato). No host: box **Abertas** nas Aplicações (janelas do uConsole) e rotação pelo menu (Sistema > DashDeck); em retrato a tela inicial e as Aplicações rolam a 43 quadros/s |
| 0.23.0 | A RAM dos ícones vai para os da tela aberta e do dock (carregados do cartão ao construir a tela; os de outras telas libertam-se); reserva de 36 KB: swipe nas Aplicações de 24 para 44 quadros/s, sem as paragens de 390 ms; `bench` com `px` |
| 0.22.0 | Relógio com 5 mostradores: cidade (cena nova) e temporizador e alarme (zonas de toque, repetição ao manter o dedo); toque longo no retro muda a cor do LCD; aviso do alarme (despertador dourado, `kind`, `title`, `dur`) |
| 0.21.0 | Desenho mais rápido: build `-O2` (era `-Og`) e `memcpy` da biblioteca C; as atualizações só mudam o que mudou (texto, cor, imagem, botões), em vez de refazer tudo e reabrir os ícones do cartão (bloqueavam o ecrã até 267 ms a cada 2 s; agora até 2,4 ms); os ícones leem-se do cartão fora do lock do LVGL; comandos de medição `bench` e `stats` |
| 0.20.0 | Ícones com cópia na RAM enquanto há memória (swipes e animações sem ler o cartão), 96 lugares de ícones sem substituir os do dock (o ícone do Relógio desaparecia), `iconlost`, cache de imagens de 32 KB |
| 0.19.0 | LVGL no heap do sistema (+50 KB livres), cache de imagens de 48 KB (swipes sem ler o cartão), ícones já guardados no cartão não são reenviados |
| 0.18.0 | Mostrador de previsão por horas no relógio (costa com farol) |
| 0.17.0 | Cartão SD (ícones, e fundos quando falta RAM), buffer de ecrã reservado no arranque, aviso de mensagem por cima do relógio com a memória do fundo emprestada |
| 0.16.0 | Aviso de mensagem direta (`notify`, animação "Ondas LoRa"); a animação de arranque passa a ser mesmo aleatória (entropia do ADC no arranque) |
