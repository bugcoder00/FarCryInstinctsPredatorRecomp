// UPnP do roteador (codigo proprio: SSDP + SOAP sobre Winsock). Usado para jogar SYSTEM LINK pela internet:
// - descobrir o IP de internet do PC (GetExternalIPAddress: pergunta ao roteador, nao a sites de terceiros);
// - no anfitriao, abrir sozinho as portas do jogo (AddPortMapping) e fecha-las ao sair (DeletePortMapping).
// Passos: M-SEARCH por multicast (239.255.255.250:1900) -> o roteador responde com LOCATION (endereco do XML de
// descricao) -> GET do XML -> acha o servico WANIPConnection ou WANPPPConnection e o controlURL -> POST SOAP.
// O endereco "interno" das portas e o IP local que a propria conexao com o roteador usou (getsockname).
#include <winsock2.h>
#include <ws2tcpip.h>
#include "guest.h"
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#pragma comment(lib, "ws2_32.lib")

namespace
{
    struct Roteador
    {
        bool achado = false;
        std::string host;        // IP do roteador
        uint16_t porta = 80;
        std::string controle;    // caminho do controlURL
        std::string servico;     // urn:schemas-upnp-org:service:WANIPConnection:1 (ou PPP)
        std::string ipLocal;     // IP deste PC na rede do roteador
    };
    struct Mapa { uint16_t porta; const char* proto; };

    std::mutex g_mx;
    Roteador g_rot;
    bool g_procurado = false;
    std::vector<Mapa> g_abertas;
    std::string g_ipExterno, g_erro;
    std::atomic<bool> g_abrindo{ false };

    std::string Minusculo(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

    // http://host:porta/caminho
    bool PartirUrl(const std::string& url, std::string& host, uint16_t& porta, std::string& caminho)
    {
        std::string u = url;
        if (Minusculo(u.substr(0, 7)) != "http://") return false;
        u = u.substr(7);
        size_t barra = u.find('/');
        std::string hp = barra == std::string::npos ? u : u.substr(0, barra);
        caminho = barra == std::string::npos ? "/" : u.substr(barra);
        size_t dois = hp.find(':');
        host = hp.substr(0, dois);
        porta = dois == std::string::npos ? 80 : (uint16_t)atoi(hp.c_str() + dois + 1);
        return !host.empty() && porta;
    }

    // pedido HTTP simples (Connection: close), com tempo maximo; devolve o codigo (200...) e o corpo
    int Http(const std::string& host, uint16_t porta, const std::string& pedido, std::string& corpo, std::string* ipLocal = nullptr, int ms = 2500)
    {
        corpo.clear();
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return -1;
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(porta);
        if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) { closesocket(s); return -1; }
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        connect(s, (sockaddr*)&sa, sizeof(sa));
        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        fd_set e; FD_ZERO(&e); FD_SET(s, &e);
        timeval tv{ ms / 1000, (ms % 1000) * 1000 };
        if (select(0, nullptr, &w, &e, &tv) <= 0 || FD_ISSET(s, &e)) { closesocket(s); return -1; }
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        DWORD t = ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
        if (ipLocal)
        {
            sockaddr_in eu{}; int n = sizeof(eu);
            getsockname(s, (sockaddr*)&eu, &n);
            char b[32] = {}; inet_ntop(AF_INET, &eu.sin_addr, b, sizeof(b));
            *ipLocal = b;
        }
        if (send(s, pedido.data(), (int)pedido.size(), 0) != (int)pedido.size()) { closesocket(s); return -1; }
        std::string r;
        char buf[4096];
        for (int n; (n = recv(s, buf, sizeof(buf), 0)) > 0;) { r.append(buf, n); if (r.size() > 512 * 1024) break; }
        closesocket(s);
        size_t fimCab = r.find("\r\n\r\n");
        if (r.compare(0, 5, "HTTP/") != 0 || fimCab == std::string::npos) return -1;
        int codigo = atoi(r.c_str() + r.find(' ') + 1);
        std::string cab = Minusculo(r.substr(0, fimCab));
        corpo = r.substr(fimCab + 4);
        if (cab.find("transfer-encoding: chunked") != std::string::npos)
        {
            std::string junto;
            size_t p = 0;
            while (p < corpo.size())
            {
                size_t fim = corpo.find("\r\n", p);
                if (fim == std::string::npos) break;
                size_t tam = strtoul(corpo.c_str() + p, nullptr, 16);
                if (!tam) break;
                junto += corpo.substr(fim + 2, tam);
                p = fim + 2 + tam + 2;
            }
            corpo = junto;
        }
        return codigo;
    }

    // conteudo da 1a etiqueta <nome>...</nome> a partir de "de" (ignora prefixo de namespace como <u:nome>)
    std::string Etiqueta(const std::string& xml, const std::string& nome, size_t de = 0, size_t* onde = nullptr)
    {
        std::string m = Minusculo(xml);
        std::string n = Minusculo(nome) + ">";
        for (size_t p = m.find(n, de); p != std::string::npos; p = m.find(n, p + 1))
        {
            if (p == 0) continue;
            size_t abre = m.rfind('<', p);
            if (abre == std::string::npos || m[abre + 1] == '/' || m.substr(abre, p - abre).find(' ') != std::string::npos) continue;
            size_t ini = p + n.size(), fim = xml.find('<', ini);
            if (fim == std::string::npos) return "";
            if (onde) *onde = fim;
            std::string v = xml.substr(ini, fim - ini);
            size_t a = v.find_first_not_of(" \t\r\n"), b = v.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? "" : v.substr(a, b - a + 1);
        }
        return "";
    }

    std::string Soap(const Roteador& r, const char* acao, const std::string& args, int& codigo, int ms = 2500)
    {
        std::string corpo = std::string("<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:") + acao + " xmlns:u=\"" + r.servico + "\">" + args +
            "</u:" + acao + "></s:Body></s:Envelope>\r\n";
        std::string pedido = "POST " + r.controle + " HTTP/1.1\r\nHost: " + r.host + ":" + std::to_string(r.porta) +
            "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + r.servico + "#" + acao + "\"\r\nContent-Length: " +
            std::to_string(corpo.size()) + "\r\nConnection: close\r\n\r\n" + corpo;
        std::string resp;
        codigo = Http(r.host, r.porta, pedido, resp, nullptr, ms);
        if (codigo != 200)
        {
            std::string erro = Etiqueta(resp, "errorCode");
            if (!erro.empty()) codigo = atoi(erro.c_str());   // erro do UPnP (718 = porta ja usada, 725 = so permanente...)
        }
        return resp;
    }

    // le o XML de descricao e acha o servico de conexao com a internet
    bool LerDescricao(const std::string& local, Roteador& r)
    {
        std::string host, caminho; uint16_t porta;
        if (!PartirUrl(local, host, porta, caminho)) return false;
        std::string xml;
        std::string ipLocal;
        int c = Http(host, porta, "GET " + caminho + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(porta) + "\r\nConnection: close\r\n\r\n", xml, &ipLocal);
        if (c != 200) return false;
        for (const char* tipo : { "urn:schemas-upnp-org:service:WANIPConnection:", "urn:schemas-upnp-org:service:WANPPPConnection:" })
        {
            size_t p = Minusculo(xml).find(Minusculo(tipo));
            if (p == std::string::npos) continue;
            size_t fimTipo = xml.find('<', p);
            size_t fimServ = Minusculo(xml).find("</service>", p);
            std::string servico = xml.substr(p, fimTipo - p);
            size_t onde = 0;
            std::string ctl = Etiqueta(xml, "controlURL", p, &onde);
            if (ctl.empty() || (fimServ != std::string::npos && onde > fimServ)) continue;
            // controlURL pode ser absoluto (http://...) ou relativo ao endereco do XML
            if (Minusculo(ctl.substr(0, 7)) == "http://")
            {
                std::string h2, c2; uint16_t p2;
                if (!PartirUrl(ctl, h2, p2, c2)) continue;
                host = h2; porta = p2; ctl = c2;
            }
            else if (ctl[0] != '/') ctl = "/" + ctl;
            r.achado = true;
            r.host = host; r.porta = porta; r.controle = ctl; r.servico = servico; r.ipLocal = ipLocal;
            return true;
        }
        return false;
    }

    // procura o roteador (uma vez por execucao)
    bool Procurar(Roteador& r, std::string& erro)
    {
        {
            std::lock_guard l(g_mx);
            if (g_procurado) { r = g_rot; erro = g_erro; return r.achado; }
        }
        WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
        Roteador achado;
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET)
        {
            int ttl = 2;
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
            sockaddr_in mc{};
            mc.sin_family = AF_INET;
            mc.sin_port = htons(1900);
            inet_pton(AF_INET, "239.255.255.250", &mc.sin_addr);
            std::vector<std::string> locais;
            const char* tipos[] = { "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:service:WANIPConnection:1",
                                    "urn:schemas-upnp-org:device:InternetGatewayDevice:2", "urn:schemas-upnp-org:service:WANPPPConnection:1" };
            auto inicio = std::chrono::steady_clock::now();
            for (int volta = 0; volta < 2 && !achado.achado; volta++)
            {
                for (const char* st : tipos)
                {
                    std::string m = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + st + "\r\n\r\n";
                    sendto(s, m.data(), (int)m.size(), 0, (sockaddr*)&mc, sizeof(mc));
                }
                // respostas por ate 2,5 s em cada volta
                while (std::chrono::steady_clock::now() - inicio < std::chrono::milliseconds(2500 * (volta + 1)))
                {
                    fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
                    timeval tv{ 0, 200000 };
                    if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
                    char buf[2048];
                    int n = recv(s, buf, sizeof(buf) - 1, 0);
                    if (n <= 0) continue;
                    buf[n] = 0;
                    std::string resp(buf), m = Minusculo(resp);
                    size_t p = m.find("\nlocation:");
                    if (p == std::string::npos) continue;
                    size_t ini = resp.find_first_not_of(' ', p + 10), fim = resp.find('\r', ini);
                    std::string local = resp.substr(ini, fim - ini);
                    if (std::find(locais.begin(), locais.end(), local) != locais.end()) continue;
                    locais.push_back(local);
                    if (LerDescricao(local, achado)) { Log("[upnp] roteador encontrado: %s (%s)", local.c_str(), achado.servico.c_str()); break; }
                }
            }
            closesocket(s);
            if (!achado.achado)
                erro = locais.empty() ? "o roteador nao respondeu (UPnP desligado no roteador, ou o Windows bloqueou a busca)"
                                      : "o roteador respondeu, mas nao tem o servico de abrir portas";
        }
        else erro = "nao foi possivel procurar o roteador";
        std::lock_guard l(g_mx);
        g_procurado = true;
        g_rot = achado;
        g_erro = erro;
        r = achado;
        return achado.achado;
    }

    bool IpPrivado(uint32_t ip)
    {
        return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 22) == (100u << 2 | 1) || ip == 0;
    }
}

// IP de internet pelo roteador. Devolve false com o motivo em "erro". "aviso" explica CGNAT (IP do roteador privado).
bool UpnpIpExterno(std::string& ip, std::string& erro, std::string* aviso)
{
    Roteador r;
    if (!Procurar(r, erro)) return false;
    int c = 0;
    std::string resp = Soap(r, "GetExternalIPAddress", "", c);
    ip = Etiqueta(resp, "NewExternalIPAddress");
    if (c != 200 || ip.empty()) { erro = "o roteador nao informou o IP de internet (codigo " + std::to_string(c) + ")"; return false; }
    in_addr a{};
    inet_pton(AF_INET, ip.c_str(), &a);
    if (aviso && IpPrivado(ntohl(a.s_addr)))
        *aviso = "o IP de internet do roteador (" + ip + ") e um endereco privado: a sua internet usa CGNAT ou ha outro roteador "
                 "na frente. Os amigos nao conseguem entrar abrindo portas; use uma rede virtual (Radmin VPN, ZeroTier) ou "
                 "peca um IP publico/fixo para a operadora.";
    std::lock_guard l(g_mx);
    g_ipExterno = ip;
    return true;
}

namespace { void AbrirJa(); }
// abre as portas do jogo no roteador (anfitriao). Em segundo plano: o jogo nao espera.
void UpnpAbrirPortas()
{
    if (g_abrindo.exchange(true)) return;   // uma vez por execucao
    std::thread(AbrirJa).detach();
}
// diagnostico (PredatorGame.exe --upnp-teste): abre e espera terminar
void UpnpAbrirPortasEsperar() { g_abrindo = true; AbrirJa(); }

namespace
{
    void AbrirJa()
    {
        Roteador r;
        std::string erro;
        if (!Procurar(r, erro))
        {
            Log("[upnp] nao deu para abrir as portas sozinho: %s.", erro.c_str());
            Log("[upnp] Abra no roteador (redirecionamento de portas) para o IP deste PC: UDP 1000, 1004, 1005, 1010 e 49001 (veja o LEIA-ME).");
            return;
        }
        const Mapa portas[] = { { 1000, "UDP" }, { 1004, "UDP" }, { 1005, "UDP" }, { 1010, "UDP" }, { 49001, "UDP" }, { 49001, "TCP" } };
        std::string falhas;
        for (const Mapa& m : portas)
        {
            auto args = [&](int prazo) {
                return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(m.porta) + "</NewExternalPort><NewProtocol>" + m.proto +
                       "</NewProtocol><NewInternalPort>" + std::to_string(m.porta) + "</NewInternalPort><NewInternalClient>" + r.ipLocal +
                       "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>PredatorGame " + m.proto + " " +
                       std::to_string(m.porta) + "</NewPortMappingDescription><NewLeaseDuration>" + std::to_string(prazo) + "</NewLeaseDuration>";
            };
            int c = 0;
            Soap(r, "AddPortMapping", args(14400), c);   // 4 horas (se o jogo fechar sem avisar, o roteador apaga sozinho)
            if (c == 725) Soap(r, "AddPortMapping", args(0), c);   // roteador so aceita "permanente"
            if (c == 718)
            {
                // a porta ja esta redirecionada (talvez por uma execucao anterior): apaga e tenta de novo
                int d = 0;
                Soap(r, "DeletePortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(m.porta) +
                     "</NewExternalPort><NewProtocol>" + m.proto + "</NewProtocol>", d);
                Soap(r, "AddPortMapping", args(14400), c);
                if (c == 725) Soap(r, "AddPortMapping", args(0), c);
            }
            if (c == 200) { std::lock_guard l(g_mx); g_abertas.push_back(m); }
            else falhas += std::string(falhas.empty() ? "" : ", ") + m.proto + " " + std::to_string(m.porta) + " (codigo " + std::to_string(c) + ")";
        }
        size_t n;
        { std::lock_guard l(g_mx); n = g_abertas.size(); }
        if (falhas.empty()) Log("[upnp] portas abertas no roteador para %s: UDP 1000, 1004, 1005, 1010, 49001 e TCP 49001", r.ipLocal.c_str());
        else
        {
            Log("[upnp] %zu portas abertas; o roteador recusou: %s.", n, falhas.c_str());
            Log("[upnp] Abra essas no roteador (redirecionamento de portas) para o IP %s (veja o LEIA-ME).", r.ipLocal.c_str());
        }
    }
}

// fecha as portas abertas por este programa (ao sair)
void UpnpFecharPortas()
{
    std::vector<Mapa> abertas;
    Roteador r;
    {
        std::lock_guard l(g_mx);
        abertas.swap(g_abertas);
        r = g_rot;
    }
    if (abertas.empty() || !r.achado) return;
    for (const Mapa& m : abertas)
    {
        int c = 0;
        Soap(r, "DeletePortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(m.porta) +
             "</NewExternalPort><NewProtocol>" + m.proto + "</NewProtocol>", c, 1000);
    }
    Log("[upnp] %zu portas fechadas no roteador", abertas.size());
}

// ha portas abertas (ou abrindo) por este programa: a troca de fase passa isso ao processo novo (xam.cpp)
bool UpnpPortasAbertas()
{
    std::lock_guard l(g_mx);
    return !g_abertas.empty();
}
