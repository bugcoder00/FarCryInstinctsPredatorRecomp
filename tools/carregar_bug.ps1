# Volta ao ponto de um bug gravado com F12: copia o save do pacote build\bugs\bug_N para build\save_teste (os saves
# de quem joga, em build\save, nao sao tocados), abre o jogo, entra em INSTINCTS STORY > RESUME GAME e carrega o save.
# Uso: tools\carregar_bug.ps1 -Bug 3 (ou -Bug 0 = o save atual de build\save) [-Segundos 60] [-Prints 5,20] [-Extra "FCRECOMP_PERF=1;FCRECOMP_XMALOG=1"]
#   -Segundos: quanto tempo deixar o jogo rodando depois que a fase comecar (0 = deixa aberto e sai do roteiro)
#   -Prints:   segundos (depois do inicio da fase) para tirar prints da janela em build\bugs\bug_N\verificacao\
#   -Extra:    variaveis de ambiente para a execucao (ferramentas de diagnostico)
#   -Roteiro:  botoes depois que a fase comeca, com "F" (segundos desde o inicio da fase): "F3-8:LUP" anda para a frente
#              de 3 a 8 s, "F9-9.5:RRIGHT" vira a camera para a direita (veja FCRECOMP_AUTOINPUT em DIAGNOSTICO.md)
#   -Gravacao: pasta de tools\gravar_roteiro.ps1 (build_game\gravacoes\...): chama o repetir_roteiro.ps1
param([int]$Bug = -1, [int]$Segundos = 60, [string]$Prints = "", [string]$Extra = "", [string]$Roteiro = "", [string]$Gravacao = "")
# "5,20" vira a lista 5 e 20 (com ponto para fracoes: "2.5")
$listaPrints = @(($Prints -split '[,;\s]+') | Where-Object { $_ } | ForEach-Object { [double]::Parse($_, [Globalization.CultureInfo]::InvariantCulture) })
$ErrorActionPreference = "Stop"
$d = "C:\Projeto Far Cry Recomp\PredatorRecomp\build_game"
# -Bug 0 = o save atual de quem joga (copia de build\save; os prints vao para build\medicoes\verificacao)
if ($Gravacao) {
  # gravacoes desde a abertura do jogo (03/11/2026) sao repetidas pelo repetir_roteiro.ps1
  & (Join-Path $PSScriptRoot "repetir_roteiro.ps1") -Gravacao $Gravacao -Segundos $Segundos -Extra $Extra
  exit
}
elseif ($Bug -eq 0) { $pacote = "$d\medicoes"; $origem = "$d\save" }
elseif ($Bug -gt 0) { $pacote = "$d\bugs\bug_$Bug"; $origem = "$pacote\save" }
else { "use -Bug N ou -Gravacao pasta"; exit 1 }
if (-not (Test-Path "$origem\conteudo")) { "nao ha jogo salvo em $origem (o bug foi gravado jogando sem salvar?)"; exit 1 }
Get-Content "$pacote\info.txt" -ErrorAction SilentlyContinue

$teste = "$d\save_teste"
if (Test-Path $teste) { Remove-Item -Recurse -Force $teste }
Copy-Item -Recurse $origem $teste

Add-Type -AssemblyName System.Drawing
if (-not ("CarregarBug" -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public class CarregarBug {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@ }
[CarregarBug]::SetProcessDPIAware() | Out-Null

# Botoes apertados pelo proprio emulador (nao depende da janela estar na frente). START a cada 2 s pula os videos e
# a tela "PRESS START"; os itens "M" contam a partir do menu principal: A = INSTINCTS STORY, 2x baixo = RESUME GAME,
# A = lista de saves, A = carrega o 1o (o unico em save_teste).
# (o PowerShell nao diferencia maiusculas: uma variavel "$roteiro" apagaria o parametro -Roteiro)
$botoes = "3-60/2:START,M6:A,M9:DOWN,M10.5:DOWN,M12.5:A,M16.5:A"
if ($Roteiro) { $botoes += "," + $Roteiro }
# inicio da fase: 1o quadro com mais de 300 desenhos depois que o save foi aberto (telas de carregamento tem < 200)
$vars = @{ FCRECOMP_SAVEDIR = $teste; FCRECOMP_AUTOINPUT = $botoes; FCRECOMP_MISSAO = ":999999999"; FCRECOMP_SAVELOG = "1";
           FCRECOMP_MISSAOAPOSSAVE = "1"; FCRECOMP_MISSAOMIN = "300" }
foreach ($kv in ($Extra -split ';')) { if ($kv -match '=') { $k, $v = $kv -split '=', 2; $vars[$k] = $v } }
foreach ($k in $vars.Keys) { Set-Item "Env:$k" $vars[$k] }

$log = "$d\fcrecomp_log.txt"
function Esperar([string]$padrao, [int]$limite) {
  for ($i = 0; $i -lt $limite * 5; $i++) {
    if ($p.HasExited) { return $false }
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern $padrao -Quiet)) { return $true }
    Start-Sleep -Milliseconds 200
  }
  return $false
}
function Print([string]$arquivo) {
  $p.Refresh(); [CarregarBug]::SetForegroundWindow($p.MainWindowHandle) | Out-Null; Start-Sleep -Milliseconds 150
  $r = New-Object CarregarBug+RECT; [CarregarBug]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
  $bm = New-Object Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T); $g = [Drawing.Graphics]::FromImage($bm)
  $g.CopyFromScreen($r.L, $r.T, 0, 0, $bm.Size); $bm.Save($arquivo); $g.Dispose(); $bm.Dispose()
}

Remove-Item $log -ErrorAction SilentlyContinue   # senao o roteiro acharia os sinais no log da execucao anterior
$p = Start-Process "$d\PredatorGame.exe" -WorkingDirectory $d -PassThru
try {
  if (-not (Esperar "menu principal aos" 150)) { throw "o menu principal nao apareceu" }
  "menu principal; entrando em RESUME GAME..."
  if (-not (Esperar "XamContentCreate .*aberto" 40)) { throw "o save nao foi aberto (menu em outro ponto?)" }
  "save aberto; carregando a fase..."
  if (-not (Esperar "missao comecou" 240)) { throw "a fase nao comecou" }
  $t0 = Get-Date
  "fase comecou"
  if ($listaPrints.Count) { New-Item -ItemType Directory -Force "$pacote\verificacao" | Out-Null }
  foreach ($s2 in ($listaPrints | Sort-Object)) {
    while (((Get-Date) - $t0).TotalSeconds -lt $s2) { Start-Sleep -Milliseconds 100 }
    Print "$pacote\verificacao\print_$s2.png"
    "print: $pacote\verificacao\print_$s2.png"
  }
  if ($Segundos -le 0) { "jogo deixado aberto"; return }
  while (((Get-Date) - $t0).TotalSeconds -lt $Segundos -and -not $p.HasExited) { Start-Sleep -Milliseconds 200 }
}
catch { "FALHOU: $_" }
finally {
  if ($Segundos -gt 0 -and -not $p.HasExited) {
    try { $ev = [System.Threading.EventWaitHandle]::OpenExisting('FCRecomp_Sair'); $ev.Set() | Out-Null; $ev.Dispose() } catch {}
    if (-not $p.WaitForExit(10000)) { $p.Kill() }
  }
  foreach ($k in $vars.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
}
