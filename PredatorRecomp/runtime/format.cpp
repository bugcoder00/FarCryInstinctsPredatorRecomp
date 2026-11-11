// sprintf / vsprintf / _vsnprintf do kernel, lendo os argumentos do jogo
#include "guest.h"
#include <functional>

// Formata usando um "leitor" de argumentos de 64 bits
static std::string Format(const char* fmt, const std::function<uint64_t()>& next)
{
    std::string out;
    for (const char* p = fmt; *p; p++)
    {
        if (*p != '%') { out += *p; continue; }
        if (p[1] == '%') { out += '%'; p++; continue; }

        std::string spec = "%";
        p++;
        while (*p && strchr("-+ #0", *p)) spec += *p++;
        if (*p == '*') { spec += std::to_string((int32_t)next()); p++; }
        else while (*p >= '0' && *p <= '9') spec += *p++;
        if (*p == '.')
        {
            spec += *p++;
            if (*p == '*') { spec += std::to_string((int32_t)next()); p++; }
            else while (*p >= '0' && *p <= '9') spec += *p++;
        }

        bool is64 = false, wide = false;
        for (;;)
        {
            if (*p == 'l') { if (p[1] == 'l') { is64 = true; p++; } p++; }
            else if (*p == 'h') p++;
            else if (*p == 'I' && p[1] == '6' && p[2] == '4') { is64 = true; p += 3; }
            else if (*p == 'w') { wide = true; p++; }
            else break;
        }

        char conv = *p;
        if (!conv) break;
        char buf[512];
        uint64_t v;
        switch (conv)
        {
        case 'd': case 'i':
            v = next();
            snprintf(buf, sizeof(buf), (spec + "lld").c_str(), is64 ? (long long)v : (long long)(int32_t)v);
            out += buf; break;
        case 'u': case 'x': case 'X': case 'o':
            v = next();
            snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), is64 ? (unsigned long long)v : (unsigned long long)(uint32_t)v);
            out += buf; break;
        case 'p':
            snprintf(buf, sizeof(buf), "%08X", (uint32_t)next());
            out += buf; break;
        case 'c':
            out += (char)next(); break;
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E':
        {
            v = next();
            double d; memcpy(&d, &v, 8);
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), d);
            out += buf; break;
        }
        case 's': case 'S':
        {
            uint32_t a = (uint32_t)next();
            std::string s;
            if (!a) s = "(null)";
            else if (wide || conv == 'S') { for (uint32_t i = a; R16(i); i += 2) s += (char)R16(i); }
            else s = (const char*)G(a);
            snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
            out += buf; break;
        }
        default:
            out += spec + conv; break;
        }
    }
    return out;
}

// argumento n da chamada (0 = r3)
static uint64_t RegArg(GuestCpu& ctx, uint32_t n)
{
    if (n < 8) return Gpr64(ctx, 3 + (int)n);   // r3-r10
    return R64(Gpr32(ctx, 1) + 0x50 + (n - 8) * 8);
}

static uint32_t Output(uint32_t dst, const std::string& s, uint32_t max = 0xFFFFFFFF)
{
    uint32_t n = (uint32_t)std::min<size_t>(s.size(), max);
    memcpy(G(dst), s.data(), n);
    if (n < max) W8(dst + n, 0);
    return n;
}

KFUNC(sprintf)
{
    uint32_t n = 2;
    std::string s = Format((const char*)G(Gpr32(ctx, 4)), [&] { return RegArg(ctx, n++); });
    static const bool logSprintf = getenv("FCRECOMP_LOGSPRINTF") != nullptr;
    if (logSprintf)
        Log("[sprintf] \"%s\" -> \"%s\" (lr=%08X)", (const char*)G(Gpr32(ctx, 4)), s.c_str(), (uint32_t)Lr(ctx));
    Ret(ctx, Output(Gpr32(ctx, 3), s));
}

KFUNC(vsprintf)
{
    uint32_t va = Gpr32(ctx, 5);
    std::string s = Format((const char*)G(Gpr32(ctx, 4)), [&] { uint64_t v = R64(va); va += 8; return v; });
    Ret(ctx, Output(Gpr32(ctx, 3), s));
}

KFUNC(_vsnprintf)
{
    uint32_t va = Gpr32(ctx, 6), count = Gpr32(ctx, 4);
    std::string s = Format((const char*)G(Gpr32(ctx, 5)), [&] { uint64_t v = R64(va); va += 8; return v; });
    uint32_t n = Output(Gpr32(ctx, 3), s, count);
    Ret(ctx, s.size() > count ? 0xFFFFFFFF : n);
}
