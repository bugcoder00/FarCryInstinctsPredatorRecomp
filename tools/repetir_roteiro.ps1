# Repete uma gravacao de tools\gravar_roteiro.ps1 ("Gravar roteiro.bat") desde a abertura do jogo: usa os saves
# copiados na pasta da gravacao (em build_game\save_teste; os saves de quem joga nao sao tocados) e o controle, o
# mouse e a camera gravados, sincronizados pelo relogio do roteiro (veja runtime\input.cpp).
# Uso: tools\repetir_roteiro.ps1 -Gravacao <pasta ou nome em build_game\gravacoes> [-Fotos "0:300,1:900"] [-Segundos 3]
#   -Fotos:    etapa:tique (etapa 0 = o jogo aberto, 1 = o programa seguinte...; tique = quadro do roteiro) para gravar
#              imagens em <gravacao>\repeticao (compare com uma gravacao feita com FCRECOMP_ROTEIROFOTOS iguais)
#   -Segundos: quanto esperar depois do fim da gravacao antes de fechar o jogo (0 = deixa aberto)
#   -Ate:      tique onde a repeticao termina antes do fim da gravacao (use com -Segundos 0 para investigar ali)
#   -Extra:    variaveis de diagnostico ("FCRECOMP_XMALOG=2;FCRECOMP_VOZESLOG=1")
param([Parameter(Mandatory)][string]$Gravacao, [string]$Fotos = "", [int]$Segundos = 3, [string]$Extra = "", [int]$Ate = 0)
$d = "C:\Projeto Far Cry Recomp\PredatorRecomp\build_game"
if (-not (Test-Path $Gravacao)) { $Gravacao = "$d\gravacoes\$Gravacao" }
if (-not (Test-Path "$Gravacao\controle.txt")) { "nao achei $Gravacao\controle.txt"; exit 1 }
if ((Get-Content "$Gravacao\controle.txt" -TotalCount 1) -ne "ROTEIRO 2") { "gravacao no formato antigo (so a partir da fase): grave de novo"; exit 1 }
if (Get-Process PredatorGame, PredatorEditor -ErrorAction SilentlyContinue) { "o jogo esta aberto (alguem jogando?)"; exit 1 }
$saida = "$Gravacao\repeticao"
New-Item -ItemType Directory -Force $saida | Out-Null
$teste = "$d\save_teste"
if (Test-Path $teste) { Remove-Item -Recurse -Force $teste }
if (Test-Path "$Gravacao\save") { Copy-Item -Recurse "$Gravacao\save" $teste } else { New-Item -ItemType Directory $teste | Out-Null }

$vars = @{ FCRECOMP_SAVEDIR = $teste; FCRECOMP_REPRODUZIR = "$Gravacao\controle.txt"; FCRECOMP_ROTEIROFOTOSDIR = $saida }
if ($Fotos) { $vars["FCRECOMP_ROTEIROFOTOS"] = $Fotos }
if ($Ate -gt 0) { $vars["FCRECOMP_ROTEIROATE"] = "$Ate" }
foreach ($kv in ($Extra -split ';')) { if ($kv -match '=') { $k, $v = $kv -split '=', 2; $vars[$k] = $v } }
foreach ($k in $vars.Keys) { Set-Item "Env:$k" $vars[$k] }
$log = "$d\fcrecomp_log.txt"
Remove-Item $log, "$d\fcrecomp_log_antes_reinicio.txt" -ErrorAction SilentlyContinue
# o mesmo programa que abriu a gravacao (o jogo, ou o editor de mapas aberto direto)
$programa = "PredatorGame.exe"
if ((Get-Content "$Gravacao\controle.txt" -TotalCount 2)[1] -match '^ETAPA 0 (\S+)') { $programa = $Matches[1] }
$p = Start-Process "$d\$programa" -WorkingDirectory $d -PassThru
foreach ($k in $vars.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
"repetindo $Gravacao ($programa) ..."
# cada programa (jogo, editor) escreve o seu registro; o fim aparece no registro do ultimo
$fim = $false
while (Get-Process PredatorGame, PredatorEditor -ErrorAction SilentlyContinue) {
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern "\[roteiro\] fim da gravacao" -Quiet)) { $fim = $true; break }
    Start-Sleep -Milliseconds 500
}
if ($fim) { (Select-String -Path $log -Pattern "\[roteiro\] fim da gravacao").Line }
else { "o jogo fechou antes do fim da gravacao" }
if ($Segundos -gt 0 -and $fim) {
    Start-Sleep $Segundos
    try { $ev = [System.Threading.EventWaitHandle]::OpenExisting('FCRecomp_Sair'); $ev.Set() | Out-Null; $ev.Dispose() } catch {}
    for ($i = 0; $i -lt 20 -and (Get-Process PredatorGame, PredatorEditor -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
    Get-Process PredatorGame, PredatorEditor -ErrorAction SilentlyContinue | Stop-Process -Force
}
Copy-Item $log "$saida\fcrecomp_log.txt" -ErrorAction SilentlyContinue
Copy-Item "$d\fcrecomp_log_antes_reinicio.txt" "$saida\fcrecomp_log_antes_reinicio.txt" -ErrorAction SilentlyContinue
"registros e fotos em $saida"
