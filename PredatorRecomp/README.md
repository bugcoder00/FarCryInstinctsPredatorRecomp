# PredatorRecomp

Recompilador estático autoral: executável do console (PowerPC 64 bits + VMX + VMX128) → C, e microcódigo de shader → HLSL.
Escrito do zero a partir das especificações do hardware, sem código de outros recompiladores.

## Compilar

```
.\build.ps1            # Ninja + clang-cl do Visual Studio 2022; roda os testes (ctest)
```

## Uso

```
predatorrecomp info    default.xex --image image.bin                         # cabeçalhos, regiões, importações
predatorrecomp disasm  default.xex --image image.bin --start 0x826444F0 --count 32
predatorrecomp disasm  default.xex --image image.bin > tudo.txt              # todo o código
predatorrecomp cpu     default.xex --image image.bin --out gerado            # gera o C do jogo em gerado/cpu
predatorrecomp shaders --shaders pasta_com_bin --out gerado                  # traduz shaders extraidos -> gerado/shaders/*.hlsl
predatorrecomp --help
```

`--image` é a imagem de memória já descriptografada; o executável do jogo é criptografado (o carregador lê sozinho
XEX2 sem criptografia, com ou sem compressão básica).

## Jogo (PredatorGame)

```
.\game\build_game.ps1          # gera o C (out/cpu) e compila build_game/PredatorGame.exe
.\game\build_game.ps1 -SkipGenerate
```

O PredatorGame junta o C gerado com o runtime do jogo (`runtime/`: kernel, arquivos, vídeo, áudio, controle).
A ponte fica em `game/bridge/`:

- `guest_cpu_backend.h`: o contexto da CPU para o runtime é a própria `PredatorCpuContext`; o runtime acessa os
  registradores só por funções (`Gpr32(ctx, 3)`, `SetGpr`, `Fpr`, `Lr`...) e macros (`GUEST_FUNC`, `GUEST_ORIGINAL`),
  listadas em `runtime/guest.h`.
  nenhum cabeçalho, nome ou estrutura de outros recompiladores entra no PredatorGame.
- `predator_bridge.cpp`: chamadas indiretas (tabela endereço → função), importações (biblioteca + ordinal → nome →
  função `KFUNC` do runtime), `sc`, armadilhas, instruções sem tradução e o relógio (`mftb`).
- Funções do jogo substituídas pelo runtime: o gerador define cada função como `sub_X_original` com `sub_X` como
  apelido; o runtime define `sub_X` com `GUEST_FUNC(sub_X)` e chama a original por `GUEST_ORIGINAL(sub_X)`.

## Estado

| Parte | Situação |
|---|---|
| Linha de comando (`src/core`) | pronta, com testes |
| Carregador XEX2 (`src/loader`) | cabeçalhos, mapa de páginas (código/dados), importações, imagem (sem criptografia ou `--image`) |
| Decodificador (`src/cpu/Decoder`, `OpcodeTable.inc`) | ~330 instruções: inteiros, desvios, memória, ponto flutuante, VMX, VMX128. 100% do código do jogo reconhecido; 99,91% idêntico a um gabarito independente (as diferenças são de notação) |
| Desmontagem (`src/cpu/Disassembler`) | sintaxe usual + mnemônicos simplificados |
| Descoberta de funções (`src/cpu/Analysis`) | `.pdata` (16.914), alvos de `bl`, 205 importações (pelos registros do XEX, mesmo com thunks apagados), ponteiros nos dados e montados com `lis`+`addi`, lacunas, entradas no meio de funções (traduzem o corpo inteiro e começam com `goto`); 22.538 funções; 759 tabelas de `switch` (as mesmas 759 de um analisador independente; os 410 `bctr` restantes são chamadas virtuais) |
| Geração de C (`src/cpu/CodeGen`, `runtime/predator_cpu.h`) | `PredatorCpuContext`; 2,2 milhões de instruções em 56 arquivos C; **todas** as instruções do jogo traduzidas (incluindo `vpkd3d128`/`vupkd3d128`; a cor D3DCOLOR usa a base 3.0, `0x404000xx`, e não 1.0: com 1.0 o menu saía branco, bug 1). Semântica do modo 32 bits do console: `Rc`, `CA`, `OE` e `bdnz` olham os 32 bits de baixo |
| Jogo (`game/`) | o PredatorGame roda com o runtime: vídeos de abertura com som, menu inicial a 30 FPS |
| Tradutor de shaders (`src/gpu`) | `XenosMicrocode` separa os campos (controle de fluxo, ALU vetor+escalar, leituras de vértice/textura); `XenosHlsl` gera o corpo HLSL. Cada instrução de ALU lê todos os operandos antes de gravar (vetor e escalar rodam em paralelo no hardware); laços usam a constante inteira (contagem, início e passo de `aL`); sem laços/saltos para trás o código sai em linha reta. Usado pelo runtime (`runtime/xenos_translate.cpp` monta o resto do shader). 170 shaders do menu e da 1ª fase: todos traduzidos e compilados pelo DXC (do Windows SDK) |

## Organização

```
src/core     linha de comando, diagnósticos, pipeline
src/loader   XEX2 e imagem de memória
src/cpu      tabela de instruções, decodificador, desmontagem, tradutor de CPU
src/gpu      tradutor de shaders (tambem compilado dentro do runtime do jogo)
tests        testes (ctest)
```

A tabela `src/cpu/OpcodeTable.inc` é a fonte única das instruções: máscara, valor, forma dos operandos e flags
(Rc, OE, LK, AA, `rA|0`). Dentro do mesmo opcode primário as entradas mais específicas vêm primeiro.
