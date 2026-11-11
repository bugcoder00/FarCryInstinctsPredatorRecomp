# Gera (ou completa) a "receita" dos shaders: tools\receita_shaders.txt
#
# Uso: python tools\gerar_receita.py <pasta do warmcache> <pasta do jogo (com shaders.dat)> [receita.txt]
#      python tools\gerar_receita.py juntar <pasta com receitas coletadas> [receita.txt]
#         (receitas coletadas jogando com o coletar_receita.bat: build_game\receitas\receita_*.txt; item 39)
#
# O warmcache (build_game\warmcache) guarda, de cada execucao, os pipelines usados (pipelines.txt: so numeros), as
# superficies (superficies.txt: so numeros) e o microcode de cada shader ja preenchido pelo D3D do jogo
# (vs_/ps_*.bin: isso e do jogo e NAO pode ir no pacote). A receita troca cada .bin por "molde do shaders.dat + as
# palavras que mudaram" (posicao e valor), e o jogo remonta o .bin na primeira abertura com o shaders.dat de quem
# joga (veja AplicarReceita em runtime\xenos_draw.cpp). Shader sem molde parecido no shaders.dat fica de fora (e os
# pipelines dele tambem): ir completo seria distribuir codigo do jogo (decisao do usuario, item 44). Rodar de novo
# com outro warmcache acrescenta o que faltava (nada e apagado).
import os, struct, sys, glob

def fnv(words):
    h = 0xCBF29CE484222325
    for w in words:
        for j in range(4):
            h ^= (w >> (j * 8)) & 0xFF
            h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h

def moldes(jogo):
    fat = open(os.path.join(jogo, 'shaders.fat'), 'rb').read()
    dat = open(os.path.join(jogo, 'shaders.dat'), 'rb').read()
    assert fat[:4] == b'DAT2'
    n = struct.unpack_from('<I', fat, 4)[0]
    porTam = {}   # (pixel, palavras) -> [(hash, codigo)]
    vistos = set()
    for i in range(n):
        off, tam = struct.unpack_from('<II', fat, 8 + i * 16)
        if tam < 12 or off + tam > len(dat): continue
        mag, co, cs = struct.unpack_from('>III', dat, off)
        if mag not in (0x102A0E00, 0x102A0E01): continue
        if cs == 0 or cs % 4 or co + cs > tam: continue
        code = list(struct.unpack_from('>%dI' % (cs // 4), dat, off + co))
        h = fnv(code)
        pixel = mag == 0x102A0E00
        if (pixel, h) in vistos: continue
        vistos.add((pixel, h))
        porTam.setdefault((pixel, len(code)), []).append((h, code))
    return porTam

def juntar(pasta, saida):
    # item 39: junta as receitas coletadas jogando (coletar_receita.bat -> receitas\receita_*.txt) com a receita atual
    shaders, pipes, sups = {}, [], []
    arquivos = ([saida] if os.path.exists(saida) else []) + sorted(glob.glob(os.path.join(pasta, '*.txt')))
    antes = None
    for arq in arquivos:
        for l in open(arq, encoding='ascii', errors='ignore'):
            l = l.rstrip('\r\n')
            if l.startswith('S '):
                c = l.split()
                shaders.setdefault((c[1], c[2].upper()), l)
            elif l.startswith('P '): pipes.append(l[2:])
            elif l.startswith('F '): sups.append(l[2:])
        if antes is None: antes = (len(shaders), len(dict.fromkeys(pipes)), len(dict.fromkeys(sups)))
    # pipeline so entra se os dois shaders dele estao na receita
    pipes = [l for l in dict.fromkeys(pipes) if len(l.split()) >= 9 and ('v', l.split()[0].upper()) in shaders and ('p', l.split()[1].upper()) in shaders]
    sups = list(dict.fromkeys(sups))
    with open(saida, 'w', encoding='ascii', newline='\n') as f:
        f.write('RECEITA 1\n')
        f.write('# receita dos shaders (tools\\gerar_receita.py): moldes do shaders.dat + palavras preenchidas, pipelines e superficies\n')
        for k in sorted(shaders): f.write(shaders[k] + '\n')
        for l in pipes: f.write('P ' + l + '\n')
        for l in sups: f.write('F ' + l + '\n')
    antes = antes or (0, 0, 0)
    print('receita: %d shaders, %d pipelines, %d superficies (antes %d/%d/%d), de %d arquivo(s)' % (
        len(shaders), len(pipes), len(sups), antes[0], antes[1], antes[2], len(arquivos)))

def main():
    if sys.argv[1] == 'juntar':
        saida = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'receita_shaders.txt')
        return juntar(sys.argv[2], saida)
    warm, jogo = sys.argv[1], sys.argv[2]
    saida = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'receita_shaders.txt')
    porTam = moldes(jogo)

    shaders, pipes, sups = {}, [], []
    if os.path.exists(saida):
        for l in open(saida, encoding='ascii'):
            l = l.rstrip('\n')
            if l.startswith('S '): shaders[(l.split()[1], l.split()[2])] = l
            elif l.startswith('P '): pipes.append(l[2:])
            elif l.startswith('F '): sups.append(l[2:])
    antes = (len(shaders), len(pipes), len(sups))

    semMolde = set()
    for arq in glob.glob(os.path.join(warm, '[pv]s_*.bin')):
        nome = os.path.basename(arq)
        pixel = nome.startswith('ps_')
        hs = nome[3:19].upper()
        tipo = 'p' if pixel else 'v'
        if (tipo, hs) in shaders: continue
        b = open(arq, 'rb').read()
        code = list(struct.unpack('<%dI' % (len(b) // 4), b))
        if '%016X' % fnv(code) != hs: continue
        melhor = None
        for th, tc in porTam.get((pixel, len(code)), []):
            dif = [(k, code[k]) for k in range(len(code)) if code[k] != tc[k]]
            if melhor is None or len(dif) < len(melhor[1]):
                melhor = (th, dif)
                if not dif: break
        if melhor is None or len(melhor[1]) > max(24, len(code) // 4):
            semMolde.add((tipo, hs)); continue   # item 44: NUNCA vai completo (seria codigo do jogo no pacote)
        shaders[(tipo, hs)] = 'S %s %s %016X %d%s' % (tipo, hs, melhor[0], len(code),
                                                     ''.join(' %X=%08X' % kv for kv in melhor[1]))

    def tem(t, h): return (t, h.upper()) in shaders
    p = os.path.join(warm, 'pipelines.txt')
    vistosP = set(pipes)
    if os.path.exists(p):
        for l in open(p, encoding='ascii', errors='ignore'):
            l = l.strip()
            c = l.split()
            if len(c) < 9 or l in vistosP: continue
            if not (tem('v', c[0]) and tem('p', c[1])): continue
            vistosP.add(l); pipes.append(l)
    p = os.path.join(warm, 'superficies.txt')
    vistosF = set(sups)
    if os.path.exists(p):
        for l in open(p, encoding='ascii', errors='ignore'):
            l = l.strip()
            if l and l not in vistosF: vistosF.add(l); sups.append(l)

    with open(saida, 'w', encoding='ascii', newline='\n') as f:
        f.write('RECEITA 1\n')
        f.write('# receita dos shaders (tools\\gerar_receita.py): moldes do shaders.dat + palavras preenchidas, pipelines e superficies\n')
        for k in sorted(shaders): f.write(shaders[k] + '\n')
        for l in pipes: f.write('P ' + l + '\n')
        for l in sups: f.write('F ' + l + '\n')
    print('receita: %d shaders, %d pipelines, %d superficies (antes %d/%d/%d); sem molde no shaders.dat (ficaram de fora): %d (%s)' % (
        len(shaders), len(pipes), len(sups), antes[0], antes[1], antes[2], len(semMolde),
        ' '.join(t + h for t, h in sorted(semMolde))[:300]))

main()
