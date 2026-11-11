# Far Cry Instincts Predator Recomp

**Português** · [English](#english)

Versão para PC (Windows, DirectX 12) de *Far Cry Instincts Predator* (Xbox 360), feita por **recompilação estática**:
o código do jogo é traduzido de PowerPC para C++ e roda nativo, junto com um runtime próprio (kernel, arquivos,
vídeo, áudio, controles e rede). O recompilador e o runtime foram escritos do zero.

> **Projeto de fã, sem ligação com a Ubisoft ou a Microsoft.**
> Este repositório e os pacotes **não têm nenhum arquivo do jogo**. Para jogar é preciso ter a **sua própria cópia
> original** do jogo de Xbox 360. Não peça nem poste links do jogo aqui.

## Jogar

1. Baixe o pacote na aba **Releases** e extraia numa pasta onde você possa gravar (ex.: `Documentos`).
2. Extraia o disco da **sua** cópia do jogo (versão USA/Europa) numa pasta. Ela precisa ter o `default.xex` e os `.dat`.
3. Abra o `PredatorGame.exe`, escolha a pasta do jogo em **Pasta do jogo** e clique em **Jogar**.

Requisitos: Windows 10/11 64 bits, 4 núcleos/8 threads, 8 GB de RAM e placa de vídeo com DirectX 12.
Mais detalhes (controles, perfis, editor de mapas, tela dividida) no `LEIA-ME.txt` do pacote.

## Compilar

Precisa do Visual Studio 2022 (C++), LLVM (clang-cl), CMake, Ninja e Python 3.

1. Compile o recompilador: `PredatorRecomp\build.ps1`
2. Ponha na pasta `game\` (na raiz) o `default.xex` da sua cópia e gere a imagem descriptografada com o
   `PredatorGame.exe` do pacote (Releases): `PredatorGame.exe --extrair-imagem game\default.xex game\image.bin`
3. Gere o C++ do jogo e compile: `PredatorRecomp\game\build_game.ps1`
   (para o editor de mapas: `ige.xex` e `image.bin` em `editor\`, e `build_game.ps1 -Editor`)
4. O pacote para distribuir sai com `tools\empacotar.ps1`.

O código gerado (`PredatorRecomp\out`) é do jogo e **nunca** deve ser publicado.

## Documentação

- `DIAGNOSTICO.md`: variáveis de ambiente e ferramentas de diagnóstico.

---

## English

PC port (Windows, DirectX 12) of *Far Cry Instincts Predator* (Xbox 360), made by **static recompilation**: the
game's PowerPC code is translated to C++ and runs natively, together with a custom runtime (kernel, files, video,
audio, input and network). The recompiler and the runtime were written from scratch.

> **Fan project, not affiliated with Ubisoft or Microsoft.**
> This repository and the release packages contain **no game files**. You need **your own original copy** of the
> Xbox 360 game. Do not ask for or post links to the game here.

### Play

1. Download the package from **Releases** and extract it to a writable folder (e.g. `Documents`).
2. Extract the disc of **your** copy (USA/Europe version) to a folder containing `default.xex` and the `.dat` files.
3. Run `PredatorGame.exe`, pick that folder under **Game folder** and click **Play**.

Requirements: Windows 10/11 64-bit, 4 cores/8 threads, 8 GB RAM, DirectX 12 GPU. See `README.txt` in the package.

### Build

Needs Visual Studio 2022 (C++), LLVM (clang-cl), CMake, Ninja and Python 3.

1. `PredatorRecomp\build.ps1`
2. Put your `default.xex` in `game\` and create the decrypted image with the release's `PredatorGame.exe`:
   `PredatorGame.exe --extrair-imagem game\default.xex game\image.bin`
3. `PredatorRecomp\game\build_game.ps1` (map editor: `ige.xex`/`image.bin` in `editor\`, `build_game.ps1 -Editor`)
4. `tools\empacotar.ps1` builds the release package.

The generated code (`PredatorRecomp\out`) belongs to the game and must **never** be published.

Diagnostic documentation (in Portuguese): `DIAGNOSTICO.md`.

## Agradecimentos especiais / Special thanks

- **[Xenia](https://xenia.jp)**: o emulador foi usado só como **referência de comparação** para entender e corrigir
  alguns bugs. **Nenhum código do Xenia está neste projeto.** / The emulator was used only as a **comparison
  reference** to understand and fix some bugs. **No Xenia code is included in this project.**
- **[Claude Code](https://claude.com/claude-code)** (Anthropic): ajudou a escrever a ferramenta **PredatorRecomp**
  (o recompilador) e o runtime. / Helped write the **PredatorRecomp** tool (the recompiler) and the runtime.

## Licença / License

Código do projeto sob a licença MIT (`LICENSE`). *Far Cry* é marca da Ubisoft; o jogo e seus arquivos pertencem aos
donos. / Project code under the MIT license. *Far Cry* is a Ubisoft trademark; the game and its files belong to
their owners.
