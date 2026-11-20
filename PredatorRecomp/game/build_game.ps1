# Gera o C do jogo com o PredatorRecomp e compila o PredatorGame (codigo gerado + runtime).
# Uso: .\game\build_game.ps1 [-SkipGenerate] [-Editor]
#   -Editor: o editor de mapas (ige.xex, imagem em <projeto>\editor) -> build_editor\PredatorEditor.exe, copiado para
#            build_game ao lado do PredatorGame.exe (o menu MAP EDITOR do jogo abre ele)
# Um executavel so para portugues e ingles: o idioma e escolhido no launcher (na primeira vez, o do Windows).
#   -Ingles: nao precisa mais (aceito so para scripts antigos; compila o mesmo de sempre)
param([switch]$SkipGenerate, [switch]$Editor, [switch]$Ingles)
$root = Split-Path $PSScriptRoot -Parent
$project = Split-Path $root -Parent
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User') + ";C:\Program Files\LLVM\bin"
$vc = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if ($Editor) { $xex = "$project\editor\ige.xex"; $img = "$project\editor\image.bin"; $out = "$root\out_editor"; $dir = "build_editor"; $ed = "ON" }
else { $xex = "$project\game\default.xex"; $img = "$project\game\image.bin"; $out = "$root\out"; $dir = "build_game"; $ed = "OFF" }
$gen = "$out\cpu"
$jogo = "build_game"
if ($Ingles) { Write-Host "-Ingles nao precisa mais: o mesmo executavel tem portugues e ingles (escolha no launcher)" }

if (-not $SkipGenerate) {
    & "$root\build\predatorrecomp.exe" cpu $xex --image $img --out $out --force
    if ($LASTEXITCODE -ne 0) { Write-Host "falha ao gerar o C"; exit 1 }
}

cmd /c "`"$vc`" >nul && cd /d `"$root`" && cmake -S game -B $dir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DPREDATOR_EDITOR=$ed `"-DPREDATOR_GENERATED=$gen`" && cmake --build $dir && echo GAME_OK"
if ($Editor -and (Test-Path "$root\$dir\PredatorEditor.exe")) {
    Copy-Item "$root\$dir\PredatorEditor.exe", "$root\$dir\image_editor.bin" "$root\$jogo\" -Force
    Copy-Item "$root\$dir\FCRecomp.map" "$root\$jogo\PredatorEditor.map" -Force
    "PredatorEditor.exe copiado para $jogo"
}
