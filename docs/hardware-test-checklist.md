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
