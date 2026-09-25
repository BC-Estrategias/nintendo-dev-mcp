# Agente no lugar do "Notas de jogo" — estudo de viabilidade (só leitura, 2026-09-25)

Ideia: com um jogo suspenso, abrir "Notas de jogo" no HOME e, em vez do applet da Nintendo, rodar o agente (rede + SD), sem fechar o jogo. **Não roda em paralelo com o jogo** (o jogo fica suspenso): serve para ler/gravar arquivos e logs entre duas partes do jogo, não para log em tempo real.

## O que se verificou (fontes: código do Luma3DS e do libctru, 3dbrew)
1. **O Luma permite trocar o código e o exheader de um título que não seja módulo de sistema.** `sysmodules/loader/source/loader.c`: com `enable_game_patching = 1` (o seu console tem), o loader lê `/luma/titles/<TID>/exheader.bin` e `/luma/titles/<TID>/code.bin`. Só módulos de sistema (`00040130…`) exigem também `enable_external_firm_and_modules`. `exheader.bin` = exheader **decifrado** (0x400 ou 0x800 bytes); `code.bin` = código **decifrado e descomprimido**, com tamanho ≤ ao mapeado pelo exheader (que nós fornecemos).
2. **Alvo:** o Game Notes é um *system applet* (`00040030…`), AppID `0x113` (libctru). Na região USA (o seu console: o HOME Menu é `0004003000008F02`), o TID é **`0004003000009302`** (tabela de TIDs do 3dbrew: JPN 8702, USA 9302, EUR 9C02, CHN A502, KOR AD02, TWN B502). Usa a região de memória **SYSTEM**.
3. **Serviços/permissões:** vêm do exheader, que passaríamos a controlar: listaríamos `soc:U`, `fs:USER` (com `DirectSdmc`), `ac:u`, `ps:ps`, `ndm:u`, `hid:USER`, `gsp::Gpu`, `cfg:u` e `APT:A`/`APT:S`. O `libctru` já tenta `APT:S`, depois `APT:A`, depois `APT:U` (`apt.c`), e usa `envGetAptAppId()`, que pode ser sobrescrito (`__apt_appid = 0x113`).
4. **O nosso código já roda como CIA** (aplicativo) no console: `psInit`, `acInit`, sockets e SD funcionam com as permissões do rsf. Falta a versão "applet".

## Incógnitas que só um teste no console resolve
- **Handshake com o HOME:** o HOME espera que o applet responda ao APT (inicialização, `WAKEUP`, ciclo de suspensão). Se falhar, o HOME mostra erro ou trava por alguns segundos; sai apagando `/luma/titles/0004003000009302/`.
- **Memória:** o `libctru` aloca o heap na partida a partir da região do aplicativo (`__system_allocateHeaps`); num applet de região SYSTEM isso pode falhar (`svcBreak`, como no TMC3DS). É preciso sobrescrever essa função e reservar um tamanho fixo pequeno.
- **Vídeo e teclas** no applet (GSP/HID) e **a saída** (voltar ao HOME/jogo sem deixar o sistema em estado estranho).
- **Rede dentro de um applet** (`soc:U` no exheader; o tamanho do `soc` buffer).

## Riscos
- Baixo. Só o "Notas de jogo" é afetado; nada do jogo nem dos saves. Desfazer = apagar `/luma/titles/0004003000009302/` (o agente consegue, pois `/luma/titles` é uma exceção de escrita). Um `exheader.bin` inválido faz o Luma dar `svcBreak` (assert) ao abrir o applet: tela de erro do Luma, sem dano.
- Não usar o Rosalina aberto por muito tempo (bug do `mcu` com o LED de carga).

## Plano em etapas (cada uma com rollback)
0. **Prova de vida:** um mini-applet que só liga a tela, escreve uma linha e sai com qualquer tecla. Constrói-se um CXI com `makerom` (`Category: Applet`, `MemoryType: Base`, sem compressão), extrai-se `exheader.bin` e `code.bin`, envia-se para `/luma/titles/0004003000009302/`.
1. Rede: o mesmo applet abre uma conexão TCP para o Mac e envia uma linha.
2. O agente completo (protocolo, pareamento, lista de pastas) dentro do applet.
3. UI: aviso claro "jogo suspenso" e retorno limpo.
