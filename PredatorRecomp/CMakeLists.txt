# PredatorRecomp - recompilador estatico autoral (CPU PowerPC/VMX128 -> C, microcodigo de shader -> HLSL)
cmake_minimum_required(VERSION 3.24)

project(PredatorRecomp
    VERSION 0.1.2
    DESCRIPTION "Recompilador estatico de CPU e GPU (ferramenta CLI)"
    LANGUAGES CXX)

# ---------------------------------------------------------------- opcoes
option(PREDATOR_WARNINGS_AS_ERRORS "Trata avisos como erros" ON)
option(PREDATOR_BUILD_TESTS "Compila os testes (ctest)" ON)

# ---------------------------------------------------------------- padrao da linguagem
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Tipo de compilacao" FORCE)
endif()

# Runtime estatico no MSVC/clang-cl: o executavel nao depende das DLLs do Visual C++
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------- avisos (alvo de interface reaproveitado)
add_library(predator_warnings INTERFACE)
if(MSVC)
    target_compile_options(predator_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(PREDATOR_WARNINGS_AS_ERRORS)
        target_compile_options(predator_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(predator_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(PREDATOR_WARNINGS_AS_ERRORS)
        target_compile_options(predator_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------- modulos
# Dependencias (sem ciclos): support <- loader <- cpu; support <- gpu; cpu, gpu <- core <- executavel

# support: diagnosticos e utilitarios usados por todos os modulos
add_library(predator_support STATIC
    src/core/Diagnostics.cpp)
target_include_directories(predator_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(predator_support PRIVATE predator_warnings)

# loader: executavel XEX2 e imagem de memoria
add_library(predator_loader STATIC
    src/loader/Executable.cpp)
target_link_libraries(predator_loader PUBLIC predator_support PRIVATE predator_warnings)

# cpu: decodificador PowerPC 64 bits + VMX + VMX128 e gerador de C
# o cabecalho do runtime (src/cpu/runtime/predator_cpu.h) e embutido no executavel e copiado para a saida
set(PREDATOR_RUNTIME_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/runtime/predator_cpu.h)
set(PREDATOR_RUNTIME_EMBED ${CMAKE_CURRENT_BINARY_DIR}/generated/RuntimeHeader.cpp)
add_custom_command(
    OUTPUT ${PREDATOR_RUNTIME_EMBED}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${PREDATOR_RUNTIME_HEADER} -DOUTPUT=${PREDATOR_RUNTIME_EMBED}
            -DSYMBOL=kPredatorCpuHeader -DNAMESPACE=predator::cpu -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${PREDATOR_RUNTIME_HEADER} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "Embutindo predator_cpu.h")

add_library(predator_cpu STATIC
    src/cpu/Analysis.cpp
    src/cpu/CodeGen.cpp
    src/cpu/CpuTranslator.cpp
    src/cpu/Decoder.cpp
    src/cpu/Disassembler.cpp
    ${PREDATOR_RUNTIME_EMBED})
target_link_libraries(predator_cpu PUBLIC predator_loader predator_support PRIVATE predator_warnings)

# gpu: leitor de microcodigo de shader e gerador de HLSL
add_library(predator_gpu STATIC
    src/gpu/ShaderTranslator.cpp
    src/gpu/XenosHlsl.cpp
    src/gpu/XenosMicrocode.cpp)
target_link_libraries(predator_gpu PUBLIC predator_support PRIVATE predator_warnings)

# core: linha de comando e orquestracao do pipeline
add_library(predator_core STATIC
    src/core/CommandLine.cpp
    src/core/Pipeline.cpp)
target_link_libraries(predator_core PUBLIC predator_cpu predator_gpu predator_support PRIVATE predator_warnings)
target_compile_definitions(predator_core PUBLIC
    PREDATOR_VERSION_STRING="${PROJECT_VERSION}")

# ---------------------------------------------------------------- executavel
add_executable(predatorrecomp src/main.cpp)
target_link_libraries(predatorrecomp PRIVATE predator_core predator_warnings)

install(TARGETS predatorrecomp RUNTIME DESTINATION bin)

# ---------------------------------------------------------------- testes
if(PREDATOR_BUILD_TESTS)
    enable_testing()
    add_executable(predator_tests tests/CommandLineTests.cpp)
    target_link_libraries(predator_tests PRIVATE predator_core predator_warnings)
    add_test(NAME command_line COMMAND predator_tests)

    add_executable(predator_decoder_tests tests/DecoderTests.cpp)
    target_link_libraries(predator_decoder_tests PRIVATE predator_cpu predator_warnings)
    add_test(NAME decoder COMMAND predator_decoder_tests)

    add_executable(predator_shader_tests tests/ShaderTests.cpp)
    target_link_libraries(predator_shader_tests PRIVATE predator_gpu predator_warnings)
    add_test(NAME shaders COMMAND predator_shader_tests)

    # gerador de C de ponta a ponta: funcoes montadas a mao -> C -> compilado como C puro -> executado
    enable_language(C)
    add_executable(predator_codegen_sample tests/CodeGenSample.cpp)
    target_link_libraries(predator_codegen_sample PRIVATE predator_cpu predator_warnings)
    set(PREDATOR_SAMPLE_DIR ${CMAKE_CURRENT_BINARY_DIR}/codegen_sample)
    add_custom_command(
        OUTPUT ${PREDATOR_SAMPLE_DIR}/codegen_sample.c ${PREDATOR_SAMPLE_DIR}/predator_cpu.h
        COMMAND predator_codegen_sample ${PREDATOR_SAMPLE_DIR}
        DEPENDS predator_codegen_sample
        COMMENT "Gerando C de exemplo")
    add_executable(predator_codegen_run tests/CodeGenRun.c ${PREDATOR_SAMPLE_DIR}/codegen_sample.c)
    target_include_directories(predator_codegen_run PRIVATE ${PREDATOR_SAMPLE_DIR})
    add_test(NAME codegen_run COMMAND predator_codegen_run)
endif()
