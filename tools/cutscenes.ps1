# Ver uma cutscene sem jogar ate ela (runtime\cena_direta.cpp). Dois tipos:
#   - videos Bink da pasta videos do jogo (abertura, intro, menus): o primeiro video que o jogo abre vira o escolhido
#     (FCRECOMP_VIDEO); depois o jogo segue para o menu;
#   - cenas feitas no motor (fases da historia, lista em tools\cutscenes.txt, gerada por tools\gerar_cutscenes.py):
#     o jogo aperta START sozinho, abre a fase sem passar pelos menus e dispara o gatilho da cena (FCRECOMP_CENA).
# Os saves ficam numa pasta separada (build_game\save_cena, apagada a cada vez): os saves de quem joga nao sao tocados.
#
# Uso:
#   tools\cutscenes.ps1                         lista os videos (V1, V2...) e as cenas (1, 2...)
#   tools\cutscenes.ps1 -Cena 4                 abre a cena 4 da lista e deixa o jogo aberto
#   tools\cutscenes.ps1 -Cena "21_dunea:ProximityTrigger44"   fase[:gatilho[:evento]] direto
#   tools\cutscenes.ps1 -Video V2               (ou -Video Intro_FCI.bik) toca esse video no lugar do da Ubisoft
#   -Fotos "1,10,20": segundos depois de a cena comecar para salvar a imagem do jogo em build_game\cutscenes
#   -Rajadas "2,8":   segundos para gravar 60 quadros seguidos (ao_vivo RAJADA=1; em build_game\diag\rajada_N), para o que pisca
#   -Segundos N:      fecha o jogo N segundos depois de a cena comecar (0 = deixa aberto; com -Fotos, padrao = ultima foto + 3)
#   -Espera ms:       quanto esperar na fase antes de disparar o gatilho (padrao 4000)
#   -NaoLevar:        nao leva o jogador ate o gatilho (so dispara)
#   -Extra:           outras variaveis ("FCRECOMP_PULARCENA_AUTO=5")
param([string]$Cena = "", [string]$Video = "", [string]$Fotos = "", [string]$Rajadas = "", [int]$Segundos = -1, [int]$Espera = 0,
      [switch]$NaoLevar, [string]$Extra = "")
$ErrorActionPreference = "Stop"
$d = "C:\Projeto Far Cry Recomp\PredatorRecomp\build_game"

# pasta do jogo (opcoes.ini) e as duas listas
$pastaJogo = ""
foreach ($l in Get-Content "$d\opcoes.ini") { if ($l -match '^\s*pasta_jogo\s*=\s*(.+?)\s*$') { $pastaJogo = $Matches[1] } }
$videos = @()
if ($pastaJogo -and (Test-Path "$pastaJogo\videos")) { $videos = @(Get-ChildItem "$pastaJogo\videos\*.bik" | ForEach-Object { $_.Name }) }
$cenas = @(Get-Content (Join-Path $PSScriptRoot "cutscenes.txt") | Where-Object { $_ -and -not $_.StartsWith("#") } | ForEach-Object {
  $c = $_ -split '\|'; [pscustomobject]@{ Fase = $c[0]; Gatilho = $c[1]; Evento = $c[2]; Ancora = $c[3] } })

if (-not $Cena -and -not $Video) {
  "VIDEOS (pasta videos do jogo):"
  for ($i = 0; $i -lt $videos.Count; $i++) { "  V{0,-3} {1}" -f ($i + 1), $videos[$i] }
  ""
  "CENAS DO MOTOR (fase / gatilho / ancora da cena; sem gatilho = comeca com a fase):"
  for ($i = 0; $i -lt $cenas.Count; $i++) {
    $c = $cenas[$i]
    $g = if ($c.Gatilho) { $c.Gatilho + $(if ($c.Evento -and $c.Evento -ne "Enter") { " (" + $c.Evento + ")" }) } else { "-" }
    "  {0,-4} {1,-24} {2,-30} {3}" -f ($i + 1), $c.Fase, $g, $c.Ancora
  }
  ""
  "Ex.: tools\cutscenes.ps1 -Cena 21   |   tools\cutscenes.ps1 -Video V2   |   -Fotos ""1,10"" -Segundos 15"
  exit 0
}
if (Get-Process PredatorGame, PredatorEditor -ErrorAction SilentlyContinue) { "o jogo esta aberto (alguem jogando?): feche antes"; exit 1 }

$nome = ""
if ($Video) {
  if ($Video -match '^[Vv]?(\d+)$') {
    $n = [int]$Matches[1]
    if ($n -lt 1 -or $n -gt $videos.Count) { "nao ha o video $Video (veja a lista sem parametros)"; exit 1 }
    $Video = $videos[$n - 1]
  }
  if ($Video -notmatch '\.bik$') { $Video += ".bik" }
  $env:FCRECOMP_VIDEO = $Video
  $nome = [IO.Path]::GetFileNameWithoutExtension($Video)
}
if ($Cena) {
  if ($Cena -match '^\d+$') {
    $n = [int]$Cena
    if ($n -lt 1 -or $n -gt $cenas.Count) { "nao ha a cena $Cena (veja a lista sem parametros)"; exit 1 }
    $c = $cenas[$n - 1]
    $Cena = $c.Fase
    if ($c.Gatilho) { $Cena += ":" + $c.Gatilho; if ($c.Evento -and $c.Evento -ne "Enter") { $Cena += ":" + $c.Evento } }
    "cena {0}: {1} (ancora {2})" -f $n, $Cena, $c.Ancora
  }
  if ($Cena -notmatch '^[\w.\-]+(:[\w.\-]*){0,2}$') { "use fase[:gatilho[:evento]] (letras, numeros, - _ .)"; exit 1 }
  $env:FCRECOMP_CENA = $Cena
  if ($Espera -gt 0) { $env:FCRECOMP_CENA_ESPERA = "$Espera" }
  if ($NaoLevar) { $env:FCRECOMP_CENA_LEVAR = "0" }
  $nome = $Cena -replace ':', '_'
}
foreach ($kv in ($Extra -split ';')) { if ($kv -match '=') { $k, $v = $kv -split '=', 2; Set-Item "Env:$k" $v } }

$sd = "$d\save_cena"
if (Test-Path $sd) { Remove-Item -Recurse -Force $sd }
New-Item -ItemType Directory $sd | Out-Null
$env:FCRECOMP_SAVEDIR = $sd

$listaFotos = @(($Fotos -split '[,;\s]+') | Where-Object { $_ } | ForEach-Object { [double]::Parse($_, [Globalization.CultureInfo]::InvariantCulture) })
$listaRajadas = @(($Rajadas -split '[,;\s]+') | Where-Object { $_ } | ForEach-Object { [double]::Parse($_, [Globalization.CultureInfo]::InvariantCulture) })
$ultimo = @($listaFotos) + @($listaRajadas) | Sort-Object | Select-Object -Last 1
if ($Segundos -lt 0) { $Segundos = if ($null -ne $ultimo) { [int]($ultimo + 3) } else { 0 } }
$saida = "$d\cutscenes"
if ($listaFotos.Count) { New-Item -ItemType Directory -Force $saida | Out-Null }

$log = "$d\fcrecomp_log.txt"
$aoVivo = "$d\ao_vivo.txt"
Remove-Item $log -ErrorAction SilentlyContinue
$p = Start-Process "$d\PredatorGame.exe" -WorkingDirectory $d -PassThru
"jogo aberto (processo $($p.Id)); esperando a cena..."
$t0 = Get-Date; $tCena = $null; $k = 0; $kr = 0; $visto = 0
try {
  while (-not $p.HasExited) {
    Start-Sleep -Milliseconds 300
    if (-not (Test-Path $log)) { continue }
    $linhas = @(Get-Content $log -ErrorAction SilentlyContinue)
    for (; $visto -lt $linhas.Count; $visto++) {
      $l = $linhas[$visto]
      if ($l -match '^\[cena\]|^\[CRASH\]') { "  " + $l }
      if (-not $tCena -and ($l -match '^\[cena\] .*(disparado|sem gatilho|ja esta tocando)' -or ($Video -and -not $Cena -and $l -match '^\[cena\] video'))) {
        $tCena = Get-Date
        "a cena comecou ({0:N0} s depois de abrir o jogo)" -f ($tCena - $t0).TotalSeconds
      }
    }
    if (-not $tCena) {
      if (($Cena -and ((Get-Date) - $t0).TotalSeconds -gt 240) -or ($Video -and ((Get-Date) - $t0).TotalSeconds -gt 60)) { "a cena nao comecou (veja $log)"; break }
      continue
    }
    $passou = ((Get-Date) - $tCena).TotalSeconds
    if ($k -lt $listaFotos.Count -and $passou -ge $listaFotos[$k]) {
      $arq = "$saida\{0}_{1}s.bmp" -f $nome, $listaFotos[$k]
      "FOTO=$arq" | Set-Content $aoVivo   # o jogo rele o ao_vivo.txt quando ele muda (input.cpp)
      "  foto: $arq"
      $k++
    }
    if ($kr -lt $listaRajadas.Count -and $passou -ge $listaRajadas[$kr]) {
      "RAJADA=1" | Set-Content $aoVivo
      "  rajada aos {0} s" -f $listaRajadas[$kr]
      $kr++
    }
    if ($Segundos -gt 0 -and $passou -ge $Segundos) { break }
    if ($Segundos -eq 0 -and $k -ge $listaFotos.Count) { "o jogo fica aberto"; exit 0 }
  }
}
finally { if ($listaFotos.Count + $listaRajadas.Count) { Start-Sleep -Milliseconds 800; Remove-Item $aoVivo -ErrorAction SilentlyContinue } }
if (-not $p.HasExited) {
  try { [System.Threading.EventWaitHandle]::OpenExisting("FCRecomp_Sair").Set() | Out-Null } catch { Stop-Process -Id $p.Id -Force }
  $p.WaitForExit(15000) | Out-Null
}
if ($p.HasExited -and $p.ExitCode -ne 0) { "o jogo saiu com o codigo $($p.ExitCode) (veja $log)" }
