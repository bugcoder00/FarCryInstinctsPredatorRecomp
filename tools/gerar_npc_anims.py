# Gera PredatorRecomp/runtime/npc_anims_lista.h: os arquivos de animacao dos mercenarios (merc01.xba e tudo que ele
# chama: clipes .mcl.mrg.bin -> dados .man.mrg.bin ...) que o editor de mapas (farcryed) nao tem. Eles so existem
# nos pacotes das fases da historia; o editor passa a ver esses arquivos dentro do farcryed (traducao_jogo.cpp copia
# os bytes guardados do pacote da fase na abertura). Uso: python tools/gerar_npc_anims.py [pasta do jogo]
# Nomes nos .fat: crc32 de "\" + caminho em minusculas.
import glob
import os
import re
import sys
import zlib

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(AQUI, '..', 'traducao'))
from fatdat import ler_fat, ler_arquivo  # noqa: E402

BS = '\\'
JOGO = sys.argv[1] if len(sys.argv) > 1 else \
    r'C:/Documentos/Projeto Far Cry/Jogo/Far Cry Instincts - Predator (USA, Europe) (En,Fr,De,Es,It)'
SAIDA = os.path.join(AQUI, '..', 'PredatorRecomp', 'runtime', 'npc_anims_lista.h')
INICIO = BS.join(['animations', 'characters', 'mercenaire', 'merc01.xba'])


def nome_hash(p):
    return zlib.crc32((p if p.startswith(BS) else BS + p).lower().encode())


def ler_xba(t):
    """.xba: "#nome", extensao, N pastas (">i<sub" = pasta i + sub), depois "anim=>pasta<arquivo" """
    L = t.split('\n')
    ext = L[1].strip()
    n = int(L[2])
    pastas = []
    for i in range(n):
        s = L[3 + i].strip()
        m = re.match(r'>(\d+)<(.*)', s)
        pastas.append(pastas[int(m.group(1))] + m.group(2) if m else s)
    refs = []
    for s in L[3 + n:]:
        m = re.match(r'([^=]*)=>(\d+)<(.*)', s.strip())
        if m:
            refs.append(pastas[int(m.group(2))] + m.group(3) + ext)
    return refs


def refs_binario(b):
    """caminhos "\\animations\\..." guardados como u32 tamanho + texto"""
    refs = []
    low = b.lower()
    alvo = (BS + 'animations').encode()
    k = low.find(alvo)
    while k >= 4:
        n = int.from_bytes(b[k - 4:k], 'little')
        if 4 < n < 400:
            refs.append(b[k:k + n].split(b'\0')[0].decode('latin1'))
        k = low.find(alvo, k + 1)
    return refs


def main():
    J = JOGO.rstrip('/\\') + '/'
    fases = sorted(os.path.relpath(x, J)[:-4] for x in glob.glob(J + 'levels/*/*.fat'))
    treino = [f for f in fases if '00_training' in f.lower()]
    ordem = treino + [f for f in fases if f not in treino]   # o treino tem quase tudo
    pac = {f: {e[3]: e for e in ler_fat(J + f + '.fat')} for f in ['farcryed'] + ordem}
    dats = {}
    fila, visto, incluir, faltam = [INICIO], set(), {}, 0
    while fila:
        p = fila.pop()
        h = nome_hash(p)
        if h in visto:
            continue
        visto.add(h)
        if h in pac['farcryed']:
            continue
        f = next((f for f in ordem if h in pac[f]), None)
        if f is None:
            faltam += 1   # animacoes de cenas que o jogo nao traz
            continue
        e = pac[f][h]
        if f not in dats:
            dats[f] = open(J + f + '.dat', 'rb')
        b = ler_arquivo(dats[f], e)
        incluir[h] = (f, p, e[1])
        fila += ler_xba(b.decode('latin1')) if p.lower().endswith('.xba') else refs_binario(b)
    porPacote = {}
    for h, (f, p, g) in incluir.items():
        porPacote.setdefault(f, []).append(h)
    total = sum(g for _, _, g in incluir.values())
    with open(SAIDA, 'w', encoding='utf-8', newline='\n') as o:
        o.write('// Gerado por tools/gerar_npc_anims.py: animacoes dos mercenarios que o editor de mapas nao tem\n')
        o.write('// (%d arquivos, %d bytes guardados; %d citados que o jogo nao traz). Pacote da fase + hash do nome.\n'
                % (len(incluir), total, faltam))
        o.write('#pragma once\n#include <cstdint>\n\n')
        o.write('struct NpcAnimArquivo { const char* pacote; uint32_t hash; };\n')
        o.write('static const NpcAnimArquivo kNpcAnims[] = {\n')
        for f in ordem:
            if f not in porPacote:
                continue
            nome = f.lower().replace(BS, BS + BS).replace('/', BS + BS)
            for h in sorted(porPacote[f]):
                o.write('    { "%s", 0x%08Xu },\n' % (nome, h))
        o.write('};\n')
    print('%d arquivos (%d bytes) de %d pacotes -> %s' % (len(incluir), total, len(porPacote), SAIDA))


if __name__ == '__main__':
    main()
