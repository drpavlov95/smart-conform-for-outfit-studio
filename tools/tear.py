#!/usr/bin/env python3
"""Costura rasgada: gemeos exatos que se separaram. O portao nao ve isso.

Ele reportou "muita mesh rasgando" e cada metrica que eu tinha disse que estava
tudo bem, porque nenhuma delas PODE ver isto. Costura em malha de Skyrim nao e
soldada topologicamente - sao vertices COINCIDENTES, dois indices no mesmo
ponto. Quando um passe move um gemeo e nao o outro, a costura abre e vira
buraco, mas as duas metades continuam do tamanho certo e fora do corpo. Logo:
estiramento nao ve, penetracao nao ve, interseccao nao ve.

A referencia e a saida do CONFORM DO HOST na mesma captura, e ela e categorica -
o host separa ZERO gemeos em 19 pecas. Entao o invariante nao e uma questao de
grau: se o vanilla mantem os gemeos juntos, nos temos que manter tambem.

Medido em 2026-08-05, com tudo que esta no disco ligado:

    vanilla   0 costuras abertas
    nosso   101, pior 4.66u (cinto do Blacksmith), 1.81u (cinto do Yarl)

E os cintos rasgam com TODAS as flags desligadas, inclusive sem o solve - o dono
e o bind do ARAP do cinto, que faz uma consulta de ponto-mais-proximo por
vertice. Dois gemeos ocupam o mesmo ponto, o empate e desfeito pela ordem de
travessia do BVH, eles caem em faces diferentes e seguem normais diferentes.
`exactSeamGroups` existe e e usado nas GUARDAS de transacao daquele caminho,
nunca no bind.

Uso:
    py -3 tools/tear.py [--caps tmp/live0726]
"""
from __future__ import annotations
import argparse
import os
import subprocess
from pathlib import Path

import numpy as np
from scipy.spatial import cKDTree

import sys
sys.path.insert(0, str(Path(__file__).parent))
from intersect import load_obj   # noqa: E402

BASE_ENV = dict(DRAPE_SHELL_SOLVE="1", DRAPE_SHELL_TRANSACTIONS="0",
                DRAPE_SHELL_BODY_EVICT="1", DRAPE_ANTICLIP_SAFE_FLOOR="1",
                DRAPE_POSTERIOR_CHORD="1")


def open_seams(ref_verts, pairs, verts, tol=0.05):
    """Quantos pares coincidentes na referencia estao separados agora, e quanto."""
    if len(pairs) == 0 or len(verts) != len(ref_verts):
        return 0, 0.0
    d = np.linalg.norm(verts[pairs[:, 0]] - verts[pairs[:, 1]], axis=1)
    return int((d > tol).sum()), float(d.max())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--caps", default="tmp/live0726")
    ap.add_argument("--harness", default="./build/algo/Release/drape-harness.exe")
    ap.add_argument("--env", default="", help="env extra do candidato")
    ap.add_argument("--work", default="tmp/tear")
    ap.add_argument("--tol", type=float, default=0.05)
    a = ap.parse_args()

    work = Path(a.work); work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env.update(BASE_ENV)
    for kv in a.env.split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            env[k] = v

    print(f"{'peca':<34} {'gemeos':>7} {'vanilla':>10} {'nosso':>16}")
    print("-" * 72)
    tot_v = tot_o = 0
    worst = []
    for cap in sorted(Path(a.caps).rglob("*.drcap")):
        shape = cap.name.split("___")[-1].split("__RM")[0]
        name = f"{cap.parent.name[:11]}/{shape[:20]}"
        vo = work / f"{cap.parent.name[:8]}_{shape}_van.obj"
        ro = work / f"{cap.parent.name[:8]}_{shape}_our.obj"
        subprocess.run([a.harness, "--replay", str(cap), "--input-obj", str(vo)],
                       env=env, capture_output=True)
        subprocess.run([a.harness, "--replay", str(cap), "--result-obj", str(ro)],
                       env=env, capture_output=True)
        if not (vo.exists() and ro.exists()):
            continue
        vv, _ = load_obj(vo)
        rv, _ = load_obj(ro)
        pairs = np.array(sorted(cKDTree(vv).query_pairs(1e-4)))
        if len(pairs) == 0:
            continue
        nv, _wv = open_seams(vv, pairs, vv, a.tol)
        no, wo = open_seams(vv, pairs, rv, a.tol)
        tot_v += nv; tot_o += no
        flag = "  <<<" if no > nv else ""
        print(f"{name:<34} {len(pairs):>7} {nv:>10} {no:>8} pior {wo:>5.2f}{flag}")
        if no > nv:
            worst.append((no, name))
    print("-" * 72)
    print(f"{'TOTAL':<34} {'':>7} {tot_v:>10} {tot_o:>8}")
    if tot_o > tot_v:
        worst.sort(reverse=True)
        print()
        print("*** COSTURA RASGANDO em " + ", ".join(n for _c, n in worst[:5]))
        print("O host nao separa nenhum gemeo. Se nos separamos, e defeito nosso.")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
