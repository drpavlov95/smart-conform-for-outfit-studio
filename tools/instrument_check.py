#!/usr/bin/env python3
"""Bloco 0: o instrumento mente? Rode ISTO antes de olhar qualquer resultado.

Passei semanas medindo roupa - onde nao existe gabarito - com um rasterizador que
ninguem tinha conferido, e mostrei as imagens como se fossem prova. Quando fui
testar, `--cull` estava descartando as faces ERRADAS em duas das sete vistas:
desenhando o interior da malha e escondendo a superficie. Todo perfil e toda
frente que eu apresentei estavam invertidos.

A causa e trivial e por isso passou: `VIEWS` define cada camera por (right, fwd)
e duas dessas bases sao canhotas, o que troca o sinal da area em espaco de tela.
O teste `area > 0` entao seleciona o lado oposto. Um rasterizador e trivialmente
testavel e eu nunca o testei.

O gabarito e uma malha FECHADA POR CONSTRUCAO - esfera e toro gerados aqui.
Usar o corpo do jogo parecia obvio e esta errado: corpo de Skyrim e ABERTO no
pescoco, nos punhos e nos tornozelos, entao a vista `debaixo` enxerga por dentro
do tornozelo e o culling remove aquele interior CORRETAMENTE. O teste acusava
83.1% e o culpado era o gabarito, nao o rasterizador. Um gabarito precisa ter a
resposta garantida, nao provavel.

O toro entra junto porque tem genero 1: ele expoe erro de orientacao que uma
esfera convexa esconde, ja que numa esfera "para fora" e "para longe do centro"
coincidem e num toro nao.

Numa malha fechada:

  1. toda face visivel e face da frente  -> culling nao pode abrir buraco
  2. a silhueta com culling e sem culling e a MESMA area
  3. o z-buffer nunca mostra o interior

Se qualquer vista falhar, nenhuma leitura visual daquele dia vale nada.

Uso:
    py -3 tools/instrument_check.py [--body tmp/vermelho/body.obj]
Codigo de saida 1 se alguma vista falhar.
"""
from __future__ import annotations
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
from raster import Frame, draw, frame_bounds, VIEWS   # noqa: E402
from intersect import load_obj                        # noqa: E402


def uv_sphere(nu=48, nv=24, r=1.0):
    """Esfera fechada. Winding para FORA."""
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
    """Toro fechado, genero 1. Winding para FORA."""
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


def painted_pixels(frame):
    """Pixels que receberam geometria (o fundo e branco puro)."""
    img = frame.image()
    return int((img.sum(axis=2) < 2.97).sum())


def check_view(verts, faces, view, px=420):
    center, half = frame_bounds([(verts, faces, (0.5, 0.5, 0.5))], view)

    solid = Frame(px, int(px * 1.25))
    draw(solid, verts, faces, (0.70, 0.72, 0.76), view, center, half, cull=False)
    culled = Frame(px, int(px * 1.25))
    draw(culled, verts, faces, (0.70, 0.72, 0.76), view, center, half, cull=True)

    a_solid = painted_pixels(solid)
    a_cull = painted_pixels(culled)
    if a_solid == 0:
        return False, 0.0, "nada desenhado"
    ratio = a_cull / a_solid

    # A silhueta de uma malha fechada nao muda com culling: as faces de tras
    # estao todas atras das da frente e o z-buffer ja as descartava. Se a area
    # cai, o culling removeu superficie visivel - o sinal esta invertido.
    if ratio < 0.97:
        return False, ratio, "culling abriu buraco na silhueta"
    if ratio > 1.03:
        return False, ratio, "culling ADICIONOU area (impossivel)"
    return True, ratio, "ok"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--also", default="",
                    help="malha extra para inspecionar (informativa, nao autoritativa)")
    a = ap.parse_args()

    gabaritos = [("esfera", *uv_sphere()), ("toro", *torus())]
    print(f"{'gabarito':<10} {'vista':<12} {'silhueta com/sem cull':>22} {'veredito':>10}")
    print("-" * 60)

    ok_all = True
    for name, verts, faces in gabaritos:
        for view in VIEWS:
            ok, ratio, why = check_view(verts, faces, view)
            ok_all &= ok
            mark = "OK" if ok else "FALHOU"
            print(f"{name:<10} {view:<12} {100 * ratio:>21.1f}% {mark:>10}" +
                  ("" if ok else f"   <- {why}"))

    if a.also and Path(a.also).exists():
        # Informativo: malhas do jogo sao ABERTAS (pescoco, punhos, tornozelos),
        # entao uma vista que olhe para dentro de um furo baixa a razao sem que
        # nada esteja errado. Nunca reprova.
        verts, faces = load_obj(a.also)
        print("-" * 60)
        for view in VIEWS:
            _ok, ratio, _why = check_view(verts, faces, view)
            print(f"{'(aberta)':<10} {view:<12} {100 * ratio:>21.1f}% {'info':>10}")

    print("-" * 60)
    if ok_all:
        print("INSTRUMENTO CONFIAVEL - as leituras visuais deste build valem algo.")
        return 0
    print("INSTRUMENTO QUEBRADO - NAO olhe render nenhum ate consertar.")
    print("Qualquer leitura visual feita com ele hoje deve ser descartada.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
