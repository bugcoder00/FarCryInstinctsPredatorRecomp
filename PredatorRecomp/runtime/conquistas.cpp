// Conquistas: leitura da tabela XDBF do jogo e registro dos desbloqueios (veja conquistas.h).
//
// XDBF (big-endian): "XDBF", versao, tamanho da tabela de entradas, n de entradas, tamanho da tabela livre, n livres;
// entradas de 18 bytes {u16 espaco, u64 id, u32 deslocamento, u32 tamanho}; os dados comecam depois das duas tabelas.
// Espaco 1 id "XACH" = conquistas: "XACH", versao, tamanho, u16 n, entradas de 0x24 bytes {u16 id, u16 texto do nome,
// u16 texto "conseguida", u16 texto "como conseguir", u32 imagem, u16 pontos, ...}. Espaco 2 = imagens PNG (pelo id).
// Espaco 3 id 1 = textos em ingles: "XSTR", versao, tamanho, u16 n, {u16 id, u16 tamanho, UTF-8}.
// Far Cry Instincts Predator: 35 conquistas, 1000 pontos, 3 icones; textos em ingles, alemao, frances, espanhol e
// italiano (sem portugues: a versao em portugues usa a explicacao traduzida daqui e o nome em ingles).
#include "conquistas.h"
#include "guest.h"
#include "idioma.h"
#include "opcoes.h"
#include "iso.h"
#include <windows.h>
#include <fstream>
#include <mutex>
#include <ctime>

namespace fs = std::filesystem;
extern fs::path g_saveDir;   // fs.cpp: pasta do perfil em uso (build\save ou save\perfis\<nome>)

namespace
{
    uint32_t B32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
    uint16_t B16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

    std::wstring Utf8(const std::string& s)
    {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }

    // explicacao em portugues (o jogo nao tem textos em portugues)
    const wchar_t* ComoObterPt(uint16_t id)
    {
        switch (id)
        {
        case 7: return L"Termine a história Instincts no nível Rookie (novato).";
        case 8: return L"Termine a história Instincts no nível Hunter (caçador).";
        case 9: return L"Termine a história Instincts no nível Predator (predador).";
        case 26: return L"Faça 50 mortes com armadilhas em qualquer modo solo.";
        case 27: return L"Faça 100 tiros na cabeça em qualquer modo solo.";
        case 30: return L"Faça 40 mortes furtivas em qualquer modo solo.";
        case 32: return L"Faça 80 mortes com o soco feral em qualquer modo solo.";
        case 37: return L"Termine a história Instincts em todos os níveis de dificuldade.";
        case 38: return L"Encontre todas as pesquisas perdidas do Doutor Krieger, espalhadas por todos os mapas das duas histórias.";
        case 40: return L"Termine uma partida online no modo Chaos em primeiro lugar.";
        case 41: return L"Termine uma partida online no modo Team Chaos em primeiro lugar.";
        case 42: return L"Termine uma partida multijogador no modo Steal the Sample em primeiro lugar.";
        case 43: return L"Termine uma partida multijogador no modo Predator em primeiro lugar.";
        case 44: return L"Termine uma partida online no modo Seek n' Secure em primeiro lugar.";
        case 45: return L"Fique em primeiro lugar pelo menos uma vez em todos os modos online.";
        case 46: return L"Fique em primeiro lugar 50 vezes no modo multijogador Chaos.";
        case 47: return L"Fique em primeiro lugar 50 vezes no modo multijogador Team Chaos.";
        case 48: return L"Fique em primeiro lugar 50 vezes no modo multijogador Steal the Sample.";
        case 49: return L"Fique em primeiro lugar 50 vezes no modo multijogador Predator.";
        case 50: return L"Fique em primeiro lugar 50 vezes no modo multijogador Seek n' Secure.";
        case 51: return L"Fique em primeiro lugar 300 vezes somando todos os modos multijogador.";
        case 72: return L"Jogue 80 horas em qualquer modo multijogador.";
        case 74: return L"Faça 300 tiros na cabeça em qualquer modo multijogador.";
        case 75: return L"Faça 1000 mortes no total em qualquer modo multijogador.";
        case 76: return L"Faça 10 mortes múltiplas (multikill) em qualquer modo multijogador.";
        case 79: return L"Faça 200 mortes com granadas em qualquer modo multijogador.";
        case 80: return L"Faça 100 mortes atropelando em qualquer modo multijogador.";
        case 82: return L"Faça 150 mortes com o soco feral em qualquer modo multijogador.";
        case 83: return L"Faça 100 mortes com armadilhas em qualquer modo multijogador.";
        case 84: return L"Termine a história Evolution no nível Rookie (novato).";
        case 85: return L"Termine a história Evolution no nível Hunter (caçador).";
        case 86: return L"Termine a história Evolution no nível Predator (predador).";
        case 87: return L"Termine a história Evolution em todos os níveis de dificuldade.";
        case 88: return L"Termine a história Instincts e a história Evolution.";
        case 89: return L"Termine as duas histórias (Instincts e Evolution) em todos os níveis de dificuldade.";
        }
        return nullptr;
    }

    // tabela XDBF dentro de uma imagem do jogo; false se nao achar ou se estiver incompleta (image.bin antigo)
    bool Analisar(const std::vector<uint8_t>& d, std::vector<Conquista>& lista, std::map<uint32_t, std::vector<uint8_t>>& imagens)
    {
        lista.clear(); imagens.clear();
        const uint8_t marca[4] = { 'X', 'D', 'B', 'F' };
        size_t o = std::string::npos;
        for (size_t i = 0; i + 24 <= d.size(); i += 4)   // a tabela fica alinhada
            if (!memcmp(&d[i], marca, 4)) { o = i; break; }
        if (o == std::string::npos) return false;
        const uint32_t tLen = B32(&d[o + 8]), tCnt = B32(&d[o + 12]), fLen = B32(&d[o + 16]);
        if (tCnt > tLen || tLen > 4096 || fLen > 4096) return false;
        const size_t base = o + 24 + (size_t)tLen * 18 + (size_t)fLen * 8;
        auto blob = [&](uint16_t ns, uint64_t id, size_t& ini, size_t& tam) {
            for (uint32_t i = 0; i < tCnt; i++)
            {
                const uint8_t* e = &d[o + 24 + i * 18];
                const uint64_t eid = (uint64_t)B32(e + 2) << 32 | B32(e + 6);
                if (B16(e) == ns && eid == id)
                {
                    ini = base + B32(e + 10); tam = B32(e + 14);
                    return ini + tam <= d.size();
                }
            }
            return false;
        };
        std::map<uint16_t, std::string> textos;
        size_t si, st;
        if (!blob(3, 1, si, st) || st < 14 || memcmp(&d[si], "XSTR", 4)) return false;
        for (size_t p = si + 14, n = B16(&d[si + 12]); n-- && p + 4 <= si + st;)
        {
            const uint16_t id = B16(&d[p]), len = B16(&d[p + 2]);
            if (p + 4 + len > si + st) break;
            textos[id] = std::string((const char*)&d[p + 4], len);
            p += 4 + len;
        }
        size_t ai, at;
        if (!blob(1, 0x58414348, ai, at) || at < 14 || memcmp(&d[ai], "XACH", 4)) return false;
        for (size_t i = 0, n = B16(&d[ai + 12]); i < n && ai + 14 + (i + 1) * 0x24 <= ai + at; i++)
        {
            const uint8_t* e = &d[ai + 14 + i * 0x24];
            Conquista c;
            c.id = B16(e);
            c.imagem = B32(e + 8);
            c.pontos = B16(e + 12);
            c.nome = Utf8(textos[B16(e + 2)]);
            c.comoObter = Utf8(textos[B16(e + 6)]);
            if (const wchar_t* pt = IDIOMA_INGLES ? nullptr : ComoObterPt(c.id)) c.comoObter = pt;
            if (c.nome.empty()) continue;   // entrada vazia (a tabela tem vagas no fim)
            lista.push_back(c);
            size_t ii, it;
            if (!imagens.count(c.imagem) && blob(2, c.imagem, ii, it))
                imagens[c.imagem].assign(d.begin() + ii, d.begin() + ii + it);
        }
        return !lista.empty();
    }

    std::vector<uint8_t> LerArquivo(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
    }

    std::mutex g_mx;
}

bool ConquistasLer(const fs::path& exeDir, const fs::path& pastaJogo, std::vector<Conquista>& lista,
                   std::map<uint32_t, std::vector<uint8_t>>& imagens)
{
    if (Analisar(LerArquivo(exeDir / GUEST_IMAGE_FILE), lista, imagens)) return true;
    // image.bin sem a tabela inteira (o antigo do desenvolvimento) ou ainda nao criado: monta do default.xex
    std::error_code ec;
    if (pastaJogo.empty() || !JogoExiste(pastaJogo / GUEST_XEX_NAME)) return false;
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    fs::path t = fs::path(tmp) / L"fcrecomp_conquistas.bin";
    std::string erro;
    bool ok = XexCriarImagem(pastaJogo / GUEST_XEX_NAME, t, erro, false) && Analisar(LerArquivo(t), lista, imagens);
    fs::remove(t, ec);
    return ok;
}

std::map<uint16_t, int64_t> ConquistasDesbloqueadas(const fs::path& pastaPerfil)
{
    std::map<uint16_t, int64_t> r;
    std::ifstream f(pastaPerfil / "conquistas.txt");
    unsigned id; long long quando;
    std::string linha;
    while (std::getline(f, linha))
        if (sscanf(linha.c_str(), "%u %lld", &id, &quando) == 2 && id < 65536) r[(uint16_t)id] = quando;
    return r;
}

bool ConquistaDesbloquear(uint16_t id)
{
    std::lock_guard l(g_mx);
    if (ConquistasDesbloqueadas(g_saveDir).count(id)) return false;
    std::error_code ec;
    fs::create_directories(g_saveDir, ec);
    std::ofstream(g_saveDir / "conquistas.txt", std::ios::app) << id << " " << (long long)time(nullptr) << "\n";
    return true;
}
