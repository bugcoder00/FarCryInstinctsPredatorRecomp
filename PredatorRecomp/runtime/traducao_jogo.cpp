// Traducao PT-BR do jogo aplicada na leitura (o arquivo no disco nao muda). Com o idioma do launcher em portugues
// (idioma.h), o jogo ve os pacotes common, levels\05_MINES\05_mines e levels\11_WASTELAND\11_wasteland como se o
// patch de traducao (traducao\gerar_patch.py, patch_ptbr.bin) estivesse aplicado: o .fat vem montado aqui (as tabelas
// de texto apontam para depois do fim original do .dat) e o .dat ganha, depois do fim original, as tabelas traduzidas.
// Em ingles, se a copia de quem joga tem a traducao instalada pelo programa do patch (<pacote>.fat.original), o jogo
// ve o .fat original (os dados originais continuam no .dat): o idioma do launcher vale nos dois sentidos.
// O patch vem dentro do exe (recurso 100, so quando traducao\patch_ptbr.bin existe na hora de compilar) ou num
// patch_ptbr.bin ao lado do exe. Cada tabela so e trocada se o hash e os tamanhos conferem com a versao USA/Europa.
// FCRECOMP_TRADUCAO=0/1 forca desligada/ligada (testes).
#include "traducao_jogo.h"
#include "iso.h"
#include "idioma.h"
#include "guest.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <atomic>
#ifdef PREDATOR_EDITOR
#include "npc_anims_lista.h"
#endif

namespace fs = std::filesystem;
extern fs::path g_gameDir;

namespace traducao
{
    namespace
    {
        struct Tabela { uint32_t hash, guardado, real; std::vector<uint8_t> dados; };
        struct Pacote
        {
            std::shared_ptr<const std::vector<uint8_t>> fat;     // .fat que o jogo ve (nullptr = o do disco)
            uint64_t inicioExtra = 0;                            // fim original do .dat
            std::shared_ptr<const std::vector<uint8_t>> extra;   // tabelas traduzidas depois do fim (nullptr = nada)
        };
        std::once_flag g_uma;
        std::map<std::string, Pacote> g_pacotes;   // "common", "levels\05_mines\05_mines" (minusculas)

        std::string Minusculas(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }
        uint32_t L32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

        std::vector<uint8_t> LerTudo(const fs::path& p) { return JogoLer(p); }   // da pasta ou da ISO (iso.h)

        // entradas do .fat: { posicao no .dat, tamanho guardado, tamanho real, hash do nome }
        bool LerFat(const std::vector<uint8_t>& d, std::vector<std::array<uint32_t, 4>>& e)
        {
            if (d.size() < 8 || memcmp(d.data(), "DAT2", 4)) return false;
            const uint32_t n = L32(&d[4]);
            if (d.size() < 8 + (size_t)n * 16) return false;
            e.resize(n);
            for (uint32_t i = 0; i < n; i++)
                for (int k = 0; k < 4; k++) e[i][k] = L32(&d[8 + i * 16 + k * 4]);
            return true;
        }

        std::vector<uint8_t> GravarFat(const std::vector<std::array<uint32_t, 4>>& e)
        {
            std::vector<uint8_t> d(8 + e.size() * 16);
            memcpy(d.data(), "DAT2", 4);
            const uint32_t n = (uint32_t)e.size();
            memcpy(&d[4], &n, 4);
            for (size_t i = 0; i < e.size(); i++) memcpy(&d[8 + i * 16], e[i].data(), 16);
            return d;
        }

        // patch_ptbr.bin: "PRPT", versao 1, quantidade; por tabela: u16 + nome do pacote, hash, guardado, real,
        // tamanho + a tabela traduzida
        std::vector<uint8_t> LerPatch()
        {
            if (HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(100), MAKEINTRESOURCEW(10)))   // 10 = RT_RCDATA
                if (HGLOBAL g = LoadResource(nullptr, r))
                {
                    auto* p = (const uint8_t*)LockResource(g);
                    return std::vector<uint8_t>(p, p + SizeofResource(nullptr, r));
                }
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            return LerTudo(fs::path(exe).parent_path() / "patch_ptbr.bin");
        }

        bool Ligada()
        {
            if (const char* e = getenv("FCRECOMP_TRADUCAO")) return atoi(e) != 0;
            return !IDIOMA_INGLES;
        }

#ifdef PREDATOR_EDITOR
        // Editor de mapas: o farcryed nao traz as animacoes dos mercenarios (so os modelos); elas so existem nos pacotes
        // das fases da historia. Para o teste de NPC (npc_editor.cpp) o editor ve esses arquivos dentro do farcryed:
        // entradas novas no .fat (em ordem de hash, como o jogo procura) e os bytes guardados copiados do pacote da
        // fase para depois do fim do .dat. Lista: npc_anims_lista.h (tools/gerar_npc_anims.py).
        // Ligado so com FCRECOMP_NPCANIMS=1 (teste de 18/11: o editor le o merc01.xba, mas nenhum clipe; e um mercenario
        // com o pacote do jogador (mp_third.xba, clipes todos no farcryed) tambem fica em pose T: personagem criado
        // durante o TESTAR MAPA nao anima, falta outra coisa alem dos arquivos).
        void PrepararNpcAnims()
        {
            if (const char* v = getenv("FCRECOMP_NPCANIMS"); !v || atoi(v) == 0) return;   // so em teste
            // bloco unico com os bytes guardados (posicoes relativas ao fim de cada pacote, alinhadas em 16)
            std::map<std::string, std::vector<uint32_t>> porPacote;
            for (auto& a : kNpcAnims) porPacote[a.pacote].push_back(a.hash);
            auto extra = std::make_shared<std::vector<uint8_t>>();
            std::vector<std::array<uint32_t, 4>> bloco;   // { posicao no bloco, guardado, real, hash }
            size_t faltou = 0;
            for (auto& [pacote, hashes] : porPacote)
            {
                const fs::path pb = g_gameDir / pacote;
                fs::path pf = pb; pf += ".fat";
                fs::path pd = pb; pd += ".dat";
                std::vector<std::array<uint32_t, 4>> pe;
                std::ifstream d(pd, std::ios::binary);
                if (!LerFat(LerTudo(pf), pe) || !d) { faltou += hashes.size(); continue; }
                std::map<uint32_t, size_t> idx;
                for (size_t i = 0; i < pe.size(); i++) idx[pe[i][3]] = i;
                for (uint32_t h : hashes)
                {
                    auto it = idx.find(h);
                    if (it == idx.end()) { faltou++; continue; }
                    const auto& x = pe[it->second];
                    while (extra->size() % 16) extra->push_back(0);
                    const size_t pos = extra->size();
                    extra->resize(pos + x[1]);
                    d.seekg(x[0]);
                    d.read((char*)extra->data() + pos, x[1]);
                    if (!d) { d.clear(); extra->resize(pos); faltou++; continue; }
                    bloco.push_back({ (uint32_t)pos, x[1], x[2], h });
                }
            }
            if (bloco.empty()) return;
            // pacotes que recebem o bloco: farcryed e os das fases do editor (levels\met_*)
            std::vector<std::string> alvos{ "farcryed" };
            std::error_code ec;
            for (auto& d : fs::directory_iterator(g_gameDir / "levels", ec))
            {
                const std::string pasta = Minusculas(d.path().filename().string());
                if (pasta.rfind("met_", 0) == 0) alvos.push_back("levels\\" + pasta + "\\" + pasta);
            }
            size_t pacotes = 0;
            for (auto& alvo : alvos)
            {
                const fs::path base = g_gameDir / alvo;
                fs::path fat = base; fat += ".fat";
                fs::path dat = base; dat += ".dat";
                std::vector<std::array<uint32_t, 4>> e;
                if (!LerFat(LerTudo(fat), e)) continue;
                uint64_t fim = 0;
                for (auto& x : e) fim = std::max<uint64_t>(fim, (uint64_t)x[0] + x[1]);
                fim = std::max<uint64_t>(fim, fs::file_size(dat, ec));
                fim = (fim + 15) & ~15ull;
                if (fim + extra->size() > 0xFFFFFFFFull) continue;
                std::set<uint32_t> tem;
                for (auto& x : e) tem.insert(x[3]);
                for (auto& x : bloco)
                    if (!tem.count(x[3])) e.push_back({ (uint32_t)(fim + x[0]), x[1], x[2], x[3] });
                std::sort(e.begin(), e.end(), [](const auto& a, const auto& b) { return a[3] < b[3]; });
                Pacote pc;
                pc.fat = std::make_shared<const std::vector<uint8_t>>(GravarFat(e));
                pc.inicioExtra = fim;
                pc.extra = extra;
                g_pacotes[alvo] = std::move(pc);
                pacotes++;
            }
            Log("[npc] %zu arquivos de animacao dos mercenarios vindos das fases (%zu bytes; %zu nao achados) em %zu pacotes do editor", bloco.size(), extra->size(), faltou, pacotes);
        }
#endif

        void Preparar()
        {
            const bool pt = Ligada();
#ifdef PREDATOR_EDITOR
            PrepararNpcAnims();
#endif
            std::map<std::string, std::vector<Tabela>> porPacote;
            if (pt)
            {
                const auto d = LerPatch();
                if (d.size() < 12 || memcmp(d.data(), "PRPT", 4) || L32(&d[4]) != 1)
                {
                    Log("[traducao] patch PT-BR nao veio no exe nem ao lado dele: o jogo fica em ingles");
                    return;
                }
                size_t p = 12;
                for (uint32_t i = 0, n = L32(&d[8]); i < n; i++)
                {
                    if (p + 2 > d.size()) return;
                    const uint16_t tn = (uint16_t)(d[p] | d[p + 1] << 8);
                    if (p + 2 + tn + 16 > d.size()) return;
                    std::string nome = Minusculas(std::string((const char*)&d[p + 2], tn));
                    p += 2 + tn;
                    Tabela t{ L32(&d[p]), L32(&d[p + 4]), L32(&d[p + 8]) };
                    const uint32_t tam = L32(&d[p + 12]);
                    p += 16;
                    if (p + tam > d.size()) return;
                    t.dados.assign(d.begin() + p, d.begin() + p + tam);
                    p += tam;
                    porPacote[nome].push_back(std::move(t));
                }
            }
            else
                for (const char* n : { "common", "levels\\05_mines\\05_mines", "levels\\11_wasteland\\11_wasteland" }) porPacote[n];

            for (auto& [nome, tabelas] : porPacote)
            {
                const fs::path base = g_gameDir / nome;
                fs::path fat = base; fat += ".fat";
                fs::path orig = fat; orig += ".original";
                std::error_code ec;
                const bool temOriginal = fs::exists(orig, ec);   // traducao instalada pelo programa do patch
                const auto noDisco = LerTudo(fat);
                const auto original = temOriginal ? LerTudo(orig) : noDisco;
                std::vector<std::array<uint32_t, 4>> e;
                if (!LerFat(original, e)) continue;
                Pacote pc;
                if (!pt)
                {
                    if (temOriginal && original != noDisco)
                    {
                        pc.fat = std::make_shared<const std::vector<uint8_t>>(original);
                        Log("[traducao] %s: ingles escolhido, o jogo ve o .fat original (a traducao instalada fica de lado)", nome.c_str());
                    }
                }
                else
                {
                    uint64_t fim = 0;
                    for (auto& x : e) fim = std::max<uint64_t>(fim, (uint64_t)x[0] + x[1]);
                    std::map<uint32_t, size_t> porHash;
                    for (size_t i = 0; i < e.size(); i++) porHash[e[i][3]] = i;
                    bool confere = true;
                    for (auto& t : tabelas)
                    {
                        auto it = porHash.find(t.hash);
                        if (it == porHash.end() || e[it->second][1] != t.guardado || e[it->second][2] != t.real) { confere = false; break; }
                    }
                    if (!confere || fim + 0x1000000 > 0xFFFFFFFFull)
                    {
                        Log("[traducao] %s: a copia do jogo nao e a versao esperada; este pacote fica em ingles", nome.c_str());
                        continue;
                    }
                    auto extra = std::make_shared<std::vector<uint8_t>>();
                    for (auto& t : tabelas)
                    {
                        while ((fim + extra->size()) % 16) extra->push_back(0);
                        auto& x = e[porHash[t.hash]];
                        x[0] = (uint32_t)(fim + extra->size());
                        x[1] = x[2] = (uint32_t)t.dados.size();
                        extra->insert(extra->end(), t.dados.begin(), t.dados.end());
                    }
                    pc.fat = std::make_shared<const std::vector<uint8_t>>(GravarFat(e));
                    pc.inicioExtra = fim;
                    pc.extra = extra;
                    Log("[traducao] %s: %zu tabelas de texto em portugues (%zu bytes depois do fim do .dat)", nome.c_str(), tabelas.size(), extra->size());
                }
                if (pc.fat) g_pacotes[nome] = std::move(pc);
            }
        }

        // pacote do caminho (sem a extensao, relativo a pasta do jogo) ou nullptr
        const Pacote* Achar(const fs::path& caminho, std::string& ext)
        {
            if (g_gameDir.empty()) return nullptr;
            ext = Minusculas(caminho.extension().string());
            if (ext != ".fat" && ext != ".dat") return nullptr;
            std::call_once(g_uma, Preparar);
            if (g_pacotes.empty()) return nullptr;
            std::error_code ec;
            fs::path rel = caminho.lexically_relative(g_gameDir);   // lexico: a pasta do jogo pode ser um .iso
            if (ec || rel.empty()) return nullptr;
            rel.replace_extension();
            std::string r = Minusculas(rel.string());
            std::replace(r.begin(), r.end(), '/', '\\');
            auto it = g_pacotes.find(r);
            return it == g_pacotes.end() ? nullptr : &it->second;
        }
    }

    bool Arquivo(const fs::path& caminho, uint64_t tamanhoNoDisco, Virtual& v)
    {
        std::string ext;
        const Pacote* p = Achar(caminho, ext);
        if (!p) return false;
        if (ext == ".fat")
        {
            v = Virtual{ p->fat, 0, nullptr, p->fat->size() };
            return true;
        }
        if (!p->extra) return false;
        v = Virtual{ nullptr, p->inicioExtra, p->extra, p->inicioExtra + p->extra->size() };
        (void)tamanhoNoDisco;
        return true;
    }

    uint32_t Ler(const Virtual& v, HANDLE h, uint64_t at, uint8_t* buf, uint32_t n, uint64_t base)
    {
        if (at >= v.tamanho) return 0;
        n = (uint32_t)std::min<uint64_t>(n, v.tamanho - at);
        if (v.inteiro)   // .fat montado aqui
        {
            memcpy(buf, v.inteiro->data() + at, n);
            return n;
        }
        uint32_t lido = 0;
        if (at < v.inicioExtra)   // parte do .dat original
        {
            const uint32_t parte = (uint32_t)std::min<uint64_t>(n, v.inicioExtra - at);
            OVERLAPPED ov{};
            ov.Offset = (DWORD)(base + at);
            ov.OffsetHigh = (DWORD)((base + at) >> 32);
            DWORD r = 0;
            ReadFile(h, buf, parte, &r, &ov);
            lido = r;
            if (r < parte) return lido;
        }
        if (lido < n)   // tabelas traduzidas
        {
#ifdef PREDATOR_EDITOR
            static std::atomic<int> vezes{ 0 };
            if (vezes++ < 40) Log("[npc] leitura depois do fim do .dat: +%llu, %u bytes", (unsigned long long)(at + lido - v.inicioExtra), n - lido);
#endif
            const uint64_t de = at + lido - v.inicioExtra;
            memcpy(buf + lido, v.extra->data() + de, n - lido);
            lido = n;
        }
        return lido;
    }

    bool Tamanho(const fs::path& caminho, uint64_t& tamanho)
    {
        Virtual v;
        if (!Arquivo(caminho, 0, v)) return false;
        tamanho = v.tamanho;
        return true;
    }
}
