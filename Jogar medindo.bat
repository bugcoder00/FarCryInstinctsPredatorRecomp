@echo off
rem Abre o jogo com as medicoes de desempenho ligadas (dois cliques neste arquivo).
rem No lugar lento: aperte F3 (o titulo da janela mostra "MEDINDO..."), espere acabar e aperte F12.
set FCRECOMP_PERF=1
set FCRECOMP_TRAVADA=50
set FCRECOMP_QUADROLOG=1
set FCRECOMP_PROFILE=F3:10
cd /d "C:\Projeto Far Cry Recomp\PredatorRecomp\build_game"
start "" PredatorGame.exe
