# Mede o desempenho na missao: entra sozinho, roda N quadros depois do inicio da missao e mostra a media
# do tempo de GPU por quadro (perfilador FCRECOMP_GPUPROF) e dos quadros por segundo (FCRECOMP_PERF).
# Uso: tools\medir_gpu.ps1 [-Quadros 1500] [-Vezes 1] [-SemPerfilador]  (sem o perfilador o FPS e o real)
param([int]$Quadros = 1500, [int]$Vezes = 1, [switch]$SemPerfilador)
$d = "C:\Projeto Far Cry Recomp\FCRecomp\build"
for ($i = 1; $i -le $Vezes; $i++)
{
    $env:FCRECOMP_AUTOINPUT = "3-15/2:START,19-50/3:A"
    $env:FCRECOMP_MISSAO = ":$Quadros"
    if (-not $SemPerfilador) { $env:FCRECOMP_GPUPROF = "1" }
    $env:FCRECOMP_PERF = "1"
    $p = Start-Process "$d\FCRecomp.exe" -WorkingDirectory $d -PassThru
    if (-not $p.WaitForExit(600000)) { $p.Kill(); "tempo esgotado" }
    Remove-Item Env:FCRECOMP_AUTOINPUT, Env:FCRECOMP_MISSAO, Env:FCRECOMP_GPUPROF, Env:FCRECOMP_PERF -ErrorAction SilentlyContinue
    $log = Get-Content "$d\fcrecomp_log.txt"
    $start = ($log | Select-String "missao comecou" | Select-Object -First 1).LineNumber
    if (-not $start) { "execucao ${i}: a missao nao comecou"; continue }
    $after = $log[$start..($log.Count - 1)]
    $gpu = $after | Select-String "GPU real: ([\d.]+) ms" | Select-Object -Skip 1 | ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
    # pula a primeira medicao de qps (carregamento dos shaders)
    $qps = $after | Select-String "\[perf\] ([\d.]+) qps" | Select-Object -Skip 1 | ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
    "execucao {0}: GPU media {1:N1} ms/quadro ({2} medicoes) | {3:N1} quadros/s em media ({4} medicoes)" -f $i,
        ($gpu | Measure-Object -Average).Average, $gpu.Count, ($qps | Measure-Object -Average).Average, $qps.Count
}

