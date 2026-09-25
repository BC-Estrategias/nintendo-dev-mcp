# Toolchain do plugin 3GX (spike) — como foi montado

Tudo fica em `third_party/` (fora do Git, com licenças próprias) e nada é instalado no sistema.

| Peça | Origem | Ajuste necessário no macOS/devkitARM atual |
|---|---|---|
| `libctrpf` 0.7.3 | `https://github.com/Tekito-256/CTRPluginFramework` (espelho de `gitlab.com/thepixellizeross/ctrpluginframework`); `make dist-bin` em `Library/` | `-Werror` removido do Makefile (GCC 16 avisa `overloaded-virtual`); `make libcwav` antes |
| `3gxtool` v1.3 (formato **3GX$0002**, o que o Luma 13.3.3 exige) | `https://gitlab.com/thepixellizeross/3gxtool` | sem CMake: compilado à mão com yaml-cpp 0.8 embutido; `dynalo` (sem suporte a macOS) trocado por um shim de `dlopen`; comparador do `std::sort` de símbolos corrigido (violava a ordem fraca estrita e derrubava o libc++) |
| Template | `https://github.com/PabloMK7/CTRPluginFramework-BlankTemplate` (usa `-lctrpf -lctru`) | — |

Não usar `Nanquitas/3gxtool` nem o template antigo: geram `3GX$0001` e o Luma responde `0xD8E07402` ("Outdated plugin file").

Build do spike: `NDEV_SPIKE_HOST=<IP do Mac, 4 bytes separados por vírgula> ./scripts/build-3gx-spike.sh`.
