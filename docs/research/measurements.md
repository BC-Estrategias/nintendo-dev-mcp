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

## Pendências de medição
- RTT após a correção do redesenho (v0.1.1).
- Vazão de leitura/escrita do SD e de rede (chunk 16 vs 64 KiB) — M4/M5.
- Custo de SHA-256 no ARM11.
