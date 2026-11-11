# Monta o pacote para distribuir: so o executavel, as DLLs que ele precisa e o LEIA-ME. Nenhum arquivo do jogo
# (o image.bin e criado pelo proprio jogo a partir do default.xex da copia de quem joga) e nenhum codigo-fonte.
# Uso: powershell -ExecutionPolicy Bypass -File tools\empacotar.ps1   ->   dist\FarCryPredatorRecomp + .zip
#      -Ingles: a versao em ingles (build_game_en, compilada com build_game.ps1 -Ingles e -Editor -Ingles)
#               -> dist\FarCryPredatorRecomp_EN + .zip, com o README.txt em ingles no lugar do LEIA-ME
param([switch]$Ingles)
$ErrorActionPreference = "Stop"
$raiz = "C:\Projeto Far Cry Recomp"
$build = "$raiz\PredatorRecomp\build_game"
$destino = "$raiz\dist\FarCryPredatorRecomp"
$leia = "LEIA-ME.txt"
if ($Ingles) { $build = "$raiz\PredatorRecomp\build_game_en"; $destino = "$raiz\dist\FarCryPredatorRecomp_EN"; $leia = "README.txt" }

if (Test-Path $destino) { Remove-Item -Recurse -Force $destino }
New-Item -ItemType Directory -Force $destino | Out-Null

# PredatorEditor.exe = o editor de mapas (menu MAP EDITOR; build_game.ps1 -Editor), cria o image_editor.bin do ige.xex
foreach ($arq in "PredatorGame.exe", "PredatorEditor.exe", "dxcompiler.dll", "dxil.dll") {
    if (-not (Test-Path "$build\$arq")) { throw "falta $build\$arq (compile com PredatorRecomp\game\build_game.ps1)" }
    Copy-Item "$build\$arq" $destino
}
Copy-Item "$raiz\tools\$leia" $destino
# receita dos shaders (item 38; tools\gerar_receita.py): so numeros (molde do shaders.dat + palavras preenchidas,
# pipelines e superficies); o jogo monta os shaders com o shaders.dat de quem joga
if ($Ingles) {
    # versao em ingles: a receita se chama shader_recipe.txt (com o comentario do cabecalho em ingles)
    $rec = Get-Content "$raiz\tools\receita_shaders.txt"
    $rec[0] = "RECIPE 1"   # a primeira linha so identifica o arquivo (o jogo e o gerar_receita.py nao conferem)
    $rec[1] = "# shader recipe: shaders.dat templates + filled-in words, pipelines and surfaces (numbers only, no game data)"
    [IO.File]::WriteAllLines("$destino\shader_recipe.txt", $rec)
} else { Copy-Item "$raiz\tools\receita_shaders.txt" $destino }
# item 39: abre o jogo anotando a receita na pasta receitas (para quem quiser mandar)
if ($Ingles) { Copy-Item "$raiz\tools\collect_recipe.bat" $destino } else { Copy-Item "$raiz\tools\coletar_receita.bat" $destino }

# confere que nada do jogo nem do desenvolvimento entrou
$proibidos = Get-ChildItem $destino -Recurse | Where-Object { $_.Name -match '\.(bin|xex|dat|ini|map|lib|pdb|txt)$' -and $_.Name -ne $leia -and $_.Name -ne 'receita_shaders.txt' -and $_.Name -ne 'shader_recipe.txt' }
if ($proibidos) { throw "arquivo que nao deveria ir no pacote: $($proibidos.Name -join ', ')" }

$zip = "$destino.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $destino -DestinationPath $zip
Get-ChildItem $destino | ForEach-Object { "{0,-20} {1,8:N1} MB" -f $_.Name, ($_.Length / 1MB) }
"pacote: $zip ({0:N1} MB)" -f ((Get-Item $zip).Length / 1MB)
