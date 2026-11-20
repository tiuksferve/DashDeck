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
- **Rotação pelo próprio menu** (hoje é no `config.toml`).
- **Relógio:** mais cenas (cidade à noite, neve no inverno) e escolher a cor do LCD por gesto.

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
| 0.19.0 | LVGL no heap do sistema (+50 KB livres), cache de imagens de 48 KB (swipes sem ler o cartão), ícones já guardados no cartão não são reenviados |
| 0.18.0 | Mostrador de previsão por horas no relógio (costa com farol) |
| 0.17.0 | Cartão SD (ícones, e fundos quando falta RAM), buffer de ecrã reservado no arranque, aviso de mensagem por cima do relógio com a memória do fundo emprestada |
| 0.16.0 | Aviso de mensagem direta (`notify`, animação "Ondas LoRa"); a animação de arranque passa a ser mesmo aleatória (entropia do ADC no arranque) |
