@echo off
rem Opens the game recording the "shader recipe" while you play.
rem Each time you open the game through this file, it writes a file to the "recipes" folder (next to this .bat)
rem with the shaders, pipelines and surfaces that showed up. There is no game data in these files: numbers only.
rem Play normally, go through different levels, and then send the "recipes" folder to the developer.
cd /d "%~dp0"
set FCRECOMP_COLETARRECEITA=1
start "" "%~dp0PredatorGame.exe"
