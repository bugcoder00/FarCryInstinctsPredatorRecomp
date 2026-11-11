# Converte um arquivo de texto em um vetor de bytes C++ (usado para embutir o cabecalho do runtime no executavel)
# Uso: cmake -DINPUT=arquivo -DOUTPUT=saida.cpp -DSYMBOL=nome -DNAMESPACE=ns -P EmbedFile.cmake
file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hexLength)
math(EXPR size "${hexLength} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n    " bytes "${bytes}")
file(WRITE "${OUTPUT}" "// gerado pelo CMake a partir de ${INPUT}\n#include <cstddef>\nnamespace ${NAMESPACE}\n{\n    extern const unsigned char ${SYMBOL}[];\n    extern const std::size_t ${SYMBOL}Size;\n    const unsigned char ${SYMBOL}[] = {\n    ${bytes}0x00 };\n    const std::size_t ${SYMBOL}Size = ${size};\n}\n")
