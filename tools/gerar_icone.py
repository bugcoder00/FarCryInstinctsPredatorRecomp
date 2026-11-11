# Gera o icone do PredatorGame.exe (PredatorRecomp/runtime/recursos/predator.ico): "PR" em letras grossas ambar, com
# contorno escuro e marcas de garra vermelhas ao fundo (cores do menu do jogo: ambar/amarelo, vermelho escuro, preto).
# Uso: python tools\gerar_icone.py   (precisa do Pillow; a fonte Impact vem com o Windows)
import random
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

RAIZ = Path(__file__).resolve().parent.parent
SAIDA = RAIZ / "PredatorRecomp" / "runtime" / "recursos"
FONTE = "C:/Windows/Fonts/impact.ttf"

AMBAR_CLARO = (255, 214, 92)
AMBAR_ESCURO = (196, 116, 16)
VERMELHO = (150, 16, 14)
FUNDO = (20, 17, 13)


def desenhar(tam, detalhe=True):
    s = 4  # desenha 4x maior e reduz (bordas lisas)
    T = tam * s
    img = Image.new("RGBA", (T, T), (0, 0, 0, 0))

    # fundo: quadrado arredondado escuro com brilho ambar no centro
    mascara = Image.new("L", (T, T), 0)
    ImageDraw.Draw(mascara).rounded_rectangle((0, 0, T - 1, T - 1), radius=int(T * 0.18), fill=255)
    fundo = Image.new("RGBA", (T, T), FUNDO + (255,))
    brilho = Image.new("RGBA", (T, T), (0, 0, 0, 0))
    ImageDraw.Draw(brilho).ellipse((T * 0.1, T * 0.15, T * 0.9, T * 0.95), fill=AMBAR_ESCURO + (90,))
    brilho = brilho.filter(ImageFilter.GaussianBlur(T * 0.12))
    fundo = Image.alpha_composite(fundo, brilho)

    # marcas de garra (3 rasgos diagonais vermelhos)
    garra = Image.new("RGBA", (T, T), (0, 0, 0, 0))
    g = ImageDraw.Draw(garra)
    for i in range(3):
        # rasgo pontudo nas pontas e largo no meio, de cima a direita para baixo a esquerda
        xa, ya = T * (0.62 + i * 0.15), T * 0.02
        xb, yb = T * (0.08 + i * 0.15), T * 0.98
        larg = T * (0.14 if detalhe else 0.16)
        mx, my = (xa + xb) / 2, (ya + yb) / 2
        pts = [(xa, ya), (mx + larg / 2, my - larg * 0.2), (xb, yb), (mx - larg / 2, my + larg * 0.2)]
        g.polygon(pts, fill=VERMELHO + (235,))
    if detalhe:
        garra = garra.filter(ImageFilter.GaussianBlur(T * 0.004))
    fundo = Image.alpha_composite(fundo, garra)
    img.paste(fundo, (0, 0), mascara)

    # letras "PR": Impact, gradiente ambar, contorno escuro
    fonte = ImageFont.truetype(FONTE, int(T * 0.74))
    texto = "PR"
    caixa = ImageDraw.Draw(img).textbbox((0, 0), texto, font=fonte)
    tw, th = caixa[2] - caixa[0], caixa[3] - caixa[1]
    ox, oy = (T - tw) / 2 - caixa[0], (T - th) / 2 - caixa[1] + T * 0.01
    letras = Image.new("L", (T, T), 0)
    ImageDraw.Draw(letras).text((ox, oy), texto, font=fonte, fill=255)
    contorno = letras.filter(ImageFilter.MaxFilter(int(T * (0.045 if detalhe else 0.07)) | 1))
    sombra = contorno.filter(ImageFilter.GaussianBlur(T * 0.02))
    img.paste(Image.new("RGBA", (T, T), (0, 0, 0, 200)), (int(T * 0.015), int(T * 0.025)), sombra)
    img.paste(Image.new("RGBA", (T, T), (10, 8, 6, 255)), (0, 0), contorno)
    grad = Image.new("RGBA", (T, T))
    gd = ImageDraw.Draw(grad)
    for y in range(T):
        t = y / (T - 1)
        c = tuple(int(AMBAR_CLARO[k] * (1 - t) + AMBAR_ESCURO[k] * t) for k in range(3))
        gd.line((0, y, T, y), fill=c + (255,))
    if detalhe:
        # desgaste: riscos e pontos escuros dentro das letras (como as letras gastas do jogo)
        rnd = random.Random(7)
        for _ in range(int(30 * (tam / 256) + 10)):
            x, y = rnd.uniform(0, T), rnd.uniform(0, T)
            r = rnd.uniform(T * 0.003, T * 0.007)
            gd.ellipse((x - r, y - r * 0.6, x + r, y + r * 0.6), fill=(150, 80, 10, 255))
        for _ in range(9):
            x, y = rnd.uniform(T * 0.15, T * 0.85), rnd.uniform(T * 0.2, T * 0.8)
            gd.line((x, y, x + rnd.uniform(-T * 0.15, T * 0.15), y + rnd.uniform(T * 0.02, T * 0.08)), fill=(110, 50, 6, 255),
                    width=max(1, int(T * 0.006)))
    img.paste(grad, (0, 0), letras)

    # borda fina ambar (como as linhas dos paineis do menu)
    ImageDraw.Draw(img).rounded_rectangle((s, s, T - 1 - s, T - 1 - s), radius=int(T * 0.17), outline=AMBAR_ESCURO + (200,),
                                          width=max(s, int(T * 0.012)))
    img = img.resize((tam, tam), Image.LANCZOS)
    # a reducao 4x amacia as bordas: um pouco de nitidez devolve o contorno das letras
    if tam >= 32:
        img = img.filter(ImageFilter.UnsharpMask(radius=0.8, percent=70, threshold=1))
    return img


def main():
    SAIDA.mkdir(parents=True, exist_ok=True)
    # tamanhos exatos que o Windows pede (icones pequenos/medios/grandes em 100%, 125%, 150%, 200%...): sem eles o
    # Explorer reduz o de 128 para 120 (escala 125%) e o icone fica borrado
    tamanhos = [16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 120, 128, 144, 160, 192, 256]
    imagens = [desenhar(t, detalhe=t >= 48) for t in tamanhos]
    imagens[-1].save(SAIDA / "predator.ico", format="ICO", sizes=[(t, t) for t in tamanhos], append_images=imagens[:-1])
    imagens[-1].save(SAIDA / "predator_256.png")
    # folha de conferencia (todos os tamanhos lado a lado, ampliados 2x)
    folha = Image.new("RGBA", (sum(t * 2 for t in tamanhos) + 10 * len(tamanhos), 520), (60, 60, 60, 255))
    x = 5
    for t, im in zip(tamanhos, imagens):
        folha.paste(im.resize((t * 2, t * 2), Image.NEAREST), (x, 5), im.resize((t * 2, t * 2), Image.NEAREST))
        x += t * 2 + 10
    folha.save(SAIDA / "predator_tamanhos.png")
    print("icone em", SAIDA / "predator.ico")


if __name__ == "__main__":
    main()
