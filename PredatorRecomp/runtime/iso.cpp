// Jogo direto da ISO (iso.h). A ISO do disco de Xbox 360 (copia de quem joga, ex. a da Redump) tem a particao do
// jogo em XDVDFS: o descritor fica no setor 32 da particao ("MICROSOFT*XBOX*MEDIA", setor e tamanho da pasta raiz).
// A particao comeca em 0 (ISO so do jogo, "xiso") ou depois da particao de video do disco: 0xFD90000 (XGD2, o caso de
// Far Cry Instincts Predator), 0x18300000 (XGD1) ou 0x2080000 (XGD3). Cada pasta e uma arvore binaria de entradas
// { u16 esquerda, u16 direita (em palavras de 4 bytes), u32 setor, u32 tamanho, u8 atributos, u8 tamanho do nome,
// nome }; atributo 0x10 = pasta. Setores de 2048 bytes. O indice inteiro e montado uma vez por ISO (poucos ms).
#include "iso.h"
#include "guest.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace iso
{
    namespace
    {
        constexpr uint64_t SETOR = 2048;

        struct Montada
        {
            HANDLE h = INVALID_HANDLE_VALUE;
            std::vector<Entrada> entradas;                    // 0 = raiz
            std::unordered_map<std::string, size_t> porCaminho;   // "levels\\05_mines\\05_mines.dat" (minusculas)
            bool ok = false;
            ~Montada() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
        };

        std::mutex g_trava;
        std::map<std::wstring, std::shared_ptr<Montada>> g_isos;   // caminho do .iso (minusculas) -> indice

        std::string Minusculas(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }
        std::wstring MinusculasW(std::wstring s) { std::transform(s.begin(), s.end(), s.begin(), ::towlower); return s; }
        uint32_t L32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
        uint16_t L16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }

        bool LerEm(HANDLE h, uint64_t at, void* buf, uint32_t n, uint32_t* lido = nullptr)
        {
            OVERLAPPED ov{};
            ov.Offset = (DWORD)at;
            ov.OffsetHigh = (DWORD)(at >> 32);
            DWORD r = 0;
            const bool ok = ReadFile(h, buf, n, &r, &ov) != 0;
            if (lido) *lido = r;
            return ok && r == n;
        }

        void LerPasta(Montada& m, uint64_t base, size_t indice, const std::string& prefixo, int profundidade)
        {
            const uint64_t setor = m.entradas[indice].inicio, tam = m.entradas[indice].tamanho;
            if (!tam || tam > 64 * 1024 * 1024 || profundidade > 32) return;
            std::vector<uint8_t> d((size_t)tam);
            if (!LerEm(m.h, setor, d.data(), (uint32_t)tam)) return;
            std::set<size_t> vistos;
            std::vector<size_t> pilha{ 0 };
            while (!pilha.empty())
            {
                const size_t off = pilha.back();
                pilha.pop_back();
                if (off + 14 > d.size() || !vistos.insert(off).second) continue;
                const uint16_t esq = L16(&d[off]), dir = L16(&d[off + 2]);
                if (esq == 0xFFFF) continue;   // preenchimento do fim do setor
                const uint8_t atrib = d[off + 12], tn = d[off + 13];
                if (!tn || off + 14 + tn > d.size()) continue;
                Entrada e;
                e.nome.assign((const char*)&d[off + 14], tn);
                e.inicio = base + (uint64_t)L32(&d[off + 4]) * SETOR;
                e.tamanho = L32(&d[off + 8]);
                e.pasta = (atrib & 0x10) != 0;
                const size_t novo = m.entradas.size();
                const std::string caminho = prefixo + Minusculas(e.nome);
                m.entradas.push_back(std::move(e));
                m.entradas[indice].filhos.push_back(novo);
                m.porCaminho[caminho] = novo;
                if (m.entradas[novo].pasta) LerPasta(m, base, novo, caminho + "\\", profundidade + 1);
                if (esq) pilha.push_back((size_t)esq * 4);
                if (dir) pilha.push_back((size_t)dir * 4);
            }
            auto& f = m.entradas[indice].filhos;
            std::sort(f.begin(), f.end(), [&](size_t a, size_t b) { return Minusculas(m.entradas[a].nome) < Minusculas(m.entradas[b].nome); });
        }

        std::shared_ptr<Montada> Montar(const fs::path& arquivo)
        {
            auto m = std::make_shared<Montada>();
            m->h = CreateFileW(arquivo.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (m->h == INVALID_HANDLE_VALUE) return m;
            for (uint64_t base : { 0ull, 0xFD90000ull, 0x18300000ull, 0x2080000ull })
            {
                uint8_t v[0x800];
                if (!LerEm(m->h, base + 32 * SETOR, v, sizeof(v)) || memcmp(v, "MICROSOFT*XBOX*MEDIA", 20) != 0) continue;
                Entrada raiz;
                raiz.pasta = true;
                raiz.inicio = base + (uint64_t)L32(&v[20]) * SETOR;
                raiz.tamanho = L32(&v[24]);
                m->entradas.push_back(raiz);
                LerPasta(*m, base, 0, "", 0);
                m->entradas[0].inicio = 0;
                m->entradas[0].tamanho = 0;
                m->ok = m->entradas.size() > 1;
                Log("[iso] %s: particao do jogo em 0x%llX, %zu arquivos e pastas", arquivo.string().c_str(), (unsigned long long)base, m->entradas.size() - 1);
                break;
            }
            if (!m->ok) Log("[iso] %s: nao e uma ISO de Xbox (XDVDFS nao encontrado)", arquivo.string().c_str());
            return m;
        }

        // separa <arquivo.iso> e o resto do caminho; false = o caminho nao passa por um .iso
        bool Separar(const fs::path& p, fs::path& arquivo, std::string& resto)
        {
            const std::wstring s = p.wstring(), low = MinusculasW(s);
            for (size_t i = low.find(L".iso"); i != std::wstring::npos; i = low.find(L".iso", i + 1))
            {
                const size_t fim = i + 4;
                if (fim != low.size() && low[fim] != L'\\' && low[fim] != L'/') continue;
                arquivo = fs::path(s.substr(0, fim));
                std::error_code ec;
                if (!fs::is_regular_file(arquivo, ec)) continue;
                std::wstring r = fim < s.size() ? s.substr(fim + 1) : L"";
                std::replace(r.begin(), r.end(), L'/', L'\\');
                while (!r.empty() && r.back() == L'\\') r.pop_back();
                resto = Minusculas(fs::path(r).string());
                return true;
            }
            return false;
        }

        std::shared_ptr<Montada> Pegar(const fs::path& arquivo)
        {
            std::lock_guard t(g_trava);
            auto& m = g_isos[MinusculasW(arquivo.wstring())];
            if (!m) m = Montar(arquivo);
            return m;
        }
    }

    bool EIso(const fs::path& p)
    {
        fs::path a;
        std::string r;
        return Separar(p, a, r) && r.empty() && Pegar(a)->ok;
    }

    const Entrada* Achar(const fs::path& p, bool* naoAchou)
    {
        if (naoAchou) *naoAchou = false;
        fs::path a;
        std::string r;
        if (!Separar(p, a, r)) return nullptr;
        auto m = Pegar(a);   // fica no mapa ate o fim do programa: os ponteiros continuam validos
        if (!m->ok) { if (naoAchou) *naoAchou = true; return nullptr; }
        if (r.empty()) return &m->entradas[0];
        auto it = m->porCaminho.find(r);
        if (it == m->porCaminho.end()) { if (naoAchou) *naoAchou = true; return nullptr; }
        return &m->entradas[it->second];
    }

    const Entrada& Filho(const Entrada& pasta, size_t i)
    {
        // as entradas de uma ISO estao num vetor so: o filho e achado pela ISO dona da pasta
        std::lock_guard t(g_trava);
        for (auto& [k, m] : g_isos)
            if (!m->entradas.empty() && &pasta >= &m->entradas.front() && &pasta <= &m->entradas.back())
                return m->entradas[pasta.filhos[i]];
        return pasta;
    }

    uint32_t Ler(const fs::path& p, const Entrada& e, uint64_t at, void* buf, uint32_t n)
    {
        fs::path a;
        std::string r;
        if (!Separar(p, a, r) || at >= e.tamanho) return 0;
        n = (uint32_t)std::min<uint64_t>(n, e.tamanho - at);
        uint32_t lido = 0;
        LerEm(Pegar(a)->h, e.inicio + at, buf, n, &lido);
        return lido;
    }

    HANDLE AbrirHandle(const fs::path& p)
    {
        fs::path a;
        std::string r;
        if (!Separar(p, a, r)) return INVALID_HANDLE_VALUE;
        HANDLE dup = INVALID_HANDLE_VALUE;
        const auto m = Pegar(a);
        if (!m->ok || !DuplicateHandle(GetCurrentProcess(), m->h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return INVALID_HANDLE_VALUE;
        return dup;
    }
}

std::vector<uint8_t> JogoLer(const fs::path& p)
{
    bool naoAchou = false;
    if (const iso::Entrada* e = iso::Achar(p, &naoAchou))
    {
        std::vector<uint8_t> v;
        if (e->pasta || e->tamanho > 0x7FFFFFFF) return v;
        v.resize((size_t)e->tamanho);
        v.resize(iso::Ler(p, *e, 0, v.data(), (uint32_t)v.size()));
        return v;
    }
    if (naoAchou) return {};
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

bool JogoTamanho(const fs::path& p, uint64_t& tamanho)
{
    bool naoAchou = false;
    if (const iso::Entrada* e = iso::Achar(p, &naoAchou)) { tamanho = e->tamanho; return !e->pasta; }
    if (naoAchou) return false;
    std::error_code ec;
    tamanho = fs::file_size(p, ec);
    return !ec;
}

bool JogoExiste(const fs::path& p)
{
    bool naoAchou = false;
    if (iso::Achar(p, &naoAchou)) return true;
    if (naoAchou) return false;
    std::error_code ec;
    return fs::exists(p, ec);
}
