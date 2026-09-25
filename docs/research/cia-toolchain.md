# CIA do agente — ferramentas e decisões

`./scripts/build-3ds-cia.sh` gera `dist/nintendo-dev-agent-vX.Y.Z.cia` (título `000400000BD00100`, código `CTR-P-NDEV`, instala no SD).

Ferramentas (em `third_party/bin/`, fora do Git, compiladas localmente):
| Ferramenta | Origem | Ajuste |
|---|---|---|
| `makerom` | `github.com/3DSGuy/Project_CTR` (`make deps && make` em `makerom/`) | nenhum |
| `bannertool` | `github.com/carstene1ns/3ds-bannertool` (espelho do de Steveice10; CMake) | 1 linha: `u8 pad[padLength] = {0}` (VLA com inicializador) → `u8 pad[4] = {0}` |

Decisões:
- **Serviços/permissões** modelados num CIA homebrew que já funciona no console (o do TMC3DS): `APT:U`, `ac:u`, `soc:U`, `cfg:u`, `fs:USER`, `gsp::Gpu`, `hid:USER`, `ndm:u`, `ps:ps` (aleatório seguro do pareamento), `DirectSdmc/DirectSdmcWrite`. Memória: `SystemMode: 64MB`, sem modo estendido.
- **Versão:** este `makerom` descarta os bits de "minor" do `-ver`; por isso a versão instalada é `(minor<<10)|patch` (0.6.3 aparece como 6.0.3): sempre crescente, o que basta para o FBI atualizar no lugar.
- Banner/ícone gerados por `agent/3ds/cia/make_banner_assets.py` (PNG e WAV puros, sem bibliotecas); o ícone vem do `.smdh` do agente.
- O CIA é um **aplicativo**: abrir pelo Home Menu fecha o jogo em andamento (igual ao 3dsx).
- Não foi instalado nem testado no console (instalação é com o FBI, pela pessoa).
