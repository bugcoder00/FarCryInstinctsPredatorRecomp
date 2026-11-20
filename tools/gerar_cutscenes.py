# Gera tools\cutscenes.txt: a lista das cenas feitas no motor (cutscenes das fases) para tools\cutscenes.ps1.
#
# Como as cenas sao guardadas (achado nos pacotes das fases, levels\<FASE>\<fase>.fat/.dat):
#   - o XML da missao ("<Mission ...>", um arquivo do pacote) tem as ancoras de IA (Object Type="AIAnchor") cujas
#     acoes sao a cena: SendSignal GUI_BeginCutScene para o MissionHandler, PlayAnimation do Carver, falas, filmes
#     (PlaySequence) e no fim GUI_EndCutScene. Quem comeca a ancora e um gatilho (quase sempre ProximityTrigger)
#     com EventTarget Event="SwitchToGenericJob" ExtraParam="<ancora>" no personagem, ou outra ancora
#     (SendSignal SwitchToGenericJob Parameter="<ancora>"), ou a propria fase ao carregar.
#   - algumas cenas sao o proprio gatilho: EventTarget Target="0" Event="_GUI_BeginCutScene" (01_beach).
#   - os filmes (<Sequence> do arquivo de filmes da fase) sao so as animacoes de objetos/camera que as cenas soltam.
# Os videos Bink (pasta videos) nao entram aqui: a ferramenta lista a pasta.
#
# Linha: fase|gatilho|evento|ancora   (gatilho vazio = a cena comeca sozinha ao carregar a fase)
# Uso: python tools\gerar_cutscenes.py   (le pasta_jogo do PredatorRecomp\build_game\opcoes.ini)
import lzma
import os
import re
import sys
import xml.etree.ElementTree as ET

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(AQUI, '..', 'traducao'))
import fatdat  # noqa: E402


def pasta_jogo():
    ini = os.path.join(AQUI, '..', 'PredatorRecomp', 'build_game', 'opcoes.ini')
    for linha in open(ini, encoding='utf-8', errors='replace'):
        m = re.match(r'\s*pasta_jogo\s*=\s*(.+?)\s*$', linha)
        if m:
            return m.group(1)
    raise SystemExit('pasta_jogo nao achada em ' + ini)


def inicio(dat, e):
    dat.seek(e[0])
    b = dat.read(min(e[1], 4096))
    if e[1] == e[2]:
        return b[:16]
    try:
        return lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(b[1:], max_length=16)
    except Exception:
        return b''


def missao(base):
    es = fatdat.ler_fat(base + '.fat')
    with open(base + '.dat', 'rb') as dat:
        for e in es:
            if e[2] > 200 and inicio(dat, e).startswith(b'<Mission'):
                return fatdat.ler_arquivo(dat, e)
    return None


def cenas(xml):
    raiz = ET.fromstring(xml)
    objs = [o for o in raiz.iter() if o.tag in ('Entity', 'Object') and o.get('Name')]
    classe = {o.get('Name'): o.get('EntityClass') or o.get('Type') for o in objs}

    def quem_comeca(ancora, prof=0):
        r = []
        for o in objs:
            for et in o.iter('EventTarget'):
                if et.get('ExtraParam') == ancora:
                    r.append((o, et.get('SourceEvent')))
            for ss in o.iter('SendSignal'):
                if ss.get('Parameter') == ancora:
                    r.append((o, None))
        saida = []
        for o, ev in r:
            if (o.get('Type') == 'AIAnchor') and prof < 4:
                saida += quem_comeca(o.get('Name'), prof + 1)
            elif 'Trigger' in (o.get('EntityClass') or ''):
                saida.append((o.get('Name'), ev or 'Enter'))
        return saida

    lista = []
    for o in objs:
        if o.get('Type') == 'AIAnchor' and any(s.get('Name') == 'GUI_BeginCutScene' for s in o.iter('SendSignal')):
            ini = quem_comeca(o.get('Name'))
            if not ini:
                # sem gatilho: comeca com a fase (ou por script); so carregar a fase
                lista.append(('', '', o.get('Name')))
            for g, ev in ini[:1]:
                lista.append((g, ev, o.get('Name')))
        for et in o.iter('EventTarget'):
            if et.get('Event') == '_GUI_BeginCutScene' and 'Trigger' in (o.get('EntityClass') or ''):
                bind = [x.get('ExtraParam') for x in o.iter('EventTarget') if x.get('Event') == '_Player_BindEyes']
                lista.append((o.get('Name'), et.get('SourceEvent') or 'Enter', (bind[0].split(',')[0] if bind and bind[0] else o.get('Name'))))
    vistos, final = set(), []
    for c in lista:
        if (c[0] or c[2]) in vistos:
            continue
        vistos.add(c[0] or c[2])
        final.append(c)
    return final


def main():
    niveis = os.path.join(pasta_jogo(), 'levels')
    linhas = ['# gerado por tools\\gerar_cutscenes.py: fase|gatilho|evento|ancora (gatilho vazio = comeca com a fase)']
    for lv in sorted(os.listdir(niveis)):
        if not re.match(r'\d\d_', lv):   # so as fases da historia (as M* e MET_* sao do multijogador)
            continue
        base = os.path.join(niveis, lv, lv.lower())
        if not os.path.exists(base + '.fat'):
            continue
        x = missao(base)
        if not x:
            continue
        for g, ev, a in cenas(x):
            linhas.append('%s|%s|%s|%s' % (lv.lower(), g, ev, a))
            print(lv.lower(), g or '(inicio da fase)', ev, a)
    with open(os.path.join(AQUI, 'cutscenes.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(linhas) + '\n')
    print(len(linhas) - 1, 'cenas em tools\\cutscenes.txt')


if __name__ == '__main__':
    main()
