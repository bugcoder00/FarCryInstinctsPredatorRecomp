# Abre o jogo gravando tudo o que quem joga faz com o controle, teclado e mouse DESDE A ABERTURA (videos, menus,
# carregamentos, a fase e o editor de mapas, inclusive depois das trocas de programa jogo <-> editor), para o Claude
# repetir depois exatamente o mesmo caminho (tools\repetir_roteiro.ps1 -Gravacao <pasta>).
# Antes de abrir, copia os saves atuais (jogos salvos e mapas do editor) para a pasta da gravacao: a repeticao comeca
# com os mesmos saves, mesmo que o jogo salve algo novo durante a gravacao.
# Uso: dois cliques em "Gravar roteiro.bat" (pasta principal) ou: tools\gravar_roteiro.ps1 [-Nome agua]
#   -Editor                abre direto o editor de mapas (PredatorEditor.exe; "Gravar roteiro no editor.bat"), sem o
#                          jogo e o menu antes: mais rapido para gravar e para repetir
#   -Extra "VAR=valor;..." variaveis de diagnostico para a execucao (ex.: FCRECOMP_ROTEIROFOTOS)
#   -SemEspera             nao espera Enter no fim (testes)
param([string]$Nome = "", [string]$Extra = "", [switch]$SemEspera, [switch]$Editor)
$programa = if ($Editor) { "PredatorEditor.exe" } else { "PredatorGame.exe" }
$d = "C:\Projeto Far Cry Recomp\PredatorRecomp\build_game"
if (Get-Process PredatorGame, PredatorEditor -ErrorAction SilentlyContinue) {
    "O jogo ja esta aberto. Feche o jogo e abra este atalho de novo."
    if (-not $SemEspera) { Read-Host "Aperte Enter para fechar esta janela" }
    exit 1
}
$stamp = Get-Date -Format "yyyy-MM-dd_HH-mm"
$pasta = "$d\gravacoes\$stamp$(if ($Nome) { "_$Nome" })"
New-Item -ItemType Directory -Force $pasta | Out-Null
if (Test-Path "$d\save") { Copy-Item -Recurse "$d\save" "$pasta\save" }
$vars = @{ FCRECOMP_GRAVAR = "$pasta\controle.txt" }
foreach ($kv in ($Extra -split ';')) { if ($kv -match '=') { $k, $v = $kv -split '=', 2; $vars[$k] = $v } }
foreach ($k in $vars.Keys) { Set-Item "Env:$k" $vars[$k] }
"Gravando em: $pasta"
""
if ($Editor) { "A gravacao comeca agora, junto com o editor de mapas (um bipe). Carregue o seu mapa e jogue o trecho." }
else { "A gravacao comeca agora, junto com o jogo (um bipe). Faca tudo normalmente: pule os videos, use os menus,"
       "entre no editor de mapas, carregue o seu mapa e jogue o trecho." }
"Para terminar: aperte F4 (dois bipes) e depois feche o jogo. Fechar o jogo direto tambem termina a gravacao."
Remove-Item "$d\fcrecomp_log_antes_reinicio.txt" -ErrorAction SilentlyContinue
$p = Start-Process "$d\$programa" -WorkingDirectory $d -PassThru
foreach ($k in $vars.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
# o jogo se reabre nas trocas (editor de mapas, troca de fase): espera ate nao sobrar nenhum
$p.WaitForExit()
while (Get-Process PredatorGame, PredatorEditor -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 500 }
if (Test-Path "$pasta\controle.txt") {
    Copy-Item "$d\fcrecomp_log.txt" "$pasta\fcrecomp_log.txt" -ErrorAction SilentlyContinue
    Copy-Item "$d\fcrecomp_log_antes_reinicio.txt" "$pasta\fcrecomp_log_antes_reinicio.txt" -ErrorAction SilentlyContinue
    $linhas = Get-Content "$pasta\controle.txt"
    $etapas = @($linhas | Where-Object { $_ -like "ETAPA *" }).Count
    $tiques = 0
    foreach ($l in $linhas) { if ($l -match '^ETAPA ') { $base = $tiques } elseif ($l -match '^\w+ (\d+)') { $tiques = [math]::Max($tiques, $base + [int]$Matches[1]) } }
    "Gravacao pronta: $etapas programa(s) (jogo/editor), cerca de $([int]($tiques / 30)) segundos de jogo, em $pasta"
} else { "Nada foi gravado (o jogo nao chegou a abrir?)." }
if (-not $SemEspera) { Read-Host "Aperte Enter para fechar esta janela" }
