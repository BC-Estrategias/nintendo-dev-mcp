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

## 4. Autenticação por frame (MAC)
Quando `flags.bit0 = 1`, 16 bytes seguem o payload:
```
mac = HMAC-SHA256(session_key, u64le(counter) ‖ header[0:20] ‖ payload)[0:16]
```
`counter` é um contador por direção, começando em 0 e incrementando a cada frame **com MAC**. Um frame com MAC inválido ou contador fora de ordem DEVE ser descartado e a conexão fechada. A derivação de `session_key` e o pairing serão especificados no marco M5; o M0 define apenas o formato e o cálculo do MAC. Até lá o agent anuncia `auth = "none"`.

## 5. Comandos
| ID | Nome | M0 | Descrição |
|---|---|---|---|
| 0x0001 | HELLO | ✔ | negocia protocolo, anuncia plataforma |
| 0x0002 | PING | ✔ | eco de nonce |
| 0x0010 | DEVICE_INFO | M2 | |
| 0x0020 | FS_LIST | M3 ✔ | lista um diretório (paginado) |
| 0x0021 | FS_STAT | M3 ✔ | tipo, tamanho, mtime |
| 0x0022 | FS_READ | M4 ✔ | lê arquivo em streaming |
| 0x0030 | FS_WRITE | M5 ✔ | grava um arquivo (temp → validar → rename) |
| 0x0031 | FS_MKDIR | M5 ✔ | cria um diretório |
| 0x0032 | FS_RENAME | pós-MVP | |
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
Entrada: `mode` (`READ_ONLY` < `DEVELOPMENT` < `FULL`), `op` (`read`|`write`), path, e a configuração `{read_roots, write_roots, never_read, never_write}` (listas de paths normalizados).

Avaliação, na ordem (a primeira que falha encerra):
1. path inválido → `PATH_INVALID`.
2. `op = write` e `mode = READ_ONLY` → `FORBIDDEN_MODE`.
3. path em alguma `never_*` da operação → `PROTECTED_PATH`.
4. path fora de todas as raízes (`read_roots` / `write_roots`) → `PROTECTED_PATH`.
5. caso contrário → `OK`.

Padrões: `read_roots = ["/"]`; `write_roots = ["/3ds/nintendo-dev-agent"]`;
`never_write = ["/Nintendo 3DS", "/luma", "/boot.firm", "/gm9", "/private", "/3ds/nintendo-dev-agent/config"]`;
`never_read = ["/3ds/nintendo-dev-agent/config"]` (política e chaves).
Um root ou zona pode ser um arquivo (`/boot.firm`). A configuração só é alterada com confirmação física no console (ver `ARCHITECTURE.md`); o protocolo NÃO tem comando para isso.

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
Passam pela política de **escrita** (§11): `FORBIDDEN_MODE` em `READ_ONLY`; `PROTECTED_PATH` fora das `write_roots` ou dentro de uma zona `never_write`; `PATH_INVALID`. O agent NUNCA cria diretórios implicitamente (o pai deve existir, senão `NOT_FOUND`) e NUNCA apaga nada por pedido do Bridge (não há comando de remoção).

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
