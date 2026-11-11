# Localiza o compilador de shaders da Microsoft (dxcompiler.dll + dxil.dll) no Windows SDK e define DXC_BIN.
# O runtime carrega as DLLs em tempo de execucao para compilar o HLSL gerado; elas sao copiadas ao lado do executavel.
# DXC_BIN pode ser informado na linha de comando (-DDXC_BIN=pasta) para usar outra copia.
if(NOT DXC_BIN)
    file(GLOB _dxc_candidates "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/10.*/x64/dxcompiler.dll")
    list(SORT _dxc_candidates COMPARE NATURAL ORDER DESCENDING)
    foreach(_dll IN LISTS _dxc_candidates)
        get_filename_component(_dir ${_dll} DIRECTORY)
        if(EXISTS ${_dir}/dxil.dll)
            set(DXC_BIN ${_dir})
            break()
        endif()
    endforeach()
endif()
if(NOT DXC_BIN OR NOT EXISTS ${DXC_BIN}/dxcompiler.dll)
    message(FATAL_ERROR "dxcompiler.dll/dxil.dll nao encontrados no Windows SDK (informe -DDXC_BIN=pasta)")
endif()
message(STATUS "DXC: ${DXC_BIN}")
