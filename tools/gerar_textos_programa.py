# Gera PredatorRecomp\runtime\idioma_textos.cpp (tabela ingles -> frances/alemao/espanhol/italiano dos textos do
# programa) a partir de tools\textos_programa.py e tools\textos_programa_instrucoes.py. Confere contra os TR(...)
# do codigo e avisa o que ficou sem traducao.
# Uso (na pasta do projeto): python tools\gerar_textos_programa.py
import os
import re
import sys
import glob

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.normpath(os.path.join(AQUI, '..'))
RUNTIME = os.path.join(RAIZ, 'PredatorRecomp', 'runtime')
sys.path.insert(0, AQUI)
import textos_programa as textos   # noqa: E402
import textos_programa_instrucoes as instrucoes   # noqa: E402

LIT = re.compile(r'\s*(?:L|u8)?"((?:[^"\\]|\\.)*)"')
STR = re.compile(r'"(?:[^"\\]|\\.)*"')


def argumento(s, i):
    """literais "..." colados de um argumento do TR (None se tiver outra coisa)"""
    partes = []; ok = True; prof = 0; j = i
    while j < len(s):
        m = LIT.match(s, j)
        if m:
            partes.append(m.group(1)); j = m.end(); continue
        c = s[j]
        if c in ' \t\r\n':
            j += 1; continue
        if prof == 0 and c in ',)':
            return (''.join(partes) if ok and partes else None), j
        ok = False
        if c == '(':
            prof += 1
        elif c == ')':
            prof -= 1
        elif c == '"':
            j = STR.match(s, j).end(); continue
        j += 1
    return None, j


def textos_do_codigo():
    achados = {}
    for f in glob.glob(os.path.join(RUNTIME, '*.cpp')) + glob.glob(os.path.join(RUNTIME, '*.h')):
        nome = os.path.basename(f)
        if nome in ('idioma.h', 'idioma_textos.cpp'):
            continue
        s = open(f, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'\bTR2?\(', s):
            pt, j = argumento(s, m.end())
            if j >= len(s) or s[j] != ',':
                continue
            en, _ = argumento(s, j + 1)
            if pt is not None and en is not None:
                achados.setdefault(en, nome)
    return achados


def escapar(texto):
    """texto puro (instrucoes) -> conteudo de literal C++ com \\r\\n"""
    return texto.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\r\\n')


def main():
    tabela = dict(textos.T)
    codigo = textos_do_codigo()
    # instrucoes: a chave e o texto ingles inteiro do codigo
    chave = [k for k in codigo if k.startswith(instrucoes.CHAVE)]
    assert len(chave) == 1, 'texto das instrucoes nao achado no launcher.cpp'
    tabela[chave[0]] = tuple(escapar(t) for t in (instrucoes.FR, instrucoes.DE, instrucoes.ES, instrucoes.IT))

    faltam = [k for k in codigo if k not in tabela and not k.startswith(textos.EM_INGLES_PREFIXOS)]
    for k in faltam:
        print('SEM TRADUCAO (%s): %s' % (codigo[k], k[:100]))
    for k, v in tabela.items():
        assert len(v) == 4, k
        for t in v:
            # os mesmos especificadores de formato do ingles (senao o printf quebra)
            fk = re.findall(r'%[-0-9.]*l?[a-zA-Z]', k)
            ft = re.findall(r'%[-0-9.]*l?[a-zA-Z]', t)
            if k.startswith(('%m', '\u2714 Unlocked')):
                continue   # strftime: a ordem de dia/mes muda
            assert sorted(fk) == sorted(ft) and (fk == ft or 'achievements' not in k), (k, t)

    linhas = []
    for k, v in tabela.items():
        linhas.append('    { "%s",\n      { "%s",\n        "%s",\n        "%s",\n        "%s" } },' % ((k,) + tuple(v)))
    cpp = '''// GERADO por tools\\gerar_textos_programa.py a partir de tools\\textos_programa.py e tools\\textos_programa_instrucoes.py: nao editar aqui.
// Textos do programa (TR do idioma.h) em frances, alemao, espanhol e italiano, procurados pelo texto em ingles.
#include "idioma.h"
#include <windows.h>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace
{
    struct Linha { const char* en; const char* t[4]; };
    const Linha LINHAS[] = {
%s
    };

    const std::unordered_map<std::string_view, const Linha*>& Mapa()
    {
        static const auto mapa = []
        {
            std::unordered_map<std::string_view, const Linha*> m;
            for (const Linha& l : LINHAS) m.emplace(l.en, &l);
            return m;
        }();
        return mapa;
    }
}

const char* idioma::Traduzir(const char* en)
{
    if (!en || outro < 1 || outro > 4) return en;
    const auto it = Mapa().find(en);
    return it == Mapa().end() ? en : it->second->t[outro - 1];
}

const wchar_t* idioma::Traduzir(const wchar_t* en)
{
    if (!en || outro < 1 || outro > 4) return en;
    // os textos L"..." sao literais: o mesmo ponteiro sempre tem o mesmo texto (guardado por idioma)
    static std::mutex trava;
    static std::unordered_map<const wchar_t*, const wchar_t*> feitos[5];
    static std::deque<std::wstring> guardados;
    std::lock_guard lock(trava);
    auto& feito = feitos[outro];
    if (const auto it = feito.find(en); it != feito.end()) return it->second;
    const wchar_t* r = en;
    const int n = WideCharToMultiByte(CP_UTF8, 0, en, -1, nullptr, 0, nullptr, nullptr);
    if (n > 0)
    {
        std::string u(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, en, -1, u.data(), n, nullptr, nullptr);
        if (const auto it = Mapa().find(u); it != Mapa().end())
        {
            const char* t = it->second->t[outro - 1];
            const int w = MultiByteToWideChar(CP_UTF8, 0, t, -1, nullptr, 0);
            std::wstring s(w > 0 ? w - 1 : 0, 0);
            if (w > 0) MultiByteToWideChar(CP_UTF8, 0, t, -1, s.data(), w);
            guardados.push_back(std::move(s));
            r = guardados.back().c_str();
        }
    }
    feito.emplace(en, r);
    return r;
}
''' % '\n'.join(linhas)
    destino = os.path.join(RUNTIME, 'idioma_textos.cpp')
    open(destino, 'w', encoding='utf-8', newline='\r\n').write(cpp)
    print('%d textos na tabela, %d sem traducao -> %s' % (len(tabela), len(faltam), destino))


main()
