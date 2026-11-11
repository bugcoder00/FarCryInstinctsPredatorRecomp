# Entra na missao rapido (pula video e telas com START, depois confirma os menus com A) e tira um print.
# Uso: tools\missao.ps1 [-Seconds 110] [-Out arquivo.png]
param([int]$Seconds = 110, [string]$Out = "C:\Projeto Far Cry Recomp\FCRecomp\build\missao.png")
$env:FCRECOMP_AUTOINPUT = "3-15/2:START,19-50/3:A"
& "$PSScriptRoot\screenshot.ps1" -Seconds $Seconds -Out $Out
Remove-Item Env:FCRECOMP_AUTOINPUT
