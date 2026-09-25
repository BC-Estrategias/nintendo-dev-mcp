# Roteiro de teste em hardware

Cada marco que toca o console tem um roteiro aqui. Marque o que passou e **cole o que falhou** (fotos das telas + `agent.log`) para diagnosticarmos por camada.

## M1 — agente 3DS: Wi-Fi, IP, porta, HELLO/PING

**Requisitos:** New 3DS com Luma3DS + Homebrew Launcher; 3DS e Mac na **mesma rede Wi-Fi** (sem "isolamento de clientes/AP isolation"; o 3DS só usa 2.4 GHz).

### 1. Instalar
- **Cartão SD:** copie `dist/nintendo-dev-agent.3dsx` para `SD:/3ds/nintendo-dev-agent/nintendo-dev-agent.3dsx`.
- **Ou por Wi-Fi (`3dslink`):** no 3DS abra o Homebrew Launcher e aperte **Y** (netloader; mostra o IP). No Mac:
  ```bash
  /opt/devkitpro/tools/bin/3dslink -a <IP_DO_3DS> dist/nintendo-dev-agent.3dsx
  ```

### 2. Abrir e conferir a tela
| Onde | Esperado |
|---|---|
| Superior | `Nintendo Dev Agent  v0.1.2`, `Protocol 1`, `Status: ONLINE` (verde), `IP: 192.168.x.x`, `Port: 6464`, `Bridge: not connected`, `Mode: READ_ONLY` |
| Inferior | linhas `Nintendo Dev Agent v0.1.2…`, `Console: New 3DS family`, `acInit/psInit/ndmuInit: 0x00000000`, `Wi-Fi connected`, `Network services ready`, `Listening on 192.168.x.x:6464` |

Se aparecer `NETWORK ERROR` ou algum `0x…` diferente de zero, **pare e me mande a tela** (o código é o diagnóstico).

### 3. Ping a partir do Mac
```bash
node bridge/packages/cli/src/main.ts ping <IP_DO_3DS> -c 10
```
Esperado: `Agent: 3ds  v0.1.2  protocol 1  mode READ_ONLY  auth none  max_frame 65536`, dez linhas `PONG`, e a linha `min/avg/max`. **Anote os tempos** (é a nossa primeira medida de latência real). Na tela inferior devem aparecer `[CONNECT]`, `[REQ n] HELLO`, `[OK n]`, `[REQ n] PING`…; a superior mostra `Bridge: CONNECTED`, depois volta a `not connected`.

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
