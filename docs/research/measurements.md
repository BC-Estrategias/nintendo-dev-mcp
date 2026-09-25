# Medições em hardware

Registro de medições reais (New 3DS, Wi-Fi 2.4 GHz, Mac na mesma LAN). Cada linha diz o que foi medido, como e a conclusão — e o que ainda é hipótese.

## 2026-09-24 — M1, agente v0.1.0 (Nintendo Dev Agent, HELLO/PING)

Ferramenta: `ndev ping <ip> -c N -i MS`. RTT = tempo do PING (TCP, `TCP_NODELAY` no Mac) medido no Bridge, com a conexão já aberta.

| Intervalo entre pings | RTT médio | mín / máx |
|---|---|---|
| 0 ms (em sequência) | 34,6 ms | 29,9 / 50,4 |
| 50 ms | 15,2 ms | 10,1 / 46,0 |
| 100 ms | 13,6 ms | 9,8 / 47,7 |
| 150 ms | 14,2 ms | 9,8 / 47,7 |
| 200 ms | 15,1 ms | 10,4 / 35,4 |
| 300 ms | 105,0 ms | 10,9 / 211,5 |
| 1000 ms | 74,4 ms | 13,0 / 128,2 |

Conexão "a frio" (20 s parado; CLI completo, incluindo início do Node): 368–483 ms, 3/3 sucessos.

**Conclusões**
1. **Latência de rede "quente" ≈ 10–15 ms.**
2. **Redesenho da tela custa ~20 ms por requisição** (34,6 → ~14 ms ao espaçar os pedidos): `gspWaitForVBlank` bloqueia o laço e o próximo pedido, que chega logo após a resposta, espera. Causa confirmada indiretamente (espaçar os pedidos remove o efeito); correção na v0.1.1: redesenho limitado a 4×/s. *Reavaliar com medição após a correção.*
3. **Após ~250 ms sem tráfego a latência sobe para 75–200 ms.** Hipótese (não verificada com captura de pacotes): economia de energia do rádio Wi-Fi do console (o pacote só é entregue no próximo beacon/TIM). Implicação: a primeira requisição depois de uma pausa custa até ~200 ms; transferências contínuas não sofrem.
4. **Primeira conexão da sessão falhou uma vez com `EHOSTUNREACH`** (ARP do 3DS ainda não resolvido); não reproduziu depois. Correção: o Bridge repete a conexão (3 tentativas, pausa 250/500 ms) para erros transitórios; `ECONNREFUSED` não é repetido.
5. O 3DS **não responde a ICMP echo** (`ping` comum dá 100% de perda) — não usar `ping` do sistema como teste de vida; usar `ndev ping`.

## 2026-09-24 — M1, agente v0.1.1 (redesenho limitado a 4×/s)

| Teste | Resultado |
|---|---|
| 30 pings em sequência (`-i 0`) | **17,5 ms** médio (mín 11,2 / máx 45,5) — antes: 34,6 ms |
| 30 pings, `-i 100` | 16,8 ms (10,0 / 45,1) |
| 15 pings, `-i 300` | 92,0 ms (15,1 / 111,4) — economia de energia do rádio, sem mudança |
| 500 pings em sequência | 0 falhas, médio 19,2 ms (mín 9,9 / **máx 408,7**) — um pico isolado de ~400 ms — **ver análise do agent.log abaixo: foi dentro do agente, não no Wi-Fi** |
| Nova conexão substitui a anterior | ok: a conexão A recebe `ECONNRESET`, B segue funcionando |
| 20 reconexões seguidas (connect+hello+ping+close) | 20/20 ok, 57 ms cada |

**Conclusão:** a correção do redesenho reduziu ~17 ms por requisição em sequência (confirmado). O agente ficou estável em 500 requisições e em reconexões rápidas.

## 2026-09-24 — análise do `agent.log` do SD (3 sessões, 781 requisições)

Fonte: `agent.log` copiado do cartão SD pelo usuário. Sessões: v0.1.0 (21:09–21:20), v0.1.1 (21:23–21:47), v0.1.1 (21:48–21:50).

**Correções a conclusões anteriores (feitas por mim, refutadas pelo log):**
- ~~"Depois do HOME o agente perde o listener e se recupera"~~ → **falso.** O `ECONNREFUSED` observado ocorreu porque o app estava **fechado** (`Exiting` 21:47:54, novo `agent start` 21:48:08). Não há nenhum `Wi-Fi lost` nas 3 sessões. Efeito do HOME em si: **sem evidência** (o app suspenso não escreve log).
- ~~"Pico de ~408 ms foi retransmissão do Wi-Fi"~~ → **falso.** O agente registrou `[OK] 396 ms` naquele pedido: o atraso foi **dentro do agente**.

**Achados:**
1. Tempo no agente por requisição (`[OK n] X ms`): 7 ms ×35, **8 ms ×553**, 9 ms ×132, 10–13 ms ×61, 396 ms ×1. Mediana 8 ms ≈ 70% do RTT de ~11 ms visto no Bridge → só ~3 ms são rede.
2. Suspeita: `fflush` a cada linha de log no SD (2 linhas por requisição, uma delas dentro da janela medida) custa ~7 ms e explica também o pico de 396 ms. **Hipótese** — a v0.1.2 passa a bufferizar o log e a descarregar só com o link ocioso; esperado: `[OK]` de ~1–2 ms.
3. 12 linhas `[CLOSE] replaced by a new connection` seguidas às 21:24:10–11: são as reconexões rápidas do teste (o novo connect chega antes de o agente processar o FIN do anterior). Inofensivo.
4. Início/fim limpos nas 3 sessões (`Exiting` + `agent exit`): sair com START e reabrir funciona, e a porta 6464 volta.

## 2026-09-24 — v0.1.2 (log do SD em buffer): hipótese confirmada

Versão confirmada pelo próprio agente (`ndev hello` → `v0.1.2`).

| Teste | v0.1.1 | v0.1.2 |
|---|---|---|
| 30 pings em sequência | 17,5–18,3 ms | **4,3 ms** (mín 1,8 / máx 33,8) |
| 30 pings, `-i 100` | 14,4–16,8 ms | **2,9 ms** (1,9 / 7,6) |
| 300 pings em sequência | 18,2 ms | **3,7 ms** (1,6 / 54,2) |
| 15 pings, `-i 300` | 85,7–92 ms | 85,7 ms (5,1 / 111,8) — economia de energia do rádio, inalterada |

**Conclusão (confirmada por medição):** o `fflush` por linha no cartão SD custava ~13–14 ms por requisição (~75% do RTT) e causava o pico de ~400 ms. Com o log em buffer, o RTT de rede "quente" é ~2–4 ms. Os picos restantes (30–55 ms, raros) são compatíveis com o redesenho (≤4×/s, ~20 ms) e com o flush periódico; não investigados.

## 2026-09-24 — M3+M4 no hardware (agente v0.2.0, SD real com ~54 itens na raiz)

**Aceitação do projeto:** `ndev cat <ip> /3ds/nintendo-dev-agent/test.txt` devolveu exatamente `Hello from Nintendo 3DS` (23 bytes, conferido byte a byte). `mtime` é sempre **desconhecido** (0): o `stat` do `sdmc:` não fornece data (como já se via no ftpd).

| Medida | Resultado |
|---|---|
| `FS_STAT` no agente | **~16–17 ms** por chamada |
| `FS_READ` de 23 bytes (agente) | ~19 ms (abrir+ler) |
| `ndev ls /` (54 entradas) | 517 ms; `ndev ls /3ds` (43 entradas) 459 ms → **~8–10 ms por entrada** (um `stat` cada) |
| `get` de 49 756 B | 82 ms |
| `get` de 3,88 MB, chunk 16 KiB | 995 KiB/s |
| `get` de 3,88 MB, chunk 32 KiB | 1078 KiB/s |
| `get` de 3,88 MB, chunk ~64 KB | 1082 KiB/s |

Hash SHA-256 conferido contra o agente em todos os downloads.

**Conclusões**
1. **Vazão de leitura ≈ 1,0–1,1 MiB/s**, quase independente do chunk (16 KiB é ~8% mais lento; 32 e 64 KiB empatam) → **mantido 32 KiB** como padrão. A divisão do gargalo entre SD e Wi-Fi **não está medida** (hipótese: Wi-Fi 2.4 GHz do console). Precisa de um benchmark que separe as duas partes.
2. **Listar custa ~10 ms por entrada** por causa do `stat` que traz o tamanho. Uma pasta com 500 arquivos levaria ~5 s. Próxima melhoria: tornar o tamanho opcional (`want_size`) e obter o tipo sem `stat` quando o `readdir` informar (não verificado se o `sdmc:` do libctru preenche `d_type`).
3. O `agent.log` pode ser lido remotamente e reflete o instante atual (flush antes de ler a pasta do agente).

## 2026-09-25 — M5 (escrita) no hardware, agente v0.3.0

**Aceitação:** `ndev put <ip> --text "Codex was here." /3ds/nintendo-dev-agent/from-codex.txt` criou o arquivo (15 bytes); `ndev cat` devolveu exatamente o mesmo texto. Sem sobrescrita silenciosa (`EXISTS`, conteúdo intacto); `--replace --backup` gerou `from-codex.txt.bak` com a versão anterior; nenhum `*.ndp-tmp`/`*.ndp-old` sobrou.

| Operação (tempo dentro do agente, do `agent.log`) | Resultado |
|---|---|
| escrita de arquivo novo pequeno (temp+rename) | **~85–93 ms**, estável (10 amostras, inclusive após 60 s parado) |
| substituição com rename duplo + remove | 140 ms |
| `FS_STAT` / `FS_LIST` (6 entradas) / leitura de arquivo pequeno | ~12 ms / 46 ms / ~20 ms |
| **`FS_MKDIR`** | **~5,75 s, determinístico** (5744, 5757, 5749, 5770 ms; 4 de 4) |
| **primeira escrita da sessão** (`from-codex.txt`) | **5815 ms**, não reproduzida depois (as 10 seguintes: ~85 ms) |

**Achados**
1. **`mkdir` é sempre ~5,7 s no SD deste console.** Causa **desconhecida** (o agente não discrimina qual chamada demora: `mkdir` do libctru, `stat`, sync?). Cria o diretório corretamente. A v0.3.1 passa a registrar `WARN slow <op> <ms>` para qualquer operação de SD ≥ 150 ms, para identificar a chamada.
2. **A primeira escrita de 5,8 s (quase igual ao mkdir) é inexplicada**: não voltou a ocorrer. Hipóteses não testadas: cache/alocação do FS do console, ou o cartão. Sem evidência para escolher.
3. **Problema real que o achado expôs (corrigido):** o Bridge desistia aos 5 s e o resultado ficava ambíguo, embora o agente concluísse e gravasse. Agora: `mkdir` espera 30 s; escrita espera 60 s pela confirmação final; se ainda assim não houver confirmação, o erro diz que o arquivo pode ou não ter sido gravado e manda conferir com `ndev stat`. Verificado no hardware: `mkdir` concluiu em 6,2 s.
4. **O IP do 3DS mudou de um dia para o outro** (DHCP: `192.168.15.14` → `.17`; o MAC do `.14` passou a ser de outro aparelho). O Mac também mudou de IP. Achei o console varrendo a porta 6464 da /24. Isso eleva a prioridade da **descoberta automática** e de reservar IP no roteador.
5. Ficaram arquivos de teste em `/3ds/nintendo-dev-agent/inbox/` (`t1..t5.txt`, `frio.txt`, `quente.txt`, `d1..d4/`) e `from-codex.txt(.bak)`; o agente não tem comando de remoção (por desenho) — apagar pelo cartão/outro app.

## 2026-09-25 — diagnóstico do `mkdir` lento (agente v0.3.1, instrumentado)

`WARN slow mkdir 5708 ms /3ds/nintendo-dev-agent/inbox/d5` — **a chamada `mkdir` do `sdmc:` (libctru) sozinha leva ~5,7 s**; nenhuma outra chamada (`stat`, `file_create`, `file_write`, `file_sync`, `file_close`, `rename`, `remove`) passou de 150 ms em todo o teste. Bridge total: 5934 ms. Upload de 3 MiB: **900 KiB/s** (3412 ms), sem nenhum aviso de lentidão.

Causa **dentro** do `mkdir` do serviço de arquivos do console: desconhecida. Hipótese (não verificada): a criação de diretório dispara varredura/atualização da FAT do cartão (cartão grande), que criação de arquivo evita. Não há como corrigir no agente; contorno: **evitar `mkdir`** (uma vez por pasta, custo ~6 s) e esperar 30 s no Bridge. A primeira escrita de 5,8 s **continua sem explicação** (não reproduziu com a v0.3.1).

**Vazão de escrita (2 medidas):** 611 KiB/s (240 KB, um arquivo) e 900 KiB/s (3 MiB).

## 2026-09-25 — servidor MCP contra o console real (agente v0.4.0)

Roteiro de 14 chamadas via `ndev-mcp` (stdio), **sem informar IP** (descoberta automática): tudo funcionou como esperado. Descoberta: **2,1 s** (varredura das sub-redes + HELLO); `ping` 3,1 ms de média (2,5–4,8); `fs_list` 87 ms; `fs_read` 28 ms; `fs_write` de 25 bytes 106 ms; upload de 24 bytes 89 ms; download 30 ms; delete para a lixeira 56–74 ms (a lixeira já existia). `EXISTS` e `PROTECTED_PATH` voltaram com mensagens acionáveis e sem alterar nada. O log de auditoria registrou tudo sem o conteúdo, e o último IP ficou no cache.

## Pendências de medição
- Separar SD × Wi-Fi na vazão de leitura (benchmark); **vazão de escrita de arquivo grande**.
- Por que a primeira escrita da sessão levou 5,8 s (não reproduzida); por que o `mkdir` do sistema demora ~5,7 s.
- Custo de SHA-256 no ARM11.
