// Idioma dos textos do programa (launcher, janela de controles, tela de carregamento, titulo da janela).
// A versao em ingles e um segundo executavel: build_game.ps1 -Ingles compila com PREDATOR_INGLES (pasta build_game_en).
// TR("portugues", "ingles") escolhe o texto na compilacao; serve para "..." e L"...". Os nomes no controles.ini e no
// opcoes.ini continuam os mesmos nas duas versoes (os arquivos servem para as duas).
#pragma once

#ifdef PREDATOR_INGLES
#define TR(pt, en) en
constexpr bool IDIOMA_INGLES = true;
#else
#define TR(pt, en) pt
constexpr bool IDIOMA_INGLES = false;
#endif
