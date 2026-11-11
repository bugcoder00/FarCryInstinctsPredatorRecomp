# Compila o PredatorRecomp (Ninja + clang-cl do Visual Studio) e roda os testes. Uso: .\build.ps1 [-Config Debug]
param([string]$Config = "Release")
$root = $PSScriptRoot
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User') + ";C:\Program Files\LLVM\bin"
$vc = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vc`" >nul && cd /d `"$root`" && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER=clang-cl && cmake --build build && ctest --test-dir build --output-on-failure && echo BUILD_OK"
