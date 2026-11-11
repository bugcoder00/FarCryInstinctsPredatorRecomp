# PredatorGame (e PredatorEditor, com PREDATOR_EDITOR=ON): o jogo montado com o codigo gerado pelo PredatorRecomp + o runtime (PredatorRecomp/runtime)
#
#   cmake -S game -B build_game -G Ninja -DPREDATOR_GENERATED=<pasta com predator_*.c/.h> -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#
# Nenhum arquivo, nome ou estrutura de outros recompiladores e usado: o contexto da CPU que o runtime usa vem
# de bridge/guest_cpu_backend.h, os shaders sao traduzidos por src/gpu e o compilador de shaders (DXC) vem do Windows SDK.
cmake_minimum_required(VERSION 3.24)
project(PredatorGame LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(PREDATOR_GENERATED "" CACHE PATH "Pasta com o codigo gerado (predatorrecomp cpu ... --out X  ->  X/cpu)")
if(NOT EXISTS "${PREDATOR_GENERATED}/predator_functions.h")
    message(FATAL_ERROR "PREDATOR_GENERATED nao aponta para o codigo gerado: ${PREDATOR_GENERATED}")
endif()

set(RUNTIME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../runtime)
# PREDATOR_EDITOR=ON: monta o editor de mapas (ige.xex -> PredatorEditor.exe, imagem em <projeto>/editor) com o mesmo
# runtime; o menu MAP EDITOR do jogo abre esse programa (xam.cpp, XamLoaderLaunchTitle)
option(PREDATOR_EDITOR "monta o editor de mapas (ige.xex) em vez do jogo" OFF)
# PREDATOR_INGLES=ON: textos do programa em ingles (launcher, controles, tela de carregamento; runtime/idioma.h)
option(PREDATOR_INGLES "textos do programa em ingles" OFF)
if(PREDATOR_EDITOR)
    set(PROGRAMA PredatorEditor)
    set(GAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../editor)
    set(IMAGEM image_editor.bin)
else()
    set(PROGRAMA PredatorGame)
    set(GAME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../game)
    set(IMAGEM image.bin)
endif()

# ---------------------------------------------------------------- codigo do jogo traduzido (C puro)
file(GLOB GENERATED_SOURCES ${PREDATOR_GENERATED}/*.c)
add_library(predator_game_code OBJECT ${GENERATED_SOURCES})
target_include_directories(predator_game_code PUBLIC ${PREDATOR_GENERATED})
target_compile_options(predator_game_code PRIVATE
    /clang:-march=x86-64-v2
    /clang:-fno-strict-aliasing
    -Wno-unused-label
    -Wno-unused-variable
    -Wno-unused-but-set-variable)

# ---------------------------------------------------------------- runtime + ponte
file(GLOB RUNTIME_SOURCES ${RUNTIME_DIR}/*.cpp)
# tradutor de shaders do PredatorRecomp (microcodigo Xenos -> HLSL), usado por runtime/xenos_translate.cpp
set(PREDATOR_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
# icone do exe (runtime/recursos/predator.rc -> predator.ico, gerado por tools/gerar_icone.py)
enable_language(RC)
add_executable(${PROGRAMA} ${RUNTIME_SOURCES} bridge/predator_bridge.cpp
    ${PREDATOR_SRC}/gpu/XenosHlsl.cpp ${PREDATOR_SRC}/gpu/XenosMicrocode.cpp ${RUNTIME_DIR}/recursos/predator.rc)
# bridge/ vem antes: e de la que sai o guest_cpu_backend.h
target_include_directories(${PROGRAMA} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bridge ${RUNTIME_DIR} ${PREDATOR_SRC})
target_link_libraries(${PROGRAMA} PRIVATE predator_game_code)
target_compile_definitions(${PROGRAMA} PRIVATE $<$<BOOL:${PREDATOR_EDITOR}>:PREDATOR_EDITOR> $<$<BOOL:${PREDATOR_INGLES}>:PREDATOR_INGLES> NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(${PROGRAMA} PRIVATE /clang:-march=x86-64-v2 /EHsc -Wno-microsoft-cast -Wno-duplicate-decl-specifier)
# programa de janela: o cmd nao abre; main.cpp cria o console so com "mostrar registro" ou FCRECOMP_CONSOLE=1
target_link_options(${PROGRAMA} PRIVATE /MAP:${CMAKE_BINARY_DIR}/FCRecomp.map /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/FindDxc.cmake)
add_custom_command(TARGET ${PROGRAMA} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GAME_DIR}/image.bin $<TARGET_FILE_DIR:${PROGRAMA}>/${IMAGEM}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DXC_BIN}/dxcompiler.dll ${DXC_BIN}/dxil.dll $<TARGET_FILE_DIR:${PROGRAMA}>)
