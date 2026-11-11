// Rede do Xbox (NetDll_*, XNet*) sobre a rede de verdade do PC: o modo SYSTEM LINK (LAN) do jogo.
// O jogo usa UDP comum (nao o VDP do Xbox): a busca de partidas manda 7 bytes por broadcast para a porta 1004 e as
// partidas respondem; os sockets ficam nas portas 1000 e 1005. Cabo ou Wi-Fi tanto faz: o broadcast sai por todos os
// adaptadores ligados (cada um com o seu endereco de broadcast, ex. 192.168.0.255).
// As funcoes NetDll_* recebem r3 = quem chamou; os argumentos de verdade comecam em r4. Enderecos (sockaddr_in) e
// conjuntos (fd_set) do Xbox estao em big-endian na memoria do jogo e sao traduzidos aqui.
//
// Teste com duas copias no mesmo PC: PREDATOR_LAN_IP=127.0.0.2 (e 127.0.0.3 na outra) faz a copia usar esse endereco
// (os sockets se ligam a ele e o broadcast vai para 127.0.0.1..127.0.0.8). FCRECOMP_SEMREDE=1 = sem rede (como antes).
//
// INTERNET sem VPN ("Entrar por IP"): quem entra escreve o IP do anfitriao no launcher (ou PREDATOR_ONLINE_HOST=ip[:porta]).
// O jogo e uma estrela (conferido com 3 copias: os convidados so trocam pacotes com o anfitriao, nunca entre si), entao
// basta o anfitriao ter as portas abertas (UPnP sozinho, upnp.cpp, ou a mao no roteador). O que muda aqui:
// - quem entra: a busca (broadcast para a 1004) tambem vai direto para o IP do anfitriao, saindo pelo socket da 1005
//   (assim o roteador de quem entra deixa a resposta voltar), junto com um "OLA" nosso. As portas 1000/1010 de quem
//   entra mandam um "FURO" para o anfitriao (abre o caminho no roteador dele), no maximo 1 por segundo enquanto falam.
// - o anfitriao: guarda quem mandou OLA (buscadores) e manda o anuncio da partida (1005) direto para cada um, pelo
//   socket da 1004 (o mesmo endereco para onde a busca foi). Responde ao OLA com um "ECO" (MAC dele e o IP do outro).
// - enderecos: o jogo manda o XNADDR (IP da rede local + MAC) dentro dos pacotes e acha o IP com XNetXnAddrToInAddr. Para
//   os jogadores da internet (que mandaram OLA/FURO/ECO) devolvemos o IP de onde os pacotes vieram de verdade, e as
//   portas que o roteador do outro trocou sao traduzidas nos dois sentidos (o jogo continua vendo 1000/1010).
//   Dois jogadores atras do mesmo roteador (mesmo IP de internet) ganham um endereco "virtual" 240.0.0.x para o jogo.
// - XNADDR.inaOnline = IP de internet deste PC (UPnP, ou PREDATOR_ONLINE_IP_PUBLICO); fixo depois da 1a consulta do jogo.
// Teste no mesmo PC: PREDATOR_SEMBROADCAST=1 (sem broadcast: so o caminho direto) e PREDATOR_NAT_TESTE=1 em quem entra
// (finge um roteador: as portas do jogo viram +300 e o IP local anunciado vira 192.168.77.x, que ninguem alcanca).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "objects.h"
#include "diag.h"
#include "opcoes.h"
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <random>
#include <thread>
#include <chrono>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace
{
    struct Adaptador { uint32_t ip, mascara; std::string nome; uint8_t mac[6]; bool wifi; };
    struct Sock { SOCKET s; bool udp; bool ligado; uint16_t porta = 0; };   // porta = a que o jogo pediu no bind

    std::once_flag g_inicio;
    bool g_semRede = false;
    bool g_teste = false;                // PREDATOR_LAN_IP (duas copias no mesmo PC)
    uint32_t g_ip = 0;                   // endereco que o jogo ve como seu (ordem normal: 192.168.0.10 = C0A8000A)
    uint8_t g_mac[6] = { 0x00, 0x22, 0x48, 0x01, 0x02, 0x03 };
    std::vector<Adaptador> g_adaptadores;
    std::mutex g_mx;
    std::unordered_map<uint32_t, Sock> g_socks;   // numero entregue ao jogo -> socket do Windows
    std::atomic<uint32_t> g_proximo{ 0x100 };
    thread_local uint32_t t_erro = 0;

    bool LanLog() { static const bool on = getenv("FCRECOMP_LANLOG") != nullptr; return on; }
    // FCRECOMP_LANLOG=2: tambem o conteudo dos primeiros pacotes (em hexadecimal)
    void LanDump(const char* o, uint32_t id, const void* p, int n)
    {
        static const bool on = getenv("FCRECOMP_LANLOG") && atoi(getenv("FCRECOMP_LANLOG")) >= 2;
        static std::atomic<int> total{ 0 };
        if (!on || n <= 0 || total++ > 150) return;
        std::string t;
        for (int i = 0; i < n && i < 512; i++) { char b[4]; snprintf(b, 4, "%02X", ((const uint8_t*)p)[i]); t += b; if (i % 4 == 3) t += ' '; }
        Log("[lan]   %s %X (%d): %s", o, id, n, t.c_str());
    }
    std::string Ip(uint32_t ip) { char b[20]; snprintf(b, sizeof(b), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255); return b; }

    void LerAdaptadores()
    {
        ULONG tam = 32 * 1024;
        std::vector<uint8_t> buf(tam);
        auto* lista = (IP_ADAPTER_ADDRESSES*)buf.data();
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, lista, &tam) == ERROR_BUFFER_OVERFLOW)
        {
            buf.resize(tam);
            lista = (IP_ADAPTER_ADDRESSES*)buf.data();
            if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_MULTICAST, nullptr, lista, &tam) != NO_ERROR) return;
        }
        // o adaptador com saida para a internet (gateway) vem primeiro: e o endereco que o jogo anuncia
        std::vector<Adaptador> comGateway, semGateway;
        for (auto* a = lista; a; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                Adaptador ad{};
                ad.ip = ntohl(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
                if ((ad.ip >> 16) == 0xA9FE) continue;   // 169.254.x.x: sem endereco de verdade
                ad.mascara = u->OnLinkPrefixLength ? ~0u << (32 - u->OnLinkPrefixLength) : 0xFFFFFF00u;
                char nome[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nome, sizeof(nome) - 1, nullptr, nullptr);
                ad.nome = nome;
                ad.wifi = a->IfType == IF_TYPE_IEEE80211;
                if (a->PhysicalAddressLength >= 6) memcpy(ad.mac, a->PhysicalAddress, 6);
                (a->FirstGatewayAddress ? comGateway : semGateway).push_back(ad);
            }
        }
        g_adaptadores = comGateway;
        g_adaptadores.insert(g_adaptadores.end(), semGateway.begin(), semGateway.end());
    }

    // ------------------------------------------------ internet (SYSTEM LINK sem VPN)
    bool g_semBroadcast = false;              // PREDATOR_SEMBROADCAST
    bool g_natTeste = false;                  // PREDATOR_NAT_TESTE
    uint32_t g_anfitriao = 0;                 // IP do anfitriao (quem entra pela internet); 0 = so rede local
    uint16_t g_portaBusca = 1004;             // porta de busca do anfitriao (ip:porta no launcher)
    std::atomic<uint32_t> g_ipPublico{ 0 };   // IP de internet deste PC (UPnP ou PREDATOR_ONLINE_IP_PUBLICO)
    std::atomic<bool> g_ipPublicoFixo{ false };   // veio da opcao, ou o jogo ja leu o XNADDR (nao muda mais)
    constexpr uint16_t NAT_DESLOCA = 300;     // PREDATOR_NAT_TESTE: porta do jogo + 300
    // IP da rede local que o jogo anuncia (no teste de roteador, um endereco que ninguem alcanca)
    uint32_t InaAnunciado() { return g_natTeste ? (0xC0A84D00u | (g_ip & 255)) : g_ip; }

    struct Par { uint8_t mac[6]; uint32_t ina = 0, real = 0, virt = 0; std::unordered_map<uint16_t, uint16_t> portas; };
    struct Buscador { uint32_t ip; uint16_t porta; uint64_t quando; };
    std::mutex g_mxPar;
    std::vector<Par> g_pares;                                              // jogadores da internet (por MAC)
    std::unordered_map<uint64_t, std::pair<size_t, uint16_t>> g_volta;    // (ip, porta de verdade) -> (par, porta do jogo)
    std::vector<Buscador> g_buscadores;                                    // quem procurou partida (anfitriao)
    std::unordered_map<SOCKET, uint64_t> g_ultimoFuro;
    uint64_t Chave(uint32_t ip, uint16_t p) { return (uint64_t)ip << 16 | p; }
    uint64_t Agora() { return GetTickCount64(); }

    // (com g_mxPar) registra de onde vem um jogador: MAC, IP local dele, IP:porta de verdade e a porta do jogo
    void Aprender(const uint8_t* mac, uint32_t ina, uint32_t ip, uint16_t porta, uint16_t logica, bool& novo)
    {
        size_t i = 0;
        while (i < g_pares.size() && memcmp(g_pares[i].mac, mac, 6)) i++;
        novo = i == g_pares.size();
        if (novo)
        {
            Par p;
            memcpy(p.mac, mac, 6);
            p.virt = ip;
            for (auto& o : g_pares) if (o.virt == ip) p.virt = 0xF0000000u | (uint32_t)(g_pares.size() + 1);   // mesmo IP de internet
            g_pares.push_back(std::move(p));
        }
        Par& p = g_pares[i];
        p.ina = ina;
        p.real = ip;
        const uint16_t l = logica ? logica : porta;
        p.portas[l] = porta;
        g_volta[Chave(ip, porta)] = { i, l };
    }
    // endereco que o jogo usa -> endereco de verdade
    void TraduzirSaida(sockaddr_in& sa)
    {
        const uint32_t ip = ntohl(sa.sin_addr.s_addr);
        std::lock_guard l(g_mxPar);
        for (auto& p : g_pares)
            if (p.virt == ip)
            {
                sa.sin_addr.s_addr = htonl(p.real);
                if (auto it = p.portas.find(ntohs(sa.sin_port)); it != p.portas.end()) sa.sin_port = htons(it->second);
                return;
            }
    }
    // endereco de verdade -> o que o jogo espera (o IP do XNetXnAddrToInAddr e a porta do jogo)
    void TraduzirEntrada(sockaddr_in& sa)
    {
        const uint32_t ip = ntohl(sa.sin_addr.s_addr);
        std::lock_guard l(g_mxPar);
        if (g_pares.empty()) return;
        if (auto it = g_volta.find(Chave(ip, ntohs(sa.sin_port))); it != g_volta.end())
        {
            sa.sin_addr.s_addr = htonl(g_pares[it->second.first].virt);
            sa.sin_port = htons(it->second.second);
            return;
        }
        const Par* so = nullptr;
        int n = 0;
        for (auto& p : g_pares) if (p.real == ip) { so = &p; n++; }
        if (n == 1) sa.sin_addr.s_addr = htonl(so->virt);
    }

    // pacotes do proprio runtime (o jogo nao ve): magico[4], tipo, MAC[6], IP local[4], porta do jogo[2]
    // (+ no ECO: IP de internet do anfitriao[4], IP e porta de quem recebe como o anfitriao viu[4+2])
    const uint8_t MAGICO[4] = { 0xFF, 'P', 'R', 'D' };
    enum : uint8_t { OLA = 1, ECO = 2, FURO = 3 };
    void Por32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
    void Por16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
    uint32_t Tirar32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
    uint16_t Tirar16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
    int Montar(uint8_t* b, uint8_t tipo, uint16_t logica)
    {
        memcpy(b, MAGICO, 4);
        b[4] = tipo;
        memcpy(b + 5, g_mac, 6);
        Por32(b + 11, InaAnunciado());
        Por16(b + 15, logica);
        return 17;
    }
    sockaddr_in Endereco(uint32_t ip, uint16_t porta)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(ip);
        a.sin_port = htons(porta);
        return a;
    }
    // socket UDP do jogo ligado a essa porta (1000, 1004, 1005, 1010)
    SOCKET SocketDaPorta(uint16_t porta)
    {
        std::lock_guard l(g_mx);
        for (auto& [id, k] : g_socks) if (k.udp && k.porta == porta) return k.s;
        return INVALID_SOCKET;
    }
    // "furo": um pacote para o anfitriao abre o caminho de volta no roteador de quem entra (no maximo 1 por segundo)
    void Furar(SOCKET s, uint16_t logica, const sockaddr_in& destino)
    {
        {
            std::lock_guard l(g_mxPar);
            uint64_t& t = g_ultimoFuro[s];
            if (Agora() - t < 1000) return;
            t = Agora();
        }
        uint8_t b[32];
        int n = Montar(b, FURO, logica);
        sendto(s, (const char*)b, n, 0, (const sockaddr*)&destino, sizeof(destino));
    }
    // quem entra: a busca vai tambem direto para o anfitriao (pelo socket da 1005, onde chega a resposta)
    void BuscarNoAnfitriao(SOCKET s, uint16_t logica, const char* dados, int len, int flags)
    {
        SOCKET s5 = SocketDaPorta(1005);
        uint16_t l5 = 1005;
        if (s5 == INVALID_SOCKET) { s5 = s; l5 = logica; }
        const sockaddr_in h = Endereco(g_anfitriao, g_portaBusca);
        uint8_t b[32];
        int n = Montar(b, OLA, l5);
        sendto(s5, (const char*)b, n, 0, (const sockaddr*)&h, sizeof(h));
        int r = sendto(s5, dados, len, flags, (const sockaddr*)&h, sizeof(h));
        for (uint16_t p : { (uint16_t)1000, (uint16_t)1010 })
            if (SOCKET sp = SocketDaPorta(p); sp != INVALID_SOCKET) Furar(sp, p, Endereco(g_anfitriao, p));
        static std::atomic<int> vezes{ 0 };
        if (vezes++ == 0) Log("[online] procurando partida no anfitriao %s:%u%s", Ip(g_anfitriao).c_str(), g_portaBusca, r < 0 ? " (o envio falhou)" : "");
    }
    // anfitriao: o anuncio da partida vai direto para quem procurou pela internet, saindo pela 1004
    void AnunciarAosBuscadores(SOCKET s, const char* dados, int len, int flags)
    {
        std::vector<Buscador> lista;
        {
            std::lock_guard l(g_mxPar);
            const uint64_t agora = Agora();
            std::erase_if(g_buscadores, [&](const Buscador& b) { return agora - b.quando > 30000; });
            lista = g_buscadores;
        }
        if (lista.empty()) return;
        SOCKET s4 = SocketDaPorta(1004);
        if (s4 == INVALID_SOCKET) s4 = s;
        for (auto& b : lista)
        {
            const sockaddr_in d = Endereco(b.ip, b.porta);
            sendto(s4, dados, len, flags, (const sockaddr*)&d, sizeof(d));
        }
    }
    // pacote nosso que chegou (true = era nosso: o jogo nao recebe)
    bool PacoteNosso(SOCKET s, const uint8_t* d, int n, const sockaddr_in& de)
    {
        if (n < 17 || memcmp(d, MAGICO, 4) != 0) return false;
        const uint32_t ip = ntohl(de.sin_addr.s_addr);
        const uint16_t porta = ntohs(de.sin_port);
        const uint8_t tipo = d[4];
        const uint8_t* mac = d + 5;
        const uint32_t ina = Tirar32(d + 11);
        const uint16_t logica = Tirar16(d + 15);
        bool novo = false;
        if (tipo == OLA || tipo == FURO)
        {
            {
                std::lock_guard l(g_mxPar);
                Aprender(mac, ina, ip, porta, logica, novo);
                if (tipo == OLA)
                {
                    auto it = std::find_if(g_buscadores.begin(), g_buscadores.end(), [&](const Buscador& b) { return b.ip == ip && b.porta == porta; });
                    if (it != g_buscadores.end()) it->quando = Agora();
                    else if (g_buscadores.size() < 32) g_buscadores.push_back({ ip, porta, Agora() });
                }
            }
            if (novo) Log("[online] jogador pela internet: %s:%u (na rede dele %s, MAC ..%02X%02X)", Ip(ip).c_str(), porta, Ip(ina).c_str(), mac[4], mac[5]);
            if (tipo == OLA)
            {
                uint8_t b[32];
                Montar(b, ECO, 1004);
                Por32(b + 17, g_ipPublico);
                Por32(b + 21, ip);
                Por16(b + 25, porta);
                sendto(s, (const char*)b, 27, 0, (const sockaddr*)&de, sizeof(de));
            }
        }
        else if (tipo == ECO && n >= 27)
        {
            {
                std::lock_guard l(g_mxPar);
                Aprender(mac, ina, ip, porta, logica, novo);
            }
            if (novo) Log("[online] o anfitriao %s respondeu (na rede dele %s); o seu IP na internet, visto por ele: %s:%u",
                          Ip(ip).c_str(), Ip(ina).c_str(), Ip(Tirar32(d + 21)).c_str(), Tirar16(d + 25));
        }
        return true;
    }

    // "ip[:porta]" ou "nome[:porta]" -> IP do anfitriao
    void LerAnfitriao(const std::string& texto)
    {
        std::string t = texto;
        while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
        while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
        if (t.empty()) return;
        uint16_t porta = 1004;
        if (size_t c = t.rfind(':'); c != std::string::npos) { porta = (uint16_t)atoi(t.c_str() + c + 1); t = t.substr(0, c); }
        addrinfo dica{}, *res = nullptr;
        dica.ai_family = AF_INET;
        dica.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(t.c_str(), nullptr, &dica, &res) != 0 || !res)
        {
            Log("[online] nao achei o anfitriao '%s' (IP ou nome errado, ou sem internet): so rede local", texto.c_str());
            return;
        }
        g_anfitriao = ntohl(((sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
        g_portaBusca = porta ? porta : 1004;
        freeaddrinfo(res);
        Log("[online] entrar por IP: anfitriao %s (%s), busca na porta %u", t.c_str(), Ip(g_anfitriao).c_str(), g_portaBusca);
    }
}

bool UpnpIpExterno(std::string& ip, std::string& erro, std::string* aviso);   // upnp.cpp
void UpnpAbrirPortas();
void UpnpFecharPortas();
bool UpnpPortasAbertas();

namespace
{
    // no teste no mesmo PC o UPnP so liga com PREDATOR_UPNP=1; PREDATOR_UPNP=0 desliga sempre
    bool UpnpLigado() { const char* u = getenv("PREDATOR_UPNP"); return u ? atoi(u) != 0 : !g_teste; }

    void ConfigurarInternet()
    {
        g_semBroadcast = getenv("PREDATOR_SEMBROADCAST") != nullptr;
        g_natTeste = g_teste && getenv("PREDATOR_NAT_TESTE") != nullptr;
        if (g_semBroadcast) Log("[online] PREDATOR_SEMBROADCAST: sem broadcast (so o caminho direto pelo IP)");
        if (g_natTeste) Log("[online] PREDATOR_NAT_TESTE: finge um roteador (portas +%u, IP local anunciado %s)", NAT_DESLOCA, Ip(InaAnunciado()).c_str());
        const char* e = getenv("PREDATOR_ONLINE_HOST");
        LerAnfitriao(e ? std::string(e) : OpcoesAtuais().modoTeste ? std::string() : OpcoesAtuais().ipAnfitriao);
        if (const char* p = getenv("PREDATOR_ONLINE_IP_PUBLICO"))
        {
            in_addr a{};
            if (inet_pton(AF_INET, p, &a) == 1) { g_ipPublico = ntohl(a.s_addr); g_ipPublicoFixo = true; Log("[online] IP de internet deste PC (opcao): %s", p); }
        }
        // IP de internet pelo roteador, em segundo plano (vai no XNADDR.inaOnline; os amigos precisam dele)
        if (!g_ipPublicoFixo && UpnpLigado())
            std::thread([] {
                std::string ip, erro, aviso;
                if (!UpnpIpExterno(ip, erro, &aviso)) { Log("[upnp] IP de internet: %s", erro.c_str()); return; }
                in_addr a{};
                inet_pton(AF_INET, ip.c_str(), &a);
                if (!g_ipPublicoFixo) g_ipPublico = ntohl(a.s_addr);
                Log("[upnp] IP de internet deste PC: %s", ip.c_str());
                if (!aviso.empty()) Log("[upnp] ATENCAO: %s", aviso.c_str());
            }).detach();
        // o jogo foi reaberto (troca de fase) com as portas abertas: continua com elas (e fecha ao sair)
        if (getenv("PREDATOR_UPNP_ABERTO") && UpnpLigado()) UpnpAbrirPortas();
    }

    void Iniciar()
    {
        std::call_once(g_inicio, [] {
            g_semRede = getenv("FCRECOMP_SEMREDE") != nullptr;
            WSADATA w;
            if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { g_semRede = true; Log("[lan] Winsock nao abriu: sem rede"); return; }
            if (const char* t = getenv("PREDATOR_LAN_IP"))
            {
                unsigned a, b, c, d;
                if (sscanf(t, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
                {
                    g_teste = true;
                    g_ip = a << 24 | b << 16 | c << 8 | d;
                    g_mac[5] = (uint8_t)d;
                    Log("[lan] modo de teste no mesmo PC: endereco %s", Ip(g_ip).c_str());
                    ConfigurarInternet();
                    return;
                }
            }
            LerAdaptadores();
            for (auto& a : g_adaptadores)
                Log("[lan] adaptador %s (%s): %s / %s", a.nome.c_str(), a.wifi ? "Wi-Fi" : "cabo ou outro", Ip(a.ip).c_str(), Ip(a.mascara).c_str());
            if (g_adaptadores.empty()) Log("[lan] nenhuma rede ligada (cabo ou Wi-Fi): SYSTEM LINK desligado");
            else { g_ip = g_adaptadores[0].ip; memcpy(g_mac, g_adaptadores[0].mac, 6); Log("[lan] o jogo usa o endereco %s", Ip(g_ip).c_str()); }
            if (g_semRede) Log("[lan] FCRECOMP_SEMREDE: rede desligada");
            else if (g_ip) ConfigurarInternet();
        });
    }
    bool RedeLigada() { Iniciar(); return !g_semRede && g_ip != 0; }

    SOCKET Host(uint32_t s, Sock** out = nullptr)
    {
        std::lock_guard l(g_mx);
        auto it = g_socks.find(s);
        if (it == g_socks.end()) return INVALID_SOCKET;
        if (out) *out = &it->second;
        return it->second.s;
    }
    // sockaddr_in do Xbox: familia (u16), porta (u16), endereco (u32), tudo big-endian
    bool LerEndereco(uint32_t a, sockaddr_in& sa)
    {
        if (!a) return false;
        sa = {};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(R16(a + 2));
        sa.sin_addr.s_addr = htonl(R32(a + 4));
        return true;
    }
    void EscreverEndereco(uint32_t a, uint32_t lenPtr, const sockaddr_in& sa)
    {
        if (!a) return;
        W16(a, AF_INET);
        W16(a + 2, ntohs(sa.sin_port));
        W32(a + 4, ntohl(sa.sin_addr.s_addr));
        memset(G(a + 8), 0, 8);
        if (lenPtr) W32(lenPtr, 16);
    }
    // teste no mesmo PC: o socket precisa sair pelo endereco desta copia (senao os outros veem 127.0.0.1)
    void LigarNoTeste(Sock* k)
    {
        if (!g_teste || !k || k->ligado) return;
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(g_ip);
        bind(k->s, (sockaddr*)&sa, sizeof(sa));
        k->ligado = true;
    }
    uint32_t Falhou() { t_erro = (uint32_t)WSAGetLastError(); return 0xFFFFFFFF; }
}

// ao fechar o jogo (gpu.cpp, xam.cpp, kernel.cpp, main.cpp): fecha as portas abertas no roteador
void RedeEncerrar() { UpnpFecharPortas(); }
// troca de fase (xam.cpp): o processo novo continua com as portas abertas e as fecha quando sair
bool RedeReabrirComPortas() { return UpnpPortasAbertas(); }

// usado por content.cpp: numero do jogador diferente em cada PC (e em cada copia do teste)
uint64_t RedeXuidLocal()
{
    Iniciar();
    static const uint64_t xuid = [] {
        uint64_t h = 0xCBF29CE484222325ull;
        auto mistura = [&](const void* p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t*)p)[i]; h *= 0x100000001B3ull; } };
        char nome[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD n = sizeof(nome);
        GetComputerNameA(nome, &n);
        mistura(nome, n);
        if (g_teste) mistura(&g_ip, 4);
        return 0xE000000000000000ull | (h & 0x0000FFFFFFFFFFF0ull);   // "offline" (comeca com E); +1 = jogador 2
    }();
    return xuid;
}
// nome do jogador na rede: o nome do usuario do Windows (como a gamertag, ate 15 letras)
std::string RedeNomeLocal()
{
    if (const char* n = getenv("PREDATOR_LAN_NOME")) return std::string(n).substr(0, 15);
    char nome[64] = {};
    DWORD n = sizeof(nome);
    if (!GetUserNameA(nome, &n) || !nome[0]) return "Jogador";
    std::string s;
    for (char* p = nome; *p && s.size() < 15; p++) if ((unsigned char)*p >= 32 && (unsigned char)*p < 127) s += *p;
    return s.empty() ? "Jogador" : s;
}

// ---------------------------------------------------------------- Winsock

// NetDll_WSAStartup(quem chamou, versao pedida, WSADATA*): o jogo confere wVersion == versao pedida
KFUNC(NetDll_WSAStartup)
{
    Iniciar();
    uint32_t data = Gpr32(ctx, 5);
    if (data)
    {
        memset(G(data), 0, 0x190);
        W16(data + 0, (uint16_t)Gpr32(ctx, 4));   // wVersion
        W16(data + 2, 0x0202);                 // wHighVersion
    }
    Ret(ctx, 0);
}
KFUNC(NetDll_WSACleanup) { Ret(ctx, 0); }
KFUNC(NetDll_WSAGetLastError) { Ret(ctx, t_erro); }

// socket(quem, familia, tipo, protocolo)
KFUNC(NetDll_socket)
{
    int fam = (int)Gpr32(ctx, 4), tipo = (int)Gpr32(ctx, 5), proto = (int)Gpr32(ctx, 6);
    if (!RedeLigada()) { Ret(ctx, g_proximo += 4); return; }   // sem rede: socket falso (nada chega)
    if (proto == 254) proto = IPPROTO_UDP;   // VDP do Xbox (voz + dados): aqui UDP comum
    SOCKET s = socket(fam, tipo, proto);
    if (s == INVALID_SOCKET) { Ret(ctx, Falhou()); return; }
    uint32_t id = g_proximo += 4;
    {
        std::lock_guard l(g_mx);
        g_socks[id] = { s, tipo == SOCK_DGRAM, false };
    }
    if (LanLog()) Log("[lan] socket(%d, %d, %d) = %X", fam, tipo, proto, id);
    Ret(ctx, id);
}

KFUNC(NetDll_closesocket)
{
    SOCKET s = INVALID_SOCKET;
    {
        std::lock_guard l(g_mx);
        if (auto it = g_socks.find(Gpr32(ctx, 4)); it != g_socks.end()) { s = it->second.s; g_socks.erase(it); }
    }
    if (s != INVALID_SOCKET) closesocket(s);
    Ret(ctx, 0);
}

KFUNC(NetDll_shutdown)
{
    SOCKET s = Host(Gpr32(ctx, 4));
    if (s == INVALID_SOCKET) { Ret(ctx, 0); return; }
    Ret(ctx, shutdown(s, (int)Gpr32(ctx, 5)) == 0 ? 0 : Falhou());
}

// bind(quem, s, endereco, tamanho)
KFUNC(NetDll_bind)
{
    Sock* k = nullptr;
    SOCKET s = Host(Gpr32(ctx, 4), &k);
    if (s == INVALID_SOCKET) { Ret(ctx, 0); return; }
    sockaddr_in sa;
    if (!LerEndereco(Gpr32(ctx, 5), sa)) { t_erro = WSAEFAULT; Ret(ctx, 0xFFFFFFFF); return; }
    if (g_teste && sa.sin_addr.s_addr == INADDR_ANY) sa.sin_addr.s_addr = htonl(g_ip);
    const uint16_t pedida = ntohs(sa.sin_port);
    if (g_natTeste && pedida) sa.sin_port = htons(pedida + NAT_DESLOCA);   // teste de roteador: a porta "de fora" e outra
    int r = bind(s, (sockaddr*)&sa, sizeof(sa));
    if (pedida != ntohs(sa.sin_port)) Log("[lan] bind %X na porta %u (de verdade %u): %s", Gpr32(ctx, 4), pedida, ntohs(sa.sin_port), r == 0 ? "ok" : "falhou");
    else Log("[lan] bind %X na porta %u: %s", Gpr32(ctx, 4), pedida, r == 0 ? "ok" : "falhou");
    if (r != 0) { Ret(ctx, Falhou()); return; }
    k->ligado = true;
    k->porta = pedida;
    Ret(ctx, 0);
}

KFUNC(NetDll_listen)
{
    SOCKET s = Host(Gpr32(ctx, 4));
    if (s == INVALID_SOCKET) { Ret(ctx, 0); return; }
    Ret(ctx, listen(s, (int)Gpr32(ctx, 5)) == 0 ? 0 : Falhou());
}

// setsockopt(quem, s, nivel, opcao, valor*, tamanho): mesmos numeros do Winsock; valores inteiros em big-endian
KFUNC(NetDll_setsockopt)
{
    SOCKET s = Host(Gpr32(ctx, 4));
    int nivel = (int)Gpr32(ctx, 5), opcao = (int)Gpr32(ctx, 6);
    uint32_t val = Gpr32(ctx, 7), tam = Gpr32(ctx, 8);
    if (s == INVALID_SOCKET) { Ret(ctx, 0); return; }
    if (nivel == SOL_SOCKET && opcao == SO_LINGER && val && tam >= 4)
    {
        linger lg{ R16(val), R16(val + 2) };
        setsockopt(s, nivel, opcao, (char*)&lg, sizeof(lg));
    }
    else if (val && tam == 4)
    {
        int v = (int)R32(val);
        if (setsockopt(s, nivel, opcao, (char*)&v, 4) != 0 && LanLog())
            Log("[lan] setsockopt %X nivel %X opcao %X = %d: o Windows nao aceitou (ignorado)", Gpr32(ctx, 4), nivel, opcao, v);
    }
    else if (val && tam == 1)
    {
        char v = (char)*G(val);
        setsockopt(s, nivel, opcao, &v, 1);
    }
    Ret(ctx, 0);   // opcoes so do Xbox (seguranca, VDP...) sao aceitas e ignoradas
}

// ioctlsocket(quem, s, comando, argumento*)
KFUNC(NetDll_ioctlsocket)
{
    SOCKET s = Host(Gpr32(ctx, 4));
    uint32_t cmd = Gpr32(ctx, 5), arg = Gpr32(ctx, 6);
    if (s == INVALID_SOCKET) { Ret(ctx, 0); return; }
    u_long v = arg ? R32(arg) : 0;
    if (ioctlsocket(s, (long)cmd, &v) != 0) { Ret(ctx, Falhou()); return; }
    if (arg) W32(arg, (uint32_t)v);
    Ret(ctx, 0);
}

// connect(quem, s, endereco, tamanho)
KFUNC(NetDll_connect)
{
    Sock* k = nullptr;
    SOCKET s = Host(Gpr32(ctx, 4), &k);
    sockaddr_in sa;
    if (s == INVALID_SOCKET) { t_erro = WSAECONNREFUSED; Ret(ctx, 0xFFFFFFFF); return; }
    if (!LerEndereco(Gpr32(ctx, 5), sa)) { t_erro = WSAEFAULT; Ret(ctx, 0xFFFFFFFF); return; }
    LigarNoTeste(k);
    if (LanLog()) Log("[lan] connect %X -> %s:%u", Gpr32(ctx, 4), Ip(ntohl(sa.sin_addr.s_addr)).c_str(), ntohs(sa.sin_port));
    Ret(ctx, connect(s, (sockaddr*)&sa, sizeof(sa)) == 0 ? 0 : Falhou());
}

// accept(quem, s, endereco*, tamanho*)
KFUNC(NetDll_accept)
{
    SOCKET s = Host(Gpr32(ctx, 4));
    if (s == INVALID_SOCKET) { t_erro = WSAEWOULDBLOCK; Ret(ctx, 0xFFFFFFFF); return; }
    sockaddr_in sa{};
    int len = sizeof(sa);
    SOCKET n = accept(s, (sockaddr*)&sa, &len);
    if (n == INVALID_SOCKET) { Ret(ctx, Falhou()); return; }
    uint32_t id = g_proximo += 4;
    {
        std::lock_guard l(g_mx);
        g_socks[id] = { n, false, true };
    }
    EscreverEndereco(Gpr32(ctx, 5), Gpr32(ctx, 6), sa);
    Log("[lan] conexao aceita de %s", Ip(ntohl(sa.sin_addr.s_addr)).c_str());
    Ret(ctx, id);
}

// send(quem, s, dados, tamanho, flags) / recv(quem, s, buffer, tamanho, flags)
KFUNC(NetDll_send)
{
    SOCKET s = Host(Gpr32(ctx, 4));
    if (s == INVALID_SOCKET) { Ret(ctx, Gpr32(ctx, 6)); return; }   // sem rede: finge que enviou tudo
    int r = send(s, (const char*)G(Gpr32(ctx, 5)), (int)Gpr32(ctx, 6), (int)Gpr32(ctx, 7));
    Ret(ctx, r >= 0 ? (uint32_t)r : Falhou());
}
KFUNC(NetDll_recv)
{
    SOCKET s = Host(Gpr32(ctx, 4));
    if (s == INVALID_SOCKET) { t_erro = WSAEWOULDBLOCK; Ret(ctx, 0xFFFFFFFF); return; }
    int r = recv(s, (char*)G(Gpr32(ctx, 5)), (int)Gpr32(ctx, 6), (int)Gpr32(ctx, 7));
    Ret(ctx, r >= 0 ? (uint32_t)r : Falhou());
}

// sendto(quem, s, dados, tamanho, flags, destino, tamanho do destino)
KFUNC(NetDll_sendto)
{
    Sock* k = nullptr;
    SOCKET s = Host(Gpr32(ctx, 4), &k);
    const uint32_t len = Gpr32(ctx, 6);
    if (s == INVALID_SOCKET) { Ret(ctx, len); return; }
    sockaddr_in sa;
    if (!LerEndereco(Gpr32(ctx, 8), sa)) { t_erro = WSAEDESTADDRREQ; Ret(ctx, 0xFFFFFFFF); return; }
    LigarNoTeste(k);
    const char* dados = (const char*)G(Gpr32(ctx, 5));
    const int flags = (int)Gpr32(ctx, 7);
    uint32_t dest = ntohl(sa.sin_addr.s_addr);
    if (LanLog()) { static int n = 0; if (n++ < 200) Log("[lan] sendto %X: %u bytes para %s:%u", Gpr32(ctx, 4), len, Ip(dest).c_str(), ntohs(sa.sin_port)); }
    const uint16_t minhaPorta = k ? k->porta : 0;
    if (dest == 0xFFFFFFFF)
    {
        LanDump("enviado", Gpr32(ctx, 4), dados, (int)len);
        const uint16_t porta = ntohs(sa.sin_port);
        // internet: a busca vai tambem direto para o anfitriao; o anuncio, para quem procurou pela internet
        if (g_anfitriao && porta == 1004) BuscarNoAnfitriao(s, minhaPorta, dados, (int)len, flags);
        if (porta == 1005) AnunciarAosBuscadores(s, dados, (int)len, flags);
        if (g_semBroadcast) { Ret(ctx, len); return; }
        // broadcast: por todos os adaptadores (cabo e Wi-Fi), ou para as copias do teste no mesmo PC
        std::vector<uint32_t> alvos;
        if (g_teste) for (uint32_t i = 1; i <= 8; i++) alvos.push_back(0x7F000000u | i);
        else for (auto& a : g_adaptadores) alvos.push_back(a.ip | ~a.mascara);
        bool algum = false;
        for (uint32_t ip : alvos)
        {
            sockaddr_in b = sa;
            b.sin_addr.s_addr = htonl(ip);
            algum |= sendto(s, dados, (int)len, flags, (sockaddr*)&b, sizeof(b)) >= 0;
        }
        Ret(ctx, algum ? len : Falhou());
        return;
    }
    LanDump("enviado", Gpr32(ctx, 4), dados, (int)len);
    TraduzirSaida(sa);   // jogador da internet: IP e porta de verdade
    if (g_anfitriao && ntohl(sa.sin_addr.s_addr) == g_anfitriao) Furar(s, minhaPorta, sa);
    int r = sendto(s, dados, (int)len, flags, (sockaddr*)&sa, sizeof(sa));
    Ret(ctx, r >= 0 ? (uint32_t)r : Falhou());
}

// recvfrom(quem, s, buffer, tamanho, flags, origem*, tamanho da origem*)
KFUNC(NetDll_recvfrom)
{
    SOCKET s = Host(Gpr32(ctx, 4));
    if (s == INVALID_SOCKET) { t_erro = WSAEWOULDBLOCK; Ret(ctx, 0xFFFFFFFF); return; }
    sockaddr_in sa{};
    int salen = sizeof(sa);
    int r = recvfrom(s, (char*)G(Gpr32(ctx, 5)), (int)Gpr32(ctx, 6), (int)Gpr32(ctx, 7), (sockaddr*)&sa, &salen);
    if (r < 0)
    {
        // UDP no Windows: um destino que nao respondeu volta como "conexao recusada" no proximo recvfrom; no Xbox nao
        if (WSAGetLastError() == WSAECONNRESET) { t_erro = WSAEWOULDBLOCK; Ret(ctx, 0xFFFFFFFF); return; }
        Ret(ctx, Falhou());
        return;
    }
    // OLA/ECO/FURO do runtime: o jogo nao ve (como se nada tivesse chegado)
    if (PacoteNosso(s, G(Gpr32(ctx, 5)), r, sa)) { t_erro = WSAEWOULDBLOCK; Ret(ctx, 0xFFFFFFFF); return; }
    TraduzirEntrada(sa);
    EscreverEndereco(Gpr32(ctx, 8), Gpr32(ctx, 9), sa);
    LanDump("recebido", Gpr32(ctx, 4), G(Gpr32(ctx, 5)), r);
    if (LanLog()) { static int n = 0; if (n++ < 200) Log("[lan] recvfrom %X: %d bytes de %s:%u", Gpr32(ctx, 4), r, Ip(ntohl(sa.sin_addr.s_addr)).c_str(), ntohs(sa.sin_port)); }
    Ret(ctx, (uint32_t)r);
}

// select(quem, nfds, leitura, escrita, erro, tempo): fd_set do Xbox = u32 quantidade + u32 sockets[64]
KFUNC(NetDll_select)
{
    const uint32_t conj[3] = { Gpr32(ctx, 5), Gpr32(ctx, 6), Gpr32(ctx, 7) };
    const uint32_t tv = Gpr32(ctx, 8);
    fd_set hs[3];
    std::vector<std::pair<uint32_t, SOCKET>> lista[3];
    int total = 0;
    for (int i = 0; i < 3; i++)
    {
        FD_ZERO(&hs[i]);
        if (!conj[i]) continue;
        uint32_t n = std::min<uint32_t>(R32(conj[i]), 64);
        for (uint32_t k = 0; k < n; k++)
        {
            uint32_t id = R32(conj[i] + 4 + k * 4);
            SOCKET s = Host(id);
            if (s == INVALID_SOCKET) continue;
            FD_SET(s, &hs[i]);
            lista[i].push_back({ id, s });
            total++;
        }
    }
    const uint64_t us = tv ? (uint64_t)R32(tv) * 1000000ull + R32(tv + 4) : 100000ull;
    if (total == 0)
    {
        // nenhum socket de verdade (sem rede): como no console sem rede, espera o tempo pedido e diz "nada chegou".
        // Respondendo na hora, a thread de rede do jogo ficava em laco (~37 mil chamadas por segundo).
        for (uint32_t p : conj) if (p) W32(p, 0);
        static std::atomic<bool> logged{ false };
        if (!logged.exchange(true)) Log("[rede] select com espera de %llu us%s", (unsigned long long)us, tv ? "" : " (sem limite)");
        if (us) std::this_thread::sleep_for(std::chrono::microseconds(std::min<uint64_t>(us, 100000)));
        Ret(ctx, 0);
        return;
    }
    timeval t{ (long)(us / 1000000), (long)(us % 1000000) };
    int r = select(0, conj[0] ? &hs[0] : nullptr, conj[1] ? &hs[1] : nullptr, conj[2] ? &hs[2] : nullptr, tv ? &t : nullptr);
    if (r < 0) { Ret(ctx, Falhou()); return; }
    for (int i = 0; i < 3; i++)
    {
        if (!conj[i]) continue;
        uint32_t n = 0;
        for (auto& [id, s] : lista[i]) if (FD_ISSET(s, &hs[i])) W32(conj[i] + 4 + 4 * n++, id);
        W32(conj[i], n);
    }
    Ret(ctx, (uint32_t)r);
}

KFUNC(NetDll_inet_addr)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    // ordem de rede = big-endian = ordem nativa do PowerPC (sem inverter os bytes)
    if (sscanf(RStr(Gpr32(ctx, 3)).c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
        Ret(ctx, (a << 24) | (b << 16) | (c << 8) | d);
    else
        Ret(ctx, 0xFFFFFFFF);
}

// ---------------------------------------------------------------- XNet (enderecos do Xbox)

KFUNC(NetDll_XNetStartup) { Iniciar(); Ret(ctx, 0); }
KFUNC(NetDll_XNetCleanup) { Ret(ctx, 0); }
// o menu SYSTEM LINK so acende com "cabo ligado": qualquer rede ligada (cabo ou Wi-Fi) conta
KFUNC(NetDll_XNetGetEthernetLinkStatus) { Ret(ctx, RedeLigada() ? 0x01 | 0x04 | 0x10 : 0); }   // ativo, 100 Mbps, full duplex

// XNADDR { ina; inaOnline; u16 porta; u8 mac[6]; u8 online[20] }
KFUNC(NetDll_XNetGetTitleXnAddr)
{
    uint32_t a = Gpr32(ctx, 4);
    Iniciar();
    if (LanLog()) { static int n = 0; if (n++ < 20) Log("[lan] XNetGetTitleXnAddr"); }
    // o IP de internet fica fixo a partir daqui (o jogo pode comparar copias do proprio XNADDR)
    g_ipPublicoFixo = true;
    if (a)
    {
        memset(G(a), 0, 36);
        W32(a, RedeLigada() ? InaAnunciado() : 0x7F000001);
        W32(a + 4, RedeLigada() ? g_ipPublico.load() : 0);
        memcpy(G(a + 10), g_mac, 6);
    }
    Ret(ctx, RedeLigada() ? 0x02 | 0x04 : 0x02 | 0x04);   // XNET_GET_XNADDR_ETHERNET | STATIC
}

// XNetXnAddrToInAddr(quem, XNADDR*, XNKID*, IN_ADDR*): o IP para falar com esse jogador. Na rede local e o IP do
// XNADDR; jogador da internet (mandou OLA/FURO/ECO, achado pelo MAC): o IP de onde os pacotes dele vem.
KFUNC(NetDll_XNetXnAddrToInAddr)
{
    uint32_t xna = Gpr32(ctx, 4), ina = Gpr32(ctx, 6);
    if (!xna || !ina) { Ret(ctx, 0x80070057); return; }   // E_INVALIDARG
    const uint32_t local = R32(xna), online = R32(xna + 4);
    const uint8_t* mac = G(xna + 10);
    uint32_t ip = local;
    const char* como = "rede local";
    if (!memcmp(mac, g_mac, 6)) { ip = g_ip; como = "este PC"; }
    else
    {
        std::lock_guard l(g_mxPar);
        auto it = std::find_if(g_pares.begin(), g_pares.end(), [&](const Par& p) { return !memcmp(p.mac, mac, 6); });
        if (it != g_pares.end()) { ip = it->virt; como = "internet"; }
        else if (g_anfitriao && online) { ip = online; como = "internet (IP do XNADDR)"; }
    }
    W32(ina, ip);
    if (LanLog() || (ip != local && strcmp(como, "este PC")))
    {
        static std::atomic<int> n{ 0 };
        if (n++ < 40) Log("[lan] XNetXnAddrToInAddr %s (MAC ..%02X%02X) -> %s (%s)", Ip(local).c_str(), mac[4], mac[5], Ip(ip).c_str(), como);
    }
    Ret(ctx, 0);
}
KFUNC(NetDll_XNetUnregisterInAddr) { Ret(ctx, 0); }

// XNetInAddrToString(quem, IN_ADDR, texto, tamanho)
KFUNC(NetDll_XNetInAddrToString)
{
    std::string s = Ip(Gpr32(ctx, 4));
    uint32_t buf = Gpr32(ctx, 5), len = Gpr32(ctx, 6);
    if (buf && len)
    {
        uint32_t n = std::min<uint32_t>(len - 1, (uint32_t)s.size());
        memcpy(G(buf), s.data(), n);
        W8(buf + n, 0);
    }
    Ret(ctx, 0);
}

// XNetRandom(quem, buffer, tamanho)
KFUNC(NetDll_XNetRandom)
{
    static std::mutex m;
    static std::mt19937_64 rng{ std::random_device{}() ^ (uint64_t)GetTickCount64() };
    std::lock_guard l(m);
    uint32_t buf = Gpr32(ctx, 4), len = Gpr32(ctx, 5);
    for (uint32_t i = 0; i < len; i++) W8(buf + i, (uint8_t)rng());
    Ret(ctx, 0);
}

// ---------------------------------------------------------------- sessoes de partida (XSession)
// Criar/entrar/iniciar/terminar partida vai do jogo para o sistema por mensagens (XMsgStartIORequest, app 0xFB,
// mensagens 0x000B00xx). Sem Xbox Live, no SYSTEM LINK o sistema so precisa dizer "ok" e, na criacao, preencher a
// identidade da partida (XSESSION_INFO: id da sessao, endereco do anfitriao, chave): o resto (anuncio, lista, entrada)
// o proprio jogo faz pela rede (portas 1000-1010). FCRECOMP_LANLOG=1 anota cada mensagem.



// XamSessionCreateHandle(handle*) / XamSessionRefObjByHandle(handle, objeto*)
namespace { std::unordered_map<uint32_t, uint32_t> g_sessoes; std::atomic<uint32_t> g_proxSessao{ 0xFB000004 }; }
KFUNC(XamSessionCreateHandle)
{
    uint32_t h = g_proxSessao += 4;
    uint32_t obj = GuestAlloc(0x100);
    memset(G(obj), 0, 0x100);
    { std::lock_guard l(g_mx); g_sessoes[h] = obj; }
    if (Gpr32(ctx, 3)) W32(Gpr32(ctx, 3), h);
    if (LanLog()) Log("[lan] sessao criada: handle %08X objeto %08X", h, obj);
    Ret(ctx, 0);
}
KFUNC(XamSessionRefObjByHandle)
{
    uint32_t obj = 0;
    { std::lock_guard l(g_mx); if (auto it = g_sessoes.find(Gpr32(ctx, 3)); it != g_sessoes.end()) obj = it->second; }
    if (Gpr32(ctx, 4)) W32(Gpr32(ctx, 4), obj);
    Ret(ctx, obj ? 0 : 0xC0000008);   // STATUS_INVALID_HANDLE
}

bool RedeXMsg(const char* fn, uint32_t app, uint32_t msg, uint32_t buf, uint32_t tam, uint32_t& resultado)
{
    if (LanLog())
    {
        char dump[100] = {};
        for (uint32_t i = 0; buf && i < std::min<uint32_t>(tam, 32); i += 4) snprintf(dump + strlen(dump), sizeof(dump) - strlen(dump), "%08X ", R32(buf + i));
        Log("[lan] %s app %02X mensagem %08X buffer %08X (%u bytes): %s", fn, app, msg, buf, tam, dump);
    }
    if (app != 0xFB || (msg >> 16) != 0x000B || !RedeLigada()) return false;
    resultado = 0;
    if (msg == 0x000B0010 && buf && tam >= 28)
    {
        // XSessionCreate { objeto, flags, vagas publicas, vagas privadas, usuario, XSESSION_INFO*, nonce* }
        uint32_t info = R32(buf + 0x14), nonce = R32(buf + 0x18);
        static std::mt19937_64 rng{ std::random_device{}() ^ (uint64_t)GetTickCount64() };
        const bool anfitriao = R32(buf + 4) & 1;   // XSESSION_CREATE_HOST; sem ele (quem entra) o XSESSION_INFO e do anfitriao
        if (info && !anfitriao) Log("[lan] entrando na partida de %s%s", Ip(R32(info + 8)).c_str(), g_anfitriao ? " (pela internet)" : "");
        if (info && anfitriao)
        {
            // XSESSION_INFO { XNKID id[8]; XNADDR anfitriao[36]; XNKEY chave[16] }; id do SYSTEM LINK: 4 bits altos 0
            uint64_t id = rng() & 0x0FFFFFFFFFFFFFFFull;
            W64(info, id);
            g_ipPublicoFixo = true;
            memset(G(info + 8), 0, 36);
            W32(info + 8, InaAnunciado());
            W32(info + 8 + 4, g_ipPublico.load());   // inaOnline: IP de internet (se ja se sabe)
            memcpy(G(info + 8 + 10), g_mac, 6);
            for (uint32_t i = 0; i < 16; i++) W8(info + 44 + i, (uint8_t)rng());
            Log("[lan] partida criada: flags %08X, %u vagas, anfitriao %s", R32(buf + 4), R32(buf + 8) + R32(buf + 12), Ip(g_ip).c_str());
            // anfitriao: abre as portas do jogo no roteador (para os amigos entrarem pela internet)
            if (UpnpLigado()) UpnpAbrirPortas();
            if (g_ipPublico) Log("[online] para os amigos entrarem pela internet: IP %s (eles escrevem no launcher, em \"IP do anfitriao\")", Ip(g_ipPublico).c_str());
        }
        if (nonce) W64(nonce, rng());
    }
    return true;
}

// ---------------------------------------------------------------- QoS (ping das partidas na lista) e voz

void SignalEvent(uint32_t evHandle);   // fs.cpp

// XNetQosListen(quem, XNKID*, dados, tamanho, bits/s, flags): o anfitriao responde a medicoes; aqui nada a fazer
KFUNC(NetDll_XNetQosListen) { Ret(ctx, 0); }

// XNetQosLookup(quem, n_xnaddr, XNADDR*[], XNKID*[], XNKEY*[], n_ina, IN_ADDR[], servicos[], | pilha: medicoes,
// bits/s, flags, evento, XNQOS**). Na LAN a resposta e imediata: todos alcancados, ~5 ms, 100 Mbps, sem dados extras.
// XNQOS { u32 total; u32 pendentes; XNQOSINFO[24 bytes] { u8 flags; u8; u16 enviadas; u16 recebidas; u16 tam_dados;
// u32 dados; u16 rtt_min; u16 rtt_med; u32 subida; u32 descida } }
KFUNC(NetDll_XNetQosLookup)
{
    const uint32_t sp = Gpr32(ctx, 1);
    const uint32_t n = Gpr32(ctx, 4) + Gpr32(ctx, 8);
    const uint32_t medicoes = R32(sp + 0x54), evento = R32(sp + 0x6C), saida = R32(sp + 0x74);
    uint32_t q = GuestAlloc(8 + std::max<uint32_t>(n, 1) * 24);
    memset(G(q), 0, 8 + std::max<uint32_t>(n, 1) * 24);
    W32(q, n);
    W32(q + 4, 0);
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t e = q + 8 + i * 24;
        W8(e + 0, 0x01 | 0x02);   // COMPLETE | TARGET_CONTACTED
        W16(e + 2, (uint16_t)std::max<uint32_t>(medicoes, 1));
        W16(e + 4, (uint16_t)std::max<uint32_t>(medicoes, 1));
        W16(e + 12, 2);           // rtt minimo (ms)
        W16(e + 14, 5);           // rtt mediano (ms)
        W32(e + 16, 100000000);
        W32(e + 20, 100000000);
    }
    if (saida) W32(saida, q);
    if (evento) SignalEvent(evento);
    if (LanLog()) Log("[lan] XNetQosLookup: %u alvos respondidos", n);
    Ret(ctx, 0);
}
KFUNC(NetDll_XNetQosRelease) { Ret(ctx, 0); }   // a memoria fica (poucos bytes por busca)

// Voz pela rede (headset): nao ha. XamVoiceCreate(usuario, flags, voz*) devolve "sem voz"
KFUNC(XamVoiceCreate) { if (Gpr32(ctx, 5)) W32(Gpr32(ctx, 5), 0); Ret(ctx, 0); }
KFUNC(XamVoiceHeadsetPresent) { Ret(ctx, 0); }
KFUNC(XamVoiceClose) { Ret(ctx, 0); }
KFUNC(XamVoiceSubmitPacket) { Ret(ctx, 0); }
