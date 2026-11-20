// Conquistas (achievements) do Xbox 360: a lista vem da tabela XDBF do proprio jogo (dentro do image.bin); o jogo
// avisa o desbloqueio pela mensagem XUserWriteAchievements (xam.cpp) e o runtime anota em <pasta do perfil>\conquistas.txt.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct Conquista
{
    uint16_t id = 0, pontos = 0;
    uint32_t imagem = 0;          // id da imagem (PNG) na tabela
    std::wstring nome, comoObter;
};

// Le a lista do image.bin ao lado do exe; se ele nao tiver a tabela inteira, monta a imagem a partir do default.xex da
// pasta do jogo (num arquivo temporario). imagens: id -> PNG.
bool ConquistasLer(const std::filesystem::path& exeDir, const std::filesystem::path& pastaJogo, std::vector<Conquista>& lista,
                   std::map<uint32_t, std::vector<uint8_t>>& imagens);

// id -> quando foi desbloqueada (segundos desde 1970), lido de <pastaPerfil>\conquistas.txt
std::map<uint16_t, int64_t> ConquistasDesbloqueadas(const std::filesystem::path& pastaPerfil);

// no jogo: anota o desbloqueio na pasta do perfil em uso (g_saveDir); devolve false se ja estava desbloqueada
bool ConquistaDesbloquear(uint16_t id);

// no jogo: mostra o aviso na tela (icone, nome, pontos) com um som; aviso_conquista.cpp
void ConquistaAvisar(uint16_t id);
