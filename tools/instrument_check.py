#!/usr/bin/env python3
"""Bloco 0: o rasterizador se comporta como o esperado? Rode ANTES de olhar render.

O QUE ISTO PROVA, e nada alem disso: backface culling escolhe o lado certo em
todas as camaras, a ordenacao por profundidade entre superficies sobrepostas
esta correta, e um defeito de orientacao conhecido E detectado. Nao valida
"visao", nao valida sombreamento, nao valida que eu vou interpretar a imagem
corretamente. A versao anterior deste arquivo terminava dizendo "INSTRUMENTO
CONFIAVEL - as leituras visuais deste build valem algo", o que e exatamente o
excesso de confianca que criou o problema original.

Contexto: eu media roupa - onde nao ha gabarito - com um rasterizador que
ninguem tinha conferido, e apresentava as imagens como prova. Quando testei,
`--cull` estava descartando as faces ERRADAS em duas das sete vistas, e a base
de camera de `debaixo` nao era ortonormal. Perfis e frentes que mostrei estavam
com o interior da malha desenhado por cima da superficie.

Os gabaritos sao FECHADOS POR CONSTRUCAO - esfera e toro gerados aqui. Usar o
corpo do jogo parece obvio e esta errado: corpo de Skyrim e aberto no pescoco,
punhos e tornozelos, entao `debaixo` enxerga por dentro do tornozelo e o culling
remove aquele interior CORRETAMENTE. Um gabarito precisa ter resposta garantida,
nao provavel. O toro entra porque tem genero 1: expoe erro de orientacao que uma
esfera convexa esconde, ja que numa esfera "para fora" e "para longe do centro"
coincidem e num toro nao.

Os quatro testes:

  1. SILHUETA      area pintada com e sem culling deve ser a mesma
  2. XOR           e os MESMOS pixels - area igual passa mesmo com 500 pixels
                   sumindo de um lado e 500 aparecendo do outro; XOR nao passa
  3. PROFUNDIDADE  dois quadrados sobrepostos com resposta conhecida, e a
                   resposta tem de INVERTER quando as profundidades trocam
  4. CONTROLE      esfera com um trecho de faces deliberadamente invertido TEM
                   QUE FALHAR. Sem isto o teste so prova que nada quebrou, nunca
                   que ele consegue enxergar quebra.

Uso:
    py -3 tools/instrument_check.py [--also caminho/para/malha.obj]
Saida 1 se qualquer teste falhar.
"""
from __future__ import annotations
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from raster import Frame, draw, frame_bounds, VIEWS   # noqa: E402
from intersect import load_obj                        # noqa: E402


# ---------------------------------------------------------------- gabaritos

def uv_sphere(nu=48, nv=24, r=1.0):
    """Esfera fechada, winding para FORA."""
    vs, fs = [], []
    for j in range(nv + 1):
        th = np.pi * j / nv
        for i in range(nu):
            ph = 2.0 * np.pi * i / nu
            vs.append([r * np.sin(th) * np.cos(ph),
                       r * np.sin(th) * np.sin(ph),
                       r * np.cos(th)])
    for j in range(nv):
        for i in range(nu):
            a = j * nu + i
            b = j * nu + (i + 1) % nu
            c = (j + 1) * nu + i
            d = (j + 1) * nu + (i + 1) % nu
            fs.append([a, c, b])
            fs.append([b, c, d])
    return np.array(vs, np.float64), np.array(fs, np.int32)


def torus(nu=48, nv=24, R=1.0, r=0.38):
    """Toro fechado, genero 1, winding para FORA."""
    vs, fs = [], []
    for i in range(nu):
        u = 2.0 * np.pi * i / nu
        for j in range(nv):
            v = 2.0 * np.pi * j / nv
            vs.append([(R + r * np.cos(v)) * np.cos(u),
                       (R + r * np.cos(v)) * np.sin(u),
                       r * np.sin(v)])
    for i in range(nu):
        for j in range(nv):
            a = i * nv + j
            b = ((i + 1) % nu) * nv + j
            c = i * nv + (j + 1) % nv
            d = ((i + 1) % nu) * nv + (j + 1) % nv
            fs.append([a, b, c])
            fs.append([c, b, d])
    return np.array(vs, np.float64), np.array(fs, np.int32)


def sphere_with_flipped_patch(axis=(1.0, 0.0, 0.0), frac=0.45):
    """CONTROLE NEGATIVO: esfera boa com uma CALOTA virada do avesso.

    Se o teste nao reprovar esta malha, ele so sabe dizer que nada quebrou -
    nunca que consegue VER quebra, que e para o que eu quero usa-lo.

    A primeira versao virava os 40 primeiros triangulos, que na parametrizacao
    UV ficam colados no polo: area minuscula, quase sempre fora de vista, e o
    teste "passava" na malha ruim. O defeito tem de ser VISIVEL para o controle
    significar alguma coisa - entao e uma calota inteira, definida por direcao,
    que aparece de frente em pelo menos uma camara.
    """
    v, f = uv_sphere()
    f = f.copy()
    ax = np.array(axis, np.float64)
    ax = ax / np.linalg.norm(ax)
    cen = v[f].mean(axis=1)
    cap = (cen @ ax) > frac
    f[cap] = f[cap][:, [0, 2, 1]]      # inverte o winding da calota
    return v, f


# ---------------------------------------------------------------- utilidades

def mask_of(frame):
    """Pixels que receberam geometria (fundo e branco puro)."""
    return frame.image().sum(axis=2) < 2.97


def render(verts, faces, view, cull, px=420, color=(0.70, 0.72, 0.76)):
    center, half = frame_bounds([(verts, faces, color)], view)
    fr = Frame(px, int(px * 1.25))
    draw(fr, verts, faces, color, view, center, half, cull=cull)
    return fr


# ---------------------------------------------------------------- testes

def test_silhouette_and_xor(verts, faces, view, px=420):
    """Culling nao pode mudar a silhueta de uma malha fechada, NEM trocar pixels."""
    solid = mask_of(render(verts, faces, view, cull=False, px=px))
    culled = mask_of(render(verts, faces, view, cull=True, px=px))
    a_solid = int(solid.sum())
    if a_solid == 0:
        return False, 0.0, 1.0, "nada desenhado"
    ratio = culled.sum() / a_solid
    xor = float((solid ^ culled).sum()) / a_solid
    if ratio < 0.97:
        return False, ratio, xor, "culling abriu buraco"
    if ratio > 1.03:
        return False, ratio, xor, "culling ADICIONOU area"
    if xor > 0.005:
        return False, ratio, xor, "mesma area, PIXELS diferentes"
    return True, ratio, xor, "ok"


def quad(y, x0, z0, x1, z1):
    """Quadrado plano num plano y constante.

    Na vista `frente` fwd=(0,-1,0), entao profundidade = -y e MAIOR y esta MAIS
    PERTO da camara. A primeira versao deste teste assumia o contrario e
    acusou o rasterizador de errar a profundidade - o erro era meu, na
    convencao do proprio gabarito.
    """
    vs = np.array([[x0, y, z0], [x1, y, z0], [x1, y, z1], [x0, y, z1]], np.float64)
    fs = np.array([[0, 1, 2], [0, 2, 3]], np.int32)
    return vs, fs


def test_depth_order(px=300):
    """Duas superficies sobrepostas com resposta conhecida, e ela tem de inverter.

    Testa a convencao de profundidade, a comparacao contra o buffer e a
    interpolacao - e testa entre MALHAS DIFERENTES, que e o caso do Smart
    Conform: varias pecas se sobrepondo.
    """
    view = "frente"          # fwd=(0,-1,0): profundidade = -y, MAIOR y esta mais perto
    A = quad(0.0, -1.0, -1.0, 0.5, 1.0)      # y=0
    B = quad(1.0, -0.5, -1.0, 1.0, 1.0)      # y=+1 -> MAIS PERTO da camera
    meshes = [(A[0], A[1], (1.0, 0.0, 0.0)), (B[0], B[1], (0.0, 1.0, 0.0))]
    center, half = frame_bounds(meshes, view)

    def compose(order):
        fr = Frame(px, px)
        for v, f, c in order:
            draw(fr, v, f, c, view, center, half, cull=False)
        return fr.image()

    img = compose(meshes)
    # amostra no meio da sobreposicao, em x entre -0.5 e 0.5
    h, w, _ = img.shape
    px_mid = img[h // 2, w // 2]
    near_is_green = px_mid[1] > px_mid[0] + 0.2
    if not near_is_green:
        return False, "a superficie MAIS PERTO nao ganhou o z-buffer"

    # inverte as profundidades: agora o vermelho e que esta na frente
    A2 = quad(1.0, -1.0, -1.0, 0.5, 1.0)     # agora o vermelho e que esta na frente
    B2 = quad(0.0, -0.5, -1.0, 1.0, 1.0)
    meshes2 = [(A2[0], A2[1], (1.0, 0.0, 0.0)), (B2[0], B2[1], (0.0, 1.0, 0.0))]
    center, half = frame_bounds(meshes2, view)
    img2 = compose(meshes2)
    px_mid2 = img2[h // 2, w // 2]
    near_is_red = px_mid2[0] > px_mid2[1] + 0.2
    if not near_is_red:
        return False, "trocar as profundidades NAO inverteu o resultado"
    return True, "ok"


# ---------------------------------------------------------------- principal

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--also", default="",
                    help="malha extra para inspecionar (informativa, nunca reprova)")
    a = ap.parse_args()

    fails = []

    print("1+2. SILHUETA e XOR em malha fechada")
    print(f"     {'gabarito':<10} {'vista':<12} {'cull/solid':>11} {'XOR':>8} {'':>8}")
    for name, verts, faces in (("esfera", *uv_sphere()), ("toro", *torus())):
        for view in VIEWS:
            ok, ratio, xor, why = test_silhouette_and_xor(verts, faces, view)
            if not ok:
                fails.append(f"{name}/{view}: {why}")
            print(f"     {name:<10} {view:<12} {100 * ratio:>10.1f}% {100 * xor:>7.2f}% "
                  f"{'OK' if ok else 'FALHOU':>8}" + ("" if ok else f"  <- {why}"))

    print()
    print("3. ORDENACAO POR PROFUNDIDADE entre malhas sobrepostas")
    ok, why = test_depth_order()
    if not ok:
        fails.append(f"profundidade: {why}")
    print(f"     duas superficies, resposta conhecida, e invertida  {'OK' if ok else 'FALHOU':>8}"
          + ("" if ok else f"  <- {why}"))

    print()
    print("4. CONTROLE NEGATIVO: esfera com uma calota invertida")
    bad_v, bad_f = sphere_with_flipped_patch()
    caught = []
    for view in VIEWS:
        ok, _r, _x, _w = test_silhouette_and_xor(bad_v, bad_f, view)
        if not ok:
            caught.append(view)
    if caught:
        print(f"     reprovada em {len(caught)}/{len(VIEWS)} vistas ({', '.join(caught[:3])}...)  OK")
    else:
        fails.append("controle negativo PASSOU - o teste nao enxerga defeito conhecido")
        print("     PASSOU EM TODAS AS VISTAS  <- FALHOU: nao enxerga defeito conhecido")

    if a.also and Path(a.also).exists():
        print()
        print("(informativo) malha aberta - furos legitimos baixam a razao, nunca reprova")
        verts, faces = load_obj(a.also)
        for view in VIEWS:
            _ok, ratio, xor, _why = test_silhouette_and_xor(verts, faces, view)
            print(f"     {view:<12} {100 * ratio:>10.1f}% {100 * xor:>7.2f}%")

    print()
    print("=" * 66)
    if fails:
        print("RASTER CULLING/DEPTH CHECK: FALHOU")
        for f in fails:
            print("  -", f)
        print("Descarte qualquer leitura visual feita com este build.")
        return 1
    print("RASTER CULLING/DEPTH CHECK: PASS")
    print("Provado: culling escolhe o lado certo nas 7 camaras, os pixels sao os")
    print("mesmos, a profundidade entre superficies sobrepostas esta correta, e um")
    print("defeito de orientacao conhecido E detectado.")
    print("NAO provado: sombreamento, nem que eu vou ler a imagem corretamente.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
