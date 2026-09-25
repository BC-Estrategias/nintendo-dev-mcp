# Roteiro de teste em hardware

Cada marco que toca o console tem um roteiro aqui. Marque o que passou e **cole o que falhou** (fotos das telas + `agent.log`) para diagnosticarmos por camada.

## M1 — agente 3DS: Wi-Fi, IP, porta, HELLO/PING

**Requisitos:** New 3DS com Luma3DS + Homebrew Launcher; 3DS e Mac na **mesma rede Wi-Fi** (sem "isolamento de clientes/AP isolation"; o 3DS só usa 2.4 GHz).

### 1. Instalar
- **Cartão SD:** copie `dist/nintendo-dev-agent-vX.Y.Z.3dsx` para `SD:/3ds/nintendo-dev-agent/`. **Apague a versão anterior do SD** (o Homebrew Launcher lista cada `.3dsx`, e o mesmo título apareceria duas vezes). Confira a versão na descrição do app no Launcher e na tela superior do agente.
- **Ou por Wi-Fi (`3dslink`):** no 3DS abra o Homebrew Launcher e aperte **Y** (netloader; mostra o IP). No Mac:
  ```bash
  /opt/devkitpro/tools/bin/3dslink -a <IP_DO_3DS> dist/nintendo-dev-agent-vX.Y.Z.3dsx
  ```

### 2. Abrir e conferir a tela
| Onde | Esperado |
|---|---|
| Superior | `Nintendo Dev Agent  v0.3.1`, `Protocol 1`, `Status: ONLINE` (verde), `IP: 192.168.x.x`, `Port: 6464`, `Bridge: not connected`, `Mode: READ_ONLY` |
| Inferior | linhas `Nintendo Dev Agent v0.3.1…`, `Console: New 3DS family`, `acInit/psInit/ndmuInit: 0x00000000`, `Wi-Fi connected`, `Network services ready`, `Listening on 192.168.x.x:6464` |

Se aparecer `NETWORK ERROR` ou algum `0x…` diferente de zero, **pare e me mande a tela** (o código é o diagnóstico).

### 3. Ping a partir do Mac
```bash
node bridge/packages/cli/src/main.ts ping <IP_DO_3DS> -c 10
```
Esperado: `Agent: 3ds  v0.3.1  protocol 1  mode READ_ONLY  auth none  max_frame 65536`, dez linhas `PONG`, e a linha `min/avg/max`. **Anote os tempos** (é a nossa primeira medida de latência real). Na tela inferior devem aparecer `[CONNECT]`, `[REQ n] HELLO`, `[OK n]`, `[REQ n] PING`…; a superior mostra `Bridge: CONNECTED`, depois volta a `not connected`.

### 4. Robustez
- [ ] Rode o ping duas vezes seguidas (reconexão).
- [ ] Em dois terminais ao mesmo tempo: o segundo deve **substituir** o primeiro (o primeiro falha com "connection closed"; o 3DS loga `replaced by a new connection`).
- [ ] Desligue o Wi-Fi do roteador (ou afaste-se) por ~20 s e religue: a tela deve mostrar `Wi-Fi lost` → `Wi-Fi connected` → `Listening on …`, e o ping deve voltar a funcionar **sem reiniciar o app**.
- [ ] Aperte HOME e volte ao app: o ping continua funcionando? (anote o comportamento)
- [ ] `START` para sair: volta ao Homebrew Launcher sem travar; abra o agent de novo e confira que a porta 6464 volta a abrir (sockets liberados).

### 5. Coletar
- Foto/print das duas telas.
- O arquivo `SD:/3ds/nintendo-dev-agent/agent.log` (rotaciona em 128 KB; `agent.log.1` é o anterior).
- A saída do `ping` (com os tempos).

### O que este teste NÃO cobre / suposições ainda não verificadas
Latência sob carga, arquivos grandes, comportamento com jogos em segundo plano, tampa fechada, e o efeito de `NDMU_EnterExclusiveState` em outros recursos do sistema. `aptSetSleepAllowed(false)` impede o modo de espera enquanto o agente roda.


## M3 + M4 — leitura de arquivos (`ls`, `stat`, `cat`, `get`) — agente v0.3.1

**Preparação no SD (uma vez):** crie o arquivo `SD:/3ds/nintendo-dev-agent/test.txt` com o texto `Hello from Nintendo 3DS` (teste de aceitação do projeto). Se preferir outro conteúdo, tudo bem — anote qual.

No Mac (substitua o IP):
```bash
ndev="node bridge/packages/cli/src/main.ts"; IP=<IP_DO_3DS>
$ndev ls   $IP /                                   # raiz do SD
$ndev ls   $IP /3ds                                # apps
$ndev stat $IP /3ds/nintendo-dev-agent/test.txt
$ndev cat  $IP /3ds/nintendo-dev-agent/test.txt    # deve imprimir: Hello from Nintendo 3DS
$ndev cat  $IP /3ds/nintendo-dev-agent/agent.log --tail 3000   # o log atual do agente
$ndev get  $IP /3ds/nintendo-dev-agent/agent.log /tmp/agent.log
```
Esperado: listagens corretas; `cat` idêntico ao arquivo no SD; `get` termina com `verified against the agent`. Na tela do 3DS aparecem `[REQ n] FS_LIST/FS_STAT/FS_READ` e `[OK n] … bytes … KiB/s` (a primeira medida de vazão de leitura do SD).

**Proteções (devem FALHAR, é o esperado):**
```bash
$ndev cat $IP /3ds/nintendo-dev-agent/config/qualquer   # PROTECTED_PATH (zona reservada da config/chaves)
$ndev cat $IP /nao-existe                               # NOT_FOUND
$ndev cat $IP /3ds                                      # BAD_REQUEST (é diretório)
```
**Coletar:** a saída de tudo acima, incluindo as linhas `[OK …] N bytes X ms Y KiB/s` da tela inferior (ou do `agent.log`).


## M5 (parte 1) — escrita — agente v0.3.1

**Atenção: esta é uma build de desenvolvimento.** Ela abre em `DEVELOPMENT` (escrita ligada, **só** em `/3ds/nintendo-dev-agent/`) e ainda **não tem pareamento**: qualquer aparelho da rede pode escrever nessa pasta enquanto o modo estiver ligado. Aperte **X** no 3DS para alternar para `READ_ONLY` (a tela mostra o modo). Antes de qualquer distribuição o padrão volta a ser `READ_ONLY` e o pareamento entra.

No Mac:
```bash
ndev="node bridge/packages/cli/src/main.ts"; IP=<IP_DO_3DS>
$ndev put $IP --text "Codex was here." /3ds/nintendo-dev-agent/from-codex.txt   # teste de aceitação
$ndev cat $IP /3ds/nintendo-dev-agent/from-codex.txt                            # deve devolver o mesmo texto
$ndev put $IP --text "outra" /3ds/nintendo-dev-agent/from-codex.txt             # deve FALHAR: EXISTS
$ndev put $IP --text "outra" /3ds/nintendo-dev-agent/from-codex.txt --replace --backup   # cria from-codex.txt.bak
$ndev mkdir $IP /3ds/nintendo-dev-agent/inbox
$ndev put $IP algum-arquivo.bin /3ds/nintendo-dev-agent/inbox/algum-arquivo.bin  # mostra a vazão de escrita
```
**Verificação física:** tire o cartão (ou use o `ls`) e confira que `from-codex.txt` existe com o conteúdo certo e que **não sobrou** nenhum `*.ndp-tmp`/`*.ndp-old`.

**Proteções (devem FALHAR):**
```bash
$ndev put $IP --text x /luma/teste.txt              # PROTECTED_PATH
$ndev put $IP --text x /3ds/outro/teste.txt         # PROTECTED_PATH (fora da pasta liberada)
$ndev put $IP --text x /3ds/nintendo-dev-agent/config/k   # PROTECTED_PATH
```
Aperte **X** (modo `READ_ONLY`) e repita o primeiro `put`: deve falhar com `FORBIDDEN_MODE`.
**Coletar:** a saída de tudo, a tela do agente e as linhas `[OK n] N bytes … KiB/s` (vazão de escrita).


## M5 (parte 2) — pareamento — agente v0.5.0

**Antes:** apague a versão anterior do agente do cartão SD (o `ndev put` da v0.4.0 ainda funciona porque ela não tem autenticação) e abra a v0.5.0. Na **primeira** abertura o agente cria `/3ds/nintendo-dev-agent/config/` (uma vez, ~6 s: a tela pode parecer parada) e gera a identidade do console. Confira: `Auth   : required - 0 paired`.

No Mac (o 3DS **não** deve estar pareado ainda):
```bash
ndev="node bridge/packages/cli/src/main.ts"; IP=<IP_DO_3DS>
$ndev hello $IP                    # auth required; sem pareamento, os comandos abaixo devem falhar
$ndev ls $IP /                     # FALHA: "not paired ... press Y on the console"
$ndev pair $IP                     # FALHA: "pairing window is closed" (ainda não apertou Y)
```
**Pareando (feito por você, não pela IA):**
1. No 3DS aperte **Y**: a tela superior mostra `PAIRING OPEN (120 s)` e o código `XXXX-XXXX-XXXX-XXXX`.
2. No Mac, num terminal seu: `$ndev pair $IP` e digite o código (minúsculas e sem hífen também servem).
3. Esperado: `Paired with console …`; a tela do 3DS passa a `Auth: required - 1 paired` e a janela **fecha sozinha**. O código **não** aparece no `agent.log` (confira: `$ndev cat $IP /3ds/nintendo-dev-agent/agent.log --tail 30`).
```bash
$ndev ls $IP /                                         # agora funciona
$ndev put $IP --text "Codex was here." /3ds/nintendo-dev-agent/from-codex.txt --replace
$ndev cat $IP /3ds/nintendo-dev-agent/config/pairing.bin   # deve FALHAR: PROTECTED_PATH (o arquivo de chaves é inacessível)
$ndev pairings                                         # lista este console
```
**Testes de rejeição (devem FALHAR):**
- Aperte **Y** de novo e rode `$ndev pair $IP` digitando um código **errado**: `UNAUTHORIZED`. Cinco erros fecham a janela.
- Feche o agente (START) e abra de novo: o pareamento **persiste** (`Loaded 1 paired computer(s)` no log) e `ndev ls` continua funcionando sem parear.
- **SELECT duas vezes** no 3DS: `Auth: required - 0 paired`; `ndev ls` passa a dar `rejected the stored pairing key ... press Y ... ndev pair`.
- No Claude Code/Codex: peça para listar o SD do 3DS. Sem pareamento o modelo deve dizer que **você** precisa parear (ele não tem ferramenta para isso).
**Coletar:** a tela superior durante o pareamento (foto; **apague a foto depois** — ela mostra o código, que só vale 2 min), o `agent.log`, e os tempos de `ndev pair` e do primeiro `ndev ls` (o pareamento grava no SD).


## M5 (parte 3) — pastas escolhidas no console — agente v0.6.0

**O que muda:** o agente agora abre com **só `/3ds/nintendo-dev-agent`** liberada (antes lia o cartão inteiro). Você libera o resto **no próprio 3DS**. Apague a versão anterior do SD, abra a v0.6.0 e confira no log `Loaded 0 opened folder(s)` (ou `Creating ...` se a pasta `config` ainda não existia). O pareamento da v0.5.0 continua valendo.

No Mac:
```bash
cd ~/nintendo-dev-mcp; M=bridge/packages/cli/src/main.ts; IP=<IP_DO_3DS>
node $M access $IP                 # só /3ds/nintendo-dev-agent (leitura e escrita)
node $M ls $IP /                   # aparece só "3ds" (o único caminho até a pasta liberada)
node $M ls $IP /roms               # PROTECTED_PATH (ainda fechada)
```
**No 3DS:** aperte **A** → abre "Access folders". Setas para mover, **A** entra na pasta, **B** volta, **Y** muda o acesso da linha destacada (`[  ]` fechada → `[R ]` leitura → `[RW]` leitura+escrita → fechada). A linha `.` é a pasta em que você está. Abra, por exemplo, `roms` com **leitura** e `cias` com **escrita**. **START** fecha o menu. A tela principal mostra `Open : agent folder + N more (M writable)`.
```bash
node $M access $IP                 # mostra as pastas novas, na hora (sem reiniciar o agente)
node $M ls $IP /                   # agora também mostra "roms" e "cias"
node $M ls $IP /roms | head        # funciona
node $M put $IP --text x /roms/teste.txt    # PROTECTED_PATH: só leitura
node $M put $IP --text x /cias/teste.txt    # deve funcionar se o modo permitir escrita (X); apague depois: node $M rm $IP /cias/teste.txt
node $M ls $IP /luma               # PROTECTED_PATH
```
**Testes de robustez (no 3DS):**
- Tente **Y** numa pasta filha de uma já liberada com leitura: a tela mostra `(R )` (herdado) e Y oferece só `[RW]`.
- Tente **Y** em `luma` ou `Nintendo 3DS`: pode dar leitura, **nunca** escrita (a linha tem `!`).
- Libere `/` (linha `.` na raiz): a tela principal mostra **WHOLE SD CARD** em vermelho. Volte a fechar.
- Feche o agente (START) e abra de novo: `Loaded N opened folder(s)` e `access` igual ao que você deixou.
- Abra 6 pastas e tente a 7ª: `List full`.
**Coletar:** foto do menu (top e bottom), a saída de `access`/`ls`, e o `agent.log` (linhas `[ACCESS] ...`).


## Release 1.0.0 — verificação rápida no console
1. Instale/abra o v1.0.0 (3dsx ou CIA). A tela mostra `Mode : READ_ONLY (no writes)` e `Auth : required - N paired`.
2. `ndev put <ip> --text x /3ds/nintendo-dev-agent/t.txt` deve falhar com `FORBIDDEN_MODE`. Aperte **X**: agora funciona. Aperte **X** de novo: volta a `READ_ONLY`.
3. **A** → menu de pastas: `/luma/plugins` aceita `[RW]`; `/luma` só leitura (mensagem "System folder"); `/boot.firm` e a pasta `config/` do agente nunca graváveis.
4. `ndev access <ip>` mostra o mesmo que o menu. Feche `/` se o tiver deixado aberto.
5. SELECT duas vezes esquece os pareamentos; `ndev pair` de novo funciona.

