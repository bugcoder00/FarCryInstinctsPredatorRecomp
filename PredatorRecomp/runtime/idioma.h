// Idioma dos textos do programa (launcher, janela de controles, tela de carregamento, aviso de conquista).
// O idioma e escolhido no launcher (opcoes.ini, "idioma = pt|en|fr|de|es|it"); na primeira vez vale o idioma da
// interface do Windows (ingles se nao for portugues). main.cpp liga idioma::ingles logo depois de ler o opcoes.ini,
// antes de qualquer texto aparecer.
// TR("portugues", "ingles") escolhe o texto na hora; serve para "..." e L"..." (vira ponteiro: nao da para juntar com
// outro "..." colado). Tabelas estaticas (iniciadas antes de o idioma ser lido) guardam os dois com TR2 e escolhem
// com TR na hora de usar. Os nomes no controles.ini e no opcoes.ini sao os mesmos em todos os idiomas.
// Frances/alemao/espanhol/italiano (idioma::outro 1-4): o texto em ingles e procurado na tabela de idioma_textos.cpp
// (gerada por tools\gerar_textos_programa.py a partir de tools\textos_programa.py); o que nao estiver la fica em ingles.
#pragma once

namespace idioma
{
    inline bool ingles = false;   // true tambem com frances/alemao/espanhol/italiano (a base e o ingles)
    inline int outro = 0;         // 0 = so portugues/ingles, 1 = frances, 2 = alemao, 3 = espanhol, 4 = italiano

    const char* Traduzir(const char* en);         // idioma_textos.cpp: o texto no idioma 'outro' (ou en)
    const wchar_t* Traduzir(const wchar_t* en);

    inline const char* Escolher(const char* pt, const char* en) { return !ingles ? pt : outro ? Traduzir(en) : en; }
    inline const wchar_t* Escolher(const wchar_t* pt, const wchar_t* en) { return !ingles ? pt : outro ? Traduzir(en) : en; }
}
#define IDIOMA_INGLES (::idioma::ingles)
#define TR(pt, en) (::idioma::Escolher((pt), (en)))
#define TR2(pt, en) pt, en

bool IdiomaDoWindowsIngles();   // launcher.cpp: true se a interface do Windows nao estiver em portugues
