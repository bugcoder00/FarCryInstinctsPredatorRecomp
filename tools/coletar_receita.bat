@echo off
rem Abre o jogo anotando a "receita dos shaders" enquanto voce joga (item 39).
rem Cada vez que abrir por aqui, o jogo grava um arquivo na pasta "receitas" (ao lado deste .bat) com os shaders,
rem pipelines e superficies que apareceram. Nao tem nada do jogo nesses arquivos: so numeros.
rem Jogue normalmente, passe por fases diferentes, e depois mande a pasta "receitas".
cd /d "%~dp0"
set FCRECOMP_COLETARRECEITA=1
start "" "%~dp0PredatorGame.exe"
