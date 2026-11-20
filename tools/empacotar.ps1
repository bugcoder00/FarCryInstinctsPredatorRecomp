# Monta o pacote para distribuir: so o executavel, as DLLs que ele precisa e o LEIA-ME/README. Nenhum arquivo do jogo
# (o image.bin e criado pelo proprio jogo a partir do default.xex da copia de quem joga) e nenhum codigo-fonte.
# Uso: powershell -ExecutionPolicy Bypass -File tools\empacotar.ps1   ->   dist\FarCryPredatorRecomp + zip com a versao
# Um pacote so para portugues e ingles: o idioma e escolhido no launcher (na primeira vez, o do Windows). Vao os dois
# textos (LEIA-ME.txt e README.txt) e os dois .bat da receita (coletar_receita.bat e collect_recipe.bat).
#      -Ingles: nao precisa mais (aceito so para scripts antigos; monta o mesmo pacote)
param([switch]$Ingles)
$ErrorActionPreference = "Stop"
$raiz = "C:\Projeto Far Cry Recomp"
$build = "$raiz\PredatorRecomp\build_game"
$destino = "$raiz\dist\FarCryPredatorRecomp"
$textos = "LEIA-ME.txt", "README.txt"
if ($Ingles) { Write-Host "-Ingles nao precisa mais: o pacote tem portugues e ingles" }

if (Test-Path $destino) { Remove-Item -Recurse -Force $destino }
New-Item -ItemType Directory -Force $destino | Out-Null

# PredatorEditor.exe = o editor de mapas (menu MAP EDITOR; build_game.ps1 -Editor), cria o image_editor.bin do ige.xex
foreach ($arq in "PredatorGame.exe", "PredatorEditor.exe", "dxcompiler.dll", "dxil.dll") {
    if (-not (Test-Path "$build\$arq")) { throw "falta $build\$arq (compile com PredatorRecomp\game\build_game.ps1)" }
    Copy-Item "$build\$arq" $destino
}
foreach ($t in $textos) { Copy-Item "$raiz\tools\$t" $destino }
# receita dos shaders (item 38; tools\gerar_receita.py): so numeros (molde do shaders.dat + palavras preenchidas,
# pipelines e superficies); o jogo monta os shaders com o shaders.dat de quem joga. O jogo procura receita_shaders.txt
# (e shader_recipe.txt, o nome do antigo pacote em ingles)
Copy-Item "$raiz\tools\receita_shaders.txt" $destino
# item 39: abre o jogo anotando a receita na pasta receitas/recipes (para quem quiser mandar)
Copy-Item "$raiz\tools\coletar_receita.bat", "$raiz\tools\collect_recipe.bat" $destino

# confere que nada do jogo nem do desenvolvimento entrou
$proibidos = Get-ChildItem $destino -Recurse | Where-Object { $_.Name -match '\.(bin|xex|dat|ini|map|lib|pdb|txt)$' -and $textos -notcontains $_.Name -and $_.Name -ne 'receita_shaders.txt' }
if ($proibidos) { throw "arquivo que nao deveria ir no pacote: $($proibidos.Name -join ', ')" }

# nome do zip com a versao (a do launcher.cpp, VERSAO_RECOMP): FarCryPredatorRecompLauncher_0.1.5.zip
$versao = [regex]::Match((Get-Content -Raw "$raiz\PredatorRecomp\runtime\launcher.cpp"), 'VERSAO_RECOMP L"([0-9.]+)"').Groups[1].Value
if (-not $versao) { throw "versao nao achada em launcher.cpp (VERSAO_RECOMP)" }
$zip = "$raiz\dist\FarCryPredatorRecompLauncher_${versao}.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $destino -DestinationPath $zip
Get-ChildItem $destino | ForEach-Object { "{0,-22} {1,8:N1} MB" -f $_.Name, ($_.Length / 1MB) }
"pacote: $zip ({0:N1} MB)" -f ((Get-Item $zip).Length / 1MB)
