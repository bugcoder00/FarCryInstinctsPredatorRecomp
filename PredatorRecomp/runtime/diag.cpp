// Ferramenta de diagnostico (veja diag.h)
#include "diag.h"
#include "guest.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>

bool ProfilerKeyRequest();   // profiler.cpp: medicao na tecla F3 (FCRECOMP_PROFILE=F3)
bool InputStopRecording();   // input.cpp: F4 termina a gravacao do roteiro (FCRECOMP_GRAVAR)
extern std::atomic<uint64_t> g_missionStartFrame;   // gpu.cpp

namespace diag
{
    bool g_loopShaders = false;
    bool g_noMips = false;
    bool g_noSaturate = false;
    volatile bool g_flushShaders = false;
    volatile bool g_flushTextures = false;

    namespace
    {
        struct Entry { uint64_t count = 0; uint64_t firstFrame = 0; std::string detail; };
        std::mutex g_mutex;
        std::map<std::string, Entry> g_entries[CatCount];
        std::atomic<uint64_t> g_frame{ 0 };
        std::atomic<uint64_t> g_captureFrame{ 0 };       // o mais recente (titulo da janela)
        // os ultimos quadros marcados: com capturas em quadros seguidos, a lista de desenhos de um quadro e gravada
        // quando o seguinte ja foi marcado
        std::atomic<uint64_t> g_captureRing[4]{};
        std::atomic<uint32_t> g_captureNext{ 0 };
        void MarkCapture(uint64_t frame) { g_captureFrame = frame; g_captureRing[g_captureNext++ & 3] = frame; }
        std::atomic<bool> g_memDumpRequest{ false };
        const auto g_start = std::chrono::steady_clock::now();

        const char* CatTitle(int c)
        {
            switch (c)
            {
            case Crash: return "TRAVAMENTOS / CHAMADAS PARA FUNCOES INEXISTENTES";
            case Stub: return "FUNCOES DO SISTEMA (KERNEL/XAM) CHAMADAS QUE AINDA NAO EXISTEM";
            case Kernel: return "PARTES DE FUNCOES DO SISTEMA NAO IMPLEMENTADAS";
            case File: return "ARQUIVOS QUE O JOGO PEDIU E NAO FORAM ENCONTRADOS";
            case GpuPacket: return "COMANDOS DA GPU (PM4) IGNORADOS";
            case GpuFeature: return "RECURSOS DA GPU SEM SUPORTE";
            case Shader: return "SHADERS COM PROBLEMA";
            default: return "OUTROS";
            }
        }
    }

    void Note(Cat cat, const std::string& key, const std::string& detail)
    {
        std::lock_guard lock(g_mutex);
        auto& e = g_entries[cat][key];
        if (e.count++ == 0)
        {
            e.firstFrame = g_frame;
            e.detail = detail;
        }
    }

    void SetFrame(uint64_t frame)
    {
        g_frame = frame;
        static const char* env = getenv("FCRECOMP_CAPTURE");   // mesma captura do F9, nos quadros escolhidos ("1250,1300")
        for (const char* e = env; e && *e; e = strchr(e, ',') ? strchr(e, ',') + 1 : "")
            if (frame == strtoull(e, nullptr, 10)) MarkCapture(frame);
        // FCRECOMP_CAPTURAARQUIVO=1: quando aparece o arquivo "capturar.txt" (pasta do jogo), captura o quadro seguinte e o apaga
        static const bool porArquivo = getenv("FCRECOMP_CAPTURAARQUIVO") != nullptr;
        if (porArquivo && frame % 15 == 0 && GetFileAttributesA("capturar.txt") != INVALID_FILE_ATTRIBUTES)
        {
            DeleteFileA("capturar.txt");
            MarkCapture(frame + 1);
        }
    }
    uint64_t CurrentFrame() { return g_frame; }

    void WriteReport(const char* reason)
    {
        std::lock_guard lock(g_mutex);
        FILE* f = fopen("diagnostico.txt", "w");
        if (!f) return;
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "RELATORIO DE DIAGNOSTICO - Far Cry Instincts Predator Recomp\n");
        fprintf(f, "Gerado em %04u-%02u-%02u %02u:%02u:%02u (%s), %.0f s de jogo, quadro %llu\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, reason, secs, (unsigned long long)g_frame.load());
        fprintf(f, "Interruptores: shaders %s, mipmaps %s, limite de cor 0-1 %s\n\n",
            g_loopShaders ? "LACO (F5)" : "linha reta", g_noMips ? "DESLIGADOS (F6)" : "ligados",
            g_noSaturate ? "DESLIGADO (F7)" : "ligado");

        size_t total = 0;
        for (int c = 0; c < CatCount; c++)
        {
            auto& m = g_entries[c];
            if (m.empty()) continue;
            total += m.size();
            std::vector<std::pair<std::string, const Entry*>> rows;
            for (auto& [k, e] : m) rows.push_back({ k, &e });
            std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.second->count > b.second->count; });
            fprintf(f, "== %s (%zu) ==\n", CatTitle(c), m.size());
            for (auto& [k, e] : rows)
            {
                fprintf(f, "  %8llux  %s   [1a vez no quadro %llu]\n", (unsigned long long)e->count, k.c_str(), (unsigned long long)e->firstFrame);
                if (!e->detail.empty()) fprintf(f, "             %s\n", e->detail.c_str());
            }
            fprintf(f, "\n");
        }
        if (!total) fprintf(f, "Nenhum problema registrado.\n");
        fclose(f);
        Log("[diag] relatorio escrito em diagnostico.txt (%s, %zu problemas diferentes)", reason, total);
    }

    void RequestCapture()
    {
        MarkCapture(g_frame + 2);   // o proximo quadro inteiro
        Log("[diag] capturando o quadro %llu", (unsigned long long)g_captureFrame.load());
    }

    void CaptureAt(uint64_t frame) { MarkCapture(frame); }

    bool Capturing(uint64_t frame)
    {
        if (!frame) return false;
        for (auto& f : g_captureRing) if (f == frame) return true;
        return false;
    }

    std::string CaptureDir(uint64_t frame)
    {
        std::string dir = "diag\\quadro_" + std::to_string(frame);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    bool OnKey(unsigned vk)
    {
        switch (vk)
        {
        case VK_F3:
        {
            if (!::ProfilerKeyRequest()) return false;   // perfilador desligado: a tecla segue para o jogo
            Log("[diag] F3: medindo os proximos segundos");
            return true;
        }
        case VK_F4:
            return ::InputStopRecording();   // sem gravacao em andamento a tecla segue para o jogo
        case VK_F5:
            g_loopShaders = !g_loopShaders;
            g_flushShaders = true;
            Log("[diag] F5: shaders no modo %s", g_loopShaders ? "LACO" : "linha reta");
            return true;
        case VK_F6:
            g_noMips = !g_noMips;
            g_flushTextures = true;
            Log("[diag] F6: mipmaps %s", g_noMips ? "DESLIGADOS" : "ligados");
            return true;
        case VK_F7:
            g_noSaturate = !g_noSaturate;
            g_flushShaders = true;
            Log("[diag] F7: limite de cor 0-1 %s", g_noSaturate ? "DESLIGADO" : "ligado");
            return true;
        case VK_F8:
            WriteReport("F8");
            return true;
        case VK_F9:
            RequestCapture();
            return true;
        case VK_F11:
            g_memDumpRequest = true;
            Log("[diag] F11: copiando a memoria do jogo no proximo quadro");
            return true;
        }
        return false;
    }

    void MaybeDumpMemory(uint64_t frame)
    {
        static const char* env = getenv("FCRECOMP_MEMDUMP");
        // FCRECOMP_MEMDUMP=quadro[,quadro...]; com "M" na frente os quadros contam do inicio da fase (ex.: M60,75,150)
        bool listed = false;
        if (env)
        {
            bool rel = env[0] == 'M';
            uint64_t start = ::g_missionStartFrame.load();
            for (const char* p = env + (rel ? 1 : 0); *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : "")
                if ((rel ? (start && frame == start + strtoull(p, nullptr, 10)) : frame == strtoull(p, nullptr, 10))) listed = true;
        }
        bool want = g_memDumpRequest.exchange(false) || listed;
        if (!want) return;
        std::string dir = "diag\\memoria_" + std::to_string(frame);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        // percorre os 4 GB do Xbox e salva as regioes que existem (memoria do jogo em ordem de endereco)
        size_t total = 0, files = 0;
        uint64_t addr = 0;
        while (addr < 0x100000000ull)
        {
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery(g_base + addr, &mi, sizeof(mi))) break;
            uint64_t start = (uint64_t)((uint8_t*)mi.BaseAddress - g_base), end = start + mi.RegionSize;
            // modo "M" (busca de variaveis): memoria virtual do jogo e uma copia da fisica (A0000000-BFFFFFFF)
            bool skip = env && env[0] == 'M' && start >= 0x80000000ull && !(start >= 0xA0000000ull && start < 0xC0000000ull);
            if (!skip && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && start < 0x100000000ull)
            {
                char name[64];
                snprintf(name, sizeof(name), "\\%08llX.bin", (unsigned long long)start);
                if (FILE* f = fopen((dir + name).c_str(), "wb"))
                {
                    fwrite(g_base + start, 1, (size_t)(std::min<uint64_t>(end, 0x100000000ull) - start), f);
                    fclose(f);
                    total += (size_t)(end - start);
                    files++;
                }
            }
            addr = end;
        }
        Log("[diag] memoria do jogo copiada para %s (%zu regioes, %zu MB)", dir.c_str(), files, total >> 20);
    }

    std::string Status()
    {
        std::string s;
        if (g_loopShaders) s += " [F5 laco]";
        if (g_noMips) s += " [F6 sem mips]";
        if (g_noSaturate) s += " [F7 sem limite]";
        if (g_captureFrame > g_frame) s += " [F9 capturando]";
        return s;
    }
}
