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

## Pendências de medição
- Separar SD × Wi-Fi na vazão de leitura (benchmark); vazão de escrita — M5.
- Custo de SHA-256 no ARM11.
