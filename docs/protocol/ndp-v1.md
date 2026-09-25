# NDP v1 — Nintendo Dev Protocol

Especificação canônica. As implementações (C em `agent/common`, TypeScript em `bridge/packages/core`) devem seguir este documento; os vetores em `docs/protocol/test-vectors/` são gerados por uma terceira implementação independente (`generate.py`) e verificados nas duas.

Palavras **DEVE / NÃO DEVE / PODE** têm o sentido do RFC 2119. Todos os inteiros são **little-endian**. Strings são UTF-8 sem NUL final.

## 1. Transporte
TCP, uma conexão ativa por vez, uma operação por vez. Porta padrão **6464**. O protocolo é um fluxo de *frames*; não há dependência de HTTP/JSON.

## 2. Frame

```
off  size  campo
 0    4    magic         bytes 4E 44 50 31 ("NDP1")
 4    1    version       versão do formato do frame (1)
 5    1    kind          1=REQ 2=RES 3=DATA 4=END 5=ERR 6=EVT
 6    2    flags         bit0 = MAC presente; bit1 = MORE; demais DEVEM ser 0
 8    4    request_id    escolhido pelo Bridge; RES/DATA/END/ERR repetem o id da REQ
12    2    command       ver §5
14    2    status        0 em REQ/DATA/END/EVT; código (§6) em RES/ERR
16    4    payload_len   ≤ max_frame
20    N    payload
20+N  16   mac           só se flags.bit0 (§4)
```

- O cabeçalho tem **20 bytes** e o mesmo layout em todas as versões futuras.
- `max_frame` (limite do **payload**) padrão **65 536**; o agent o anuncia no HELLO. Um receptor DEVE rejeitar (`TOO_LARGE`) frame com `payload_len > max_frame` **sem** alocar/ler o payload.
- Flags desconhecidas (bits 2–15) DEVEM ser 0 no envio; o receptor as trata como `BAD_FRAME`.
- `kind`: **REQ** (Bridge→agent), **RES** (agent→Bridge, sucesso), **ERR** (agent→Bridge, falha), **DATA** (bytes crus de arquivo, qualquer direção), **END** (fim de transferência), **EVT** (agent→Bridge, sem REQ; reservado).
- O frame `HELLO` DEVE sempre ser enviado com `version = 1`, qualquer que seja a versão que o Bridge suporte (bootstrap de negociação).

## 3. Payload TLV
Payload de REQ/RES/ERR/END é uma sequência de campos:
```
[tag u16][len u16][value len bytes]
```
- Campos desconhecidos DEVEM ser ignorados (extensibilidade).
- Tipos por tag (§7): `u8`, `u16`, `u32`, `u64` (little-endian, tamanho exato), `str` (UTF-8), `bytes`. Um campo com tamanho incompatível com o tipo → `BAD_REQUEST`.
- Um mesmo tag repetido: vale o **primeiro**, salvo tags definidas como repetíveis.
- Payload de DATA são bytes crus (sem TLV).

## 4. Autenticação, pareamento e MAC por frame
Sem TLS no console: o canal é **autenticado e íntegro (HMAC por frame), mas não criptografado** (o conteúdo dos arquivos é visível na LAN).

### 4.1 Chave de pareamento
- No console o usuário abre uma **janela de pareamento** (tecla Y; 120 s; no máximo 5 tentativas erradas). O console gera 10 bytes aleatórios seguros (80 bits) e os mostra em Base32 Crockford, `XXXX-XXXX-XXXX-XXXX` (16 caracteres; alfabeto `0123456789ABCDEFGHJKMNPQRSTVWXYZ`; ao ler, `O→0`, `I,L→1`, maiúsculas, hífens/espaços ignorados).
- `PSK = SHA-256("NDP-PSK-v1" ‖ code[10])` (32 bytes). `key_id = SHA-256(PSK)[0:4]`.
- O **código nunca trafega**. O Bridge o digita uma vez (`ndev pair`), deriva a `PSK` e a guarda; o console guarda a `PSK` (até 4 computadores pareados) em arquivo que **nenhum comando do protocolo lê ou escreve**.

### 4.2 Provas (HMAC-SHA-256)
`proof(psk, rótulo, cn, dn, extra) = HMAC(psk, rótulo ‖ cn[16] ‖ dn[16] ‖ extra)` onde `cn` = nonce do Bridge (HELLO REQ), `dn` = nonce do console (HELLO RES); `rótulo` = os bytes ASCII `"pair"`, `"auth"` ou `"session"`.

### 4.3 PAIR (0x0004) — só com a janela aberta
`REQ {label str (≤ 15 bytes), proof bytes32 = proof(PSK, "pair", cn, dn, label)}` → `RES {key_id bytes4}`. O console verifica com o código que **está exibindo** (deriva a PSK), guarda `{key_id, PSK, label}`, fecha a janela. Prova errada → `UNAUTHORIZED` e conta uma tentativa (5 → janela fechada e conexão encerrada); janela fechada → `UNAUTHORIZED` ("pairing is not open"); chaveiro cheio → `NO_SPACE`.

### 4.4 AUTH (0x0005)
`REQ {key_id bytes4, proof bytes32 = proof(PSK, "auth", cn, dn, "")}` (frame **sem** MAC) → `RES {}` **já com MAC** (é o primeiro frame selado). Chave desconhecida ou prova errada → `UNAUTHORIZED` (5 falhas encerram a conexão).
`session_key = HMAC(PSK, "session" ‖ cn ‖ dn)`.

### 4.5 Frames selados
Depois de um AUTH bem-sucedido, **todo** frame em **ambas** as direções DEVE ter `flags.MAC`:
```
mac = HMAC-SHA256(session_key, u64le(counter) ‖ header[0:20] ‖ payload)[0:16]
```
(`header` já com o bit MAC ligado). O contador é **por direção**, começa em 0 no primeiro frame selado daquela direção (para o console→Bridge, é o `RES` do AUTH; para o Bridge→console, é o primeiro REQ depois do AUTH) e sobe 1 por frame. Frame sem MAC, MAC inválido ou contador fora de ordem ⇒ a conexão é **encerrada** sem resposta. O comprimento máximo do payload continua `max_frame`; o MAC (16 bytes) vem além dele.

### 4.6 Regras do console
- `auth = "required"` (HELLO RES): antes do AUTH só são aceitos HELLO, PAIR (com a janela aberta) e AUTH; qualquer outro comando → `UNAUTHORIZED`. `auth = "none"` (somente testes/desenvolvimento): sem AUTH nem MAC.
- Com `auth = "required"` o HELLO RES traz também `device_id bytes16` (identificador estável do console, gerado no primeiro pareamento), `paired_keys u8` (quantos computadores) e `pairing_open u8`.
- O nonce do console DEVE vir de um gerador seguro; se não houver, com `auth = "required"` o HELLO falha (`IO_ERROR`) — nunca usar nonce fraco.
- Um HELLO **depois** do AUTH → `BAD_REQUEST` ("already authenticated"; abra uma nova conexão). Um frame com o bit MAC **antes** do AUTH encerra a conexão.

## 5. Comandos
| ID | Nome | M0 | Descrição |
|---|---|---|---|
| 0x0001 | HELLO | ✔ | negocia protocolo, anuncia plataforma |
| 0x0002 | PING | ✔ | eco de nonce |
| 0x0004 | PAIR | M5 ✔ | pareamento (janela aberta no console) |
| 0x0005 | AUTH | M5 ✔ | prova de posse da chave pareada |
| 0x0010 | DEVICE_INFO | M2 ✔ | modelo, firmware, RAM, regiões de memória e capacidade do SD |
| 0x0011 | ACCESS_INFO | M5 ✔ | modo e pastas que o dono liberou (só depois do AUTH) |
| 0x0020 | FS_LIST | M3 ✔ | lista um diretório (paginado) |
| 0x0021 | FS_STAT | M3 ✔ | tipo, tamanho, mtime |
| 0x0022 | FS_READ | M4 ✔ | lê arquivo em streaming |
| 0x0030 | FS_WRITE | M5 ✔ | grava um arquivo (temp → validar → rename) |
| 0x0031 | FS_MKDIR | M5 ✔ | cria um diretório |
| 0x0032 | FS_RENAME | pós-MVP | |
| 0x0033 | FS_DELETE | M5 ✔ | **move** para a lixeira (nunca apaga de verdade) |
| 0x0040+ | LOG_* / CRASH_* | fase 3 | |
| 0x8000–0xFFFF | reservado a extensões de plataforma | | |

Um comando desconhecido → ERR `UNSUPPORTED_COMMAND`. Todo comando exceto HELLO antes de um HELLO bem-sucedido → ERR `HELLO_REQUIRED`. Kind diferente de REQ onde REQ é esperado → ERR `BAD_REQUEST`.

## 6. Status
| Código | Nome | Uso |
|---|---|---|
| 0 | OK | |
| 1 | UNSUPPORTED_PROTOCOL | sem versão em comum |
| 2 | UNAUTHORIZED | falta pairing/AUTH |
| 3 | FORBIDDEN_MODE | operação exige modo maior (ex.: escrita em READ_ONLY) |
| 4 | PROTECTED_PATH | path fora das raízes permitidas ou em zona protegida |
| 5 | NOT_FOUND | |
| 6 | EXISTS | |
| 7 | IO_ERROR | |
| 8 | NO_SPACE | |
| 9 | BAD_REQUEST | campo ausente/inválido |
| 10 | BUSY | outra operação em curso |
| 11 | TOO_LARGE | |
| 12 | HASH_MISMATCH | |
| 13 | TIMEOUT | |
| 14 | UNSUPPORTED_COMMAND | |
| 15 | HELLO_REQUIRED | |
| 16 | BAD_FRAME | frame malformado (magic, flags) |
| 17 | PATH_INVALID | path não normalizável (§10) |

ERR carrega, opcionalmente: `0x0001 detail` (str, texto humano curto, **informativo — clientes NÃO DEVEM depender do texto**; o agent de referência usa exatamente as strings que aparecem nos vetores de diálogo) e `0x0002 os_result` (u32, código nativo do SO/console, para depuração). Sucesso sempre é `kind = RES`, `status = 0`; falha sempre é `kind = ERR`, `status ≠ 0`.

## 7. Tags de campos
| Tag | Nome | Tipo | Onde |
|---|---|---|---|
| 0x0001 | detail | str | ERR |
| 0x0002 | os_result | u32 | ERR |
| 0x0010 | protocol_min / protocol | u16 | HELLO REQ: mínimo; HELLO RES: versão escolhida |
| 0x0011 | protocol_max | u16 | HELLO REQ |
| 0x0012 | bridge_name | str | HELLO REQ |
| 0x0013 | nonce | bytes[16] | HELLO REQ (client_nonce) e RES (device_nonce) |
| 0x0014 | platform | str | HELLO RES (`"3ds"`, `"dsi"`, `"switch"`, `"host"`) |
| 0x0015 | agent_version | str | HELLO RES |
| 0x0016 | auth | str | HELLO RES: `"none"`, `"required"`, `"paired"` |
| 0x0017 | mode | str | HELLO RES: `"READ_ONLY"`, `"DEVELOPMENT"`, `"FULL"` |
| 0x0018 | max_frame | u32 | HELLO RES |
| 0x0019 | supported_min | u16 | ERR de HELLO (`UNSUPPORTED_PROTOCOL`) |
| 0x001A | supported_max | u16 | ERR de HELLO |
| 0x0020 | ping_nonce | u64 | PING REQ e RES (eco) |
| 0x0030 | path | str | FS_* REQ (path de protocolo, §10) |
| 0x0031 | cursor | u32 | FS_LIST REQ: índice da primeira entrada desejada (0 = início) |
| 0x0032 | next_cursor | u32 | FS_LIST RES: cursor da próxima página |
| 0x0033 | entry | bytes | FS_LIST RES, **repetível**: `type u8 ‖ size u64 ‖ name utf-8` (ver §13) |
| 0x0034 | type | u8 | FS_STAT RES: 1 = arquivo, 2 = diretório |
| 0x0035 | size | u64 | FS_STAT RES: bytes |
| 0x0036 | mtime | u64 | FS_STAT RES: segundos Unix UTC; **0 = desconhecido** |
| 0x0037 | offset | u64 | FS_READ REQ (padrão 0) |
| 0x0038 | length | u64 | FS_READ REQ (0 = até o fim do arquivo) |
| 0x0039 | chunk | u32 | FS_READ REQ: tamanho de cada DATA (padrão 32 768; limitado a [512, max_frame]) |
| 0x003A | want_hash | u8 | FS_READ REQ: 1 = agent calcula SHA-256 dos bytes enviados |
| 0x003B | sha256 | bytes[32] | FS_READ END (só se want_hash = 1) |
| 0x003C | total_size | u64 | FS_READ RES: tamanho total do arquivo |
| 0x003D | will_send | u64 | FS_READ RES: bytes que virão em DATA |
| 0x003E | list_more | u8 | FS_LIST RES: 1 = pode haver mais entradas (pedir `next_cursor`), 0 = fim |
| 0x0040 | overwrite | u8 | FS_WRITE REQ: 0 = nunca sobrescrever (padrão), 1 = substituir |
| 0x0041 | backup | u8 | FS_WRITE REQ: 1 = manter o arquivo antigo como `<nome>.bak` (só com overwrite = 1) |
| 0x0043 | replaced | u8 | FS_WRITE RES: 1 = havia um arquivo e foi substituído |
| 0x0044 | written | u64 | FS_WRITE RES: bytes gravados |
| 0x0045 | max_chunk | u32 | FS_WRITE RES (pronto): maior payload de DATA aceito |
| 0x0046 | trash_path | str | FS_DELETE RES: onde o item foi parar na lixeira |
| 0x0047 | paired_keys | u8 | HELLO RES (auth required): computadores pareados |
| 0x0048 | pairing_open | u8 | HELLO RES (auth required): 1 = janela de pareamento aberta |
| 0x0049 | device_id | bytes[16] | HELLO RES (auth required) |
| 0x004A | key_id | bytes[4] | AUTH REQ; PAIR RES |
| 0x004B | proof | bytes[32] | PAIR REQ / AUTH REQ |
| 0x004C | label | str | PAIR REQ (≤ 15 bytes) |
| 0x004D | read_root | str | ACCESS_INFO RES, repetido: pasta legível (recursivamente) |
| 0x004E | write_root | str | ACCESS_INFO RES, repetido: pasta gravável (subconjunto de read_root) |
| 0x004F | model | str | DEVICE_INFO RES |
| 0x0050 | firmware | str | DEVICE_INFO RES: versão do sistema ("11.17.0-50U") ou a do kernel |
| 0x0051 | ram_total | u64 | DEVICE_INFO RES: RAM física do modelo (128 ou 256 MiB), bytes |
| 0x0052 / 0x0053 | app_mem_total / app_mem_free | u64 | DEVICE_INFO RES: região de memória do app em primeiro plano (onde o próprio agente roda) |
| 0x0054 / 0x0055 | sys_mem_total / sys_mem_free | u64 | DEVICE_INFO RES: região SYSTEM |
| 0x0056 / 0x0057 | sd_total / sd_free | u64 | DEVICE_INFO RES: cartão SD, bytes |

## 8. Ordem de validação de um frame recebido pelo agent
Depois que o decoder entrega um frame, o agent avalia **nesta ordem** e responde ao primeiro problema:
1. `header.version ≠ 1` → ERR `UNSUPPORTED_PROTOCOL` (com `supported_min/max`);
2. `kind ≠ REQ` → ERR `BAD_REQUEST`;
3. payload não é uma sequência TLV bem formada (tamanho que ultrapassa o payload, ou resto < 4 bytes) → ERR `BAD_REQUEST`;
4. há uma transferência em curso nesta conexão → ERR `BUSY` (o REQ é descartado; a transferência continua);
5. `command = HELLO` → §9;
6. sem HELLO prévio na conexão → ERR `HELLO_REQUIRED`;
7. comando conhecido → seu tratamento; senão → ERR `UNSUPPORTED_COMMAND` (inclui comandos FS quando o agent não tem filesystem).

## 9. Handshake e PING
```
Bridge → HELLO REQ  {protocol_min=1, protocol_max=1, bridge_name, nonce[16]}
Agent  → HELLO RES  {protocol=1, platform, agent_version, nonce[16], auth, mode, max_frame}
```
O agent escolhe a maior versão comum a `[protocol_min, protocol_max]` e às que ele suporta. Sem interseção: `ERR UNSUPPORTED_PROTOCOL` com `supported_min/max` e a conexão permanece aberta (o Bridge decide fechar). `protocol_min > protocol_max` ou campos ausentes → `BAD_REQUEST`. Um segundo HELLO na mesma conexão reinicia a sessão.

`PING REQ {ping_nonce}` → `PING RES {ping_nonce}` (mesmo valor). Ausência de `ping_nonce` → `BAD_REQUEST`.

## 10. Paths
Um path de protocolo é **absoluto**, relativo à raiz do SD (`/3ds/x` ⇒ `sdmc:/3ds/x`), com componentes separados por `/`. **Normalização** (feita pelo agent, de forma autoritativa, e pelo Bridge para falhar rápido) — o path é **rejeitado** (`PATH_INVALID`) se:
1. não começa com `/`, ou é vazio, ou contém NUL, bytes de controle (< 0x20 ou 0x7F), ou `\`;
2. UTF-8 inválido (inclui sequências overlong, surrogates e > U+10FFFF);
3. contém componente vazio (`//`), `.` ou `..`;
4. algum componente contém `: * ? " < > |` (proibidos em FAT), termina em `.` ou espaço, ou contém `~` seguido de dígito (nome curto 8.3 que poderia driblar a política);
5. componente > 255 bytes ou path > 1024 bytes.
Uma `/` final única é removida (exceto na raiz `/`). O resultado normalizado é o que a política e o SO recebem.

**Comparação de prefixo** (raízes/zonas): por **componente**, com dobra de caixa **somente ASCII** (FAT é case-insensitive; para não-ASCII o agent **falha fechado**: só considera igual se os bytes forem idênticos). `/luma` cobre `/luma` e `/luma/x`, **não** `/lumax`.

## 11. Política de acesso
Entrada: `mode` (`READ_ONLY` < `DEVELOPMENT` < `FULL`), `op` (`read`|`write`), path, e a configuração `{read_roots, write_roots, never_read, never_write, write_except}` (listas de paths normalizados; `write_except` é opcional).

Avaliação, na ordem (a primeira que falha encerra):
1. path inválido → `PATH_INVALID`.
2. `op = write` e `mode = READ_ONLY` → `FORBIDDEN_MODE`.
3. path em alguma `never_*` da operação → `PROTECTED_PATH`. **Exceção de escrita:** um path dentro de uma zona `never_write` Z é liberado se existir uma `write_except` E tal que o path está dentro de E e E é sub-pasta **própria** de Z (E ≠ Z). Uma zona mais funda dentro de E protege de novo.
4. path fora de todas as raízes (`read_roots` / `write_roots`) → `PROTECTED_PATH`.
5. caso contrário → `OK`.

Padrões: `read_roots = ["/"]`; `write_roots = ["/3ds/nintendo-dev-agent"]`;
`never_write = ["/Nintendo 3DS", "/luma", "/boot.firm", "/gm9", "/private", "/3ds/nintendo-dev-agent/config"]`;
`never_read = ["/3ds/nintendo-dev-agent/config"]` (política e chaves);
`write_except = ["/luma/plugins", "/luma/titles"]` (plugins 3GX e substituição de arquivos de jogo/layeredfs — o que um desenvolvedor edita no Luma; o resto de `/luma`, como `config.ini`, `payloads` e `sysmodules`, pode impedir o console de ligar e continua protegido).
Um root ou zona pode ser um arquivo (`/boot.firm`). A configuração só é alterada com confirmação física no console (ver `ARCHITECTURE.md`); o protocolo NÃO tem comando para isso.

Os padrões acima são os de `ndp_policy_init_default` (e do agente de teste). **O agente do 3DS não os usa:** ele monta a política a partir da lista do dono (§11.2), cujo padrão é *só a pasta do próprio agente*.

### 11.1 Travessia (navegar até uma pasta liberada)
Se as raízes de leitura estão fundas (`/roms/gba`), o cliente precisa conseguir descer até elas. Por isso `FS_STAT` e `FS_LIST` (e só eles; nunca `FS_READ` nem escrita) aceitam também um diretório que seja **ancestral próprio** de alguma `read_root` e **não** esteja numa zona `never_read`:
- `FS_STAT` responde normalmente (é um diretório).
- `FS_LIST` responde só com as entradas `e` para as quais `visível(e)` vale: `e` está dentro de alguma `read_root` (e fora de `never_read`) **ou** `e` também é ancestral próprio de uma `read_root`. O resto do diretório (arquivos, pastas irmãs, `/luma`…) não aparece. Como a varredura filtrada lê entradas que não mostra (no 3DS cada uma custa ~10 ms), o agente lê no máximo **64 entradas por resposta** (`NDP_TRAVERSAL_SCAN_MAX`); uma resposta pode, então, vir com poucas entradas (até nenhuma) e `list_more = 1` — o cliente segue o `next_cursor` até `list_more = 0`.
- Qualquer outro caminho fora das raízes continua `PROTECTED_PATH`. A travessia não dá acesso ao conteúdo de nada.
Vetores: `traverse` em `vectors.json` (`travessível`/`visível` por configuração e caminho; inclui uma raiz dentro de `never_read`, que nunca serve de caminho).

### 11.2 Lista de pastas do dono
Quem escolhe o que é liberado é a pessoa **no console** (botão A → "Access folders"; nenhum comando do protocolo altera isto). A lista tem até **6 entradas** `{caminho normalizado, nível}`, nível `READ` ou `WRITE` (`WRITE` implica `READ`), e vira a política assim: `read_roots = [/3ds/nintendo-dev-agent] + todas as entradas`; `write_roots = [/3ds/nintendo-dev-agent] + entradas WRITE`; `never_*` = padrões (§11). A pasta do agente é sempre liberada.
- Um nível só pode ser concedido se o caminho não estiver numa zona protegida: `READ` fora de `never_read`, `WRITE` fora de `never_write` (exceto o que `write_except` libera; senão `PROTECTED_PATH`). Conceder `WRITE` em `/` é possível, e as zonas `never_write` continuam valendo dentro dele.
- Ciclo do botão Y numa pasta: nenhum → leitura → leitura+escrita → nenhum, pulando níveis já concedidos por uma pasta-mãe e os proibidos.
- Arquivo `/3ds/nintendo-dev-agent/config/access.bin`: `"NDPA"`, versão 1, `count u8`, 2 bytes zero, depois `count × {nível u8, len u8, caminho}` e o SHA-256 de tudo antes. Qualquer inconsistência (checksum, nível, caminho não canônico, duplicado, zona proibida) ⇒ **lista vazia** (só a pasta do agente): falha fechada. Escrita atômica (`.tmp` → verificar → `.bak` → rename). Vetores: `access` em `vectors.json` (operações, política resultante e os bytes do arquivo).

### 11.3 ACCESS_INFO (0x0011)
`REQ {}` → `RES {mode str, read_root str…, write_root str…}` (só depois do AUTH). Serve para o cliente (e o assistente) saber o que pode tocar, em vez de descobrir por erros `PROTECTED_PATH`.

### 11.4 DEVICE_INFO (0x0010)
`REQ {}` → `RES {model str?, firmware str?, ram_total u64?, app_mem_total/app_mem_free u64?, sys_mem_total/sys_mem_free u64?, sd_total/sd_free u64?}`. Depois do AUTH, em qualquer modo (só leitura). **Um campo só aparece se o console conseguiu medi-lo**: o agente nunca inventa valor (uma resposta vazia é válida). Sem suporte da plataforma → `UNSUPPORTED_COMMAND`; falha ao ler → `IO_ERROR`. No 3DS, ler o espaço livre do SD pode levar alguns segundos num cartão grande (o SO lê a FAT): o cliente usa o timeout longo. Os valores de memória descrevem o momento da consulta. `app_mem_*` é a região do app em primeiro plano; **o agente do 3DS não a informa** (ele toma toda a região ao iniciar, então o "livre" seria sempre 0, e o total seria o modo de memória do próprio CIA, não o que um jogo teria). `sys_mem_*` é informado.

## 12. Fluxo de recepção (normativo para o decoder)
Um decoder DEVE: acumular até 20 bytes; validar `magic`; ler `payload_len`; rejeitar `> max_frame` imediatamente; acumular `payload_len` (+16 se MAC); só então entregar o frame. Em erro de magic/flags a conexão é considerada desincronizada e DEVE ser fechada (não há ressincronização).

## 13. Comandos de filesystem (somente leitura)
Todos exigem HELLO prévio e passam pela política de leitura (§11) com o `path` normalizado. Falhas de política: `PATH_INVALID`, `PROTECTED_PATH`. Path ausente: `BAD_REQUEST`. Inexistente/inacessível como arquivo ou diretório: `NOT_FOUND`. Erro do SO: `IO_ERROR` (+ `os_result` quando houver).

### FS_STAT
`REQ {path}` → `RES {type, size, mtime}`. Só arquivos regulares e diretórios existem para o protocolo (links simbólicos e dispositivos são `NOT_FOUND`).

### FS_LIST
`REQ {path, cursor?}` → `RES {entry…, list_more, next_cursor}`.
- `path` deve ser diretório (senão `BAD_REQUEST`). `.` e `..` nunca aparecem. Nomes com mais de 255 bytes UTF-8 são omitidos (não endereçáveis, §10).
- Cada `entry` = `type u8 (1 arquivo, 2 diretório) ‖ size u64 ‖ name` (nome = o resto do valor, UTF-8, sem NUL). Ordem = a do diretório, estável entre páginas; o Bridge ordena se quiser.
- Uma página traz no máximo 100 entradas. `list_more = 1` significa que o Bridge deve repetir com `cursor = next_cursor`; a última página pode vir vazia com `list_more = 0`. `cursor` além do fim → página vazia, `list_more = 0`.

### FS_READ
`REQ {path, offset?, length?, chunk?, want_hash?}`.
1. Falhas de validação → `ERR` (nenhum dado é enviado): `offset > size` → `BAD_REQUEST`; diretório → `BAD_REQUEST`.
2. Sucesso → `RES {total_size, will_send}` com `will_send = min(length ou ∞, size − offset)`.
3. Depois, o agent envia **N frames `DATA`** (payload = bytes crus do arquivo, ≤ `chunk`; flag `MORE` em todos menos no último) e por fim um frame **`END`** (payload vazio, ou `{sha256}` se `want_hash = 1`). Todos repetem `request_id` e `command` da REQ. `will_send = 0` → RES seguido direto de END.
4. Erro no meio (arquivo encolheu, erro de SD) → um frame `ERR` no lugar do `END`; o Bridge descarta o que recebeu.
5. O Bridge NÃO DEVE enviar REQ antes do `END`/`ERR` da transferência. Se o fizer, o núcleo do agent responde `BUSY` ao REQ (a transferência continua); o servidor de referência (`agent/posix`) simplesmente não lê o socket durante o streaming, então o REQ só é processado depois do fim. Se a conexão cair, a transferência é abortada e o arquivo fechado.
6. `sha256` cobre exatamente os bytes enviados em DATA.

## 14. Comandos de escrita (exigem modo DEVELOPMENT ou FULL)
Passam pela política de **escrita** (§11): `FORBIDDEN_MODE` em `READ_ONLY`; `PROTECTED_PATH` fora das `write_roots` ou dentro de uma zona `never_write`; `PATH_INVALID`. O agent NUNCA cria diretórios implicitamente (o pai deve existir, senão `NOT_FOUND`) e NUNCA apaga nada de forma definitiva por pedido do Bridge (`FS_DELETE` só move para a lixeira).

### FS_MKDIR
`REQ {path}` → `RES {}`. Já existe (arquivo ou diretório) → `EXISTS`.

### FS_WRITE
Sequência (o Bridge espera o `RES` de "pronto" antes de enviar DATA):
```
Bridge → REQ FS_WRITE {path, size, overwrite?, backup?, sha256?}
Agent  → RES {max_chunk}                       (ou ERR: nada foi criado)
Bridge → DATA … DATA (MORE em todos menos no último) → END   (mesmo request_id)
Agent  → RES {written, sha256, replaced}       (ou ERR)
```
- `size` (obrigatório) é o total de bytes. `size = 0` → nenhum DATA, só END. Cada DATA ≤ `max_chunk`.
- `sha256` opcional no REQ = digest esperado; o agent calcula sempre o SHA-256 do que recebeu e o devolve no RES final para o Bridge conferir de ponta a ponta.
- **Atomicidade:** o agent grava em `<path>.ndp-tmp` (mesmo diretório; sufixo reservado — um `path` que termine em `.ndp-tmp`/`.ndp-old`/`.bak` é `BAD_REQUEST`; um temp antigo deixado por queda de energia é removido). Só no `END`, depois de conferir tamanho e hash e de sincronizar o arquivo, ocorre a troca:
  1. destino existe e `overwrite = 0` → `EXISTS` (o temp é apagado);
  2. `overwrite = 1`: o destino vai para `<path>.ndp-old`, o temp vira o destino; se isso falhar o antigo é restaurado; depois o `.ndp-old` é apagado, ou vira `<path>.bak` se `backup = 1`.
- Falha em qualquer ponto → `ERR`, temp apagado, destino intacto. Após um `ERR` no meio, o agent descarta os DATA restantes desse `request_id` até o `END` ou até o próximo REQ.
- `size` ≠ bytes recebidos → `BAD_REQUEST`; `sha256` ≠ calculado → `HASH_MISMATCH`.
- Enquanto o upload está ativo, qualquer outro REQ recebe `BUSY`. Mudar o modo para `READ_ONLY` ou fechar a conexão aborta o upload.

### FS_DELETE (lixeira)
`REQ {path}` → `RES {trash_path}`. **Não remove dados**: o item (arquivo ou diretório inteiro, com o conteúdo) é *movido* por `rename` (atômico, mesma partição) para `<raiz>/.ndp-trash/<nome>`, onde `<raiz>` é a mais específica das `write_roots` que contém o `path`.
- Passa pela política de escrita (§11): `FORBIDDEN_MODE`, `PROTECTED_PATH` (fora das raízes ou em zona `never_write`), `PATH_INVALID`.
- `path` igual a uma `write_root` → `BAD_REQUEST` ("cannot delete a write root"). `path` já dentro de uma lixeira → `BAD_REQUEST` (remoção definitiva não existe no protocolo). Inexistente → `NOT_FOUND`.
- A lixeira (`.ndp-trash`) é criada sob demanda (um `mkdir`, que no 3DS leva ~6 s na primeira vez por raiz). Nome de destino = o nome original; se já existir, `<nome>.1`, `<nome>.2`… (o sufixo `.N` não usa `~`, então continua endereçável).
- `FS_WRITE`/`FS_MKDIR` para dentro de uma lixeira → `PROTECTED_PATH` (a lixeira é gerenciada pelo agent). A leitura funciona normalmente (é como se recupera o conteúdo: ler e/ou o usuário renomear pelo cartão).
- Falha no `rename` → `IO_ERROR`, item intacto no lugar original.
