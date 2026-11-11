# Abre o jogo normalmente, com as medicoes de desempenho ligadas, para quem joga ir ate um lugar com queda de quadros.
# No lugar: aperte F3 (mede 10 s quem esta gastando tempo; pode apertar de novo em outro lugar) e depois F12 (pacote).
# Medicoes: FPS e tempos a cada 120 quadros, cada quadro acima de 50 ms, quadros.csv e o perfilador na tecla F3.
$d = "C:\Projeto Far Cry Recomp\PredatorRecomp\build_game"
$vars = @{ FCRECOMP_PERF = "1"; FCRECOMP_TRAVADA = "50"; FCRECOMP_QUADROLOG = "1"; FCRECOMP_PROFILE = "F3:10" }
foreach ($k in $vars.Keys) { Set-Item "Env:$k" $vars[$k] }
Start-Process "$d\PredatorGame.exe" -WorkingDirectory $d
foreach ($k in $vars.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
"jogo aberto com as medicoes ligadas: no lugar lento aperte F3, espere 10 s e aperte F12"
