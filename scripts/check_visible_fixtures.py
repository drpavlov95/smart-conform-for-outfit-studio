#!/usr/bin/env python3
"""Portao de regressao da METRICA, nao do algoritmo.

Roda a metrica de auto-interseccao visivel sobre poses cujo veredito humano
esta congelado em fixtures/visible_selfint/VEREDITOS.md e exige que ela ordene
como o olho do usuario ordenou. Se a metrica for alterada e deixar de
reproduzir esses julgamentos, este script falha - antes de qualquer decisao de
algoritmo ser tomada com base nela.

Uso: python scripts/check_visible_fixtures.py [--dirs N]
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIX = ROOT / "fixtures" / "visible_selfint"
TOOL = ROOT / "tools" / "pathb_visible_selfint.py"

# pior -> melhor, conforme o usuario julgou olhando no Outfit Studio
ORDEM = [
    ("telvanni_antes_do_conserto.obj", "clipping que ele reclamou"),
    ("telvanni_reprovada.obj",         "'fez o clipping retornar'"),
    ("telvanni_aprovada.obj",          "'melhorou 90% o clipping'"),
]
EIXOS = ("solid_angle_curve", "p95_projected_curve", "worst_projected_curve")


def medir(garment, body, dirs):
    fd, tmp = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    try:
        out = subprocess.run(
            [sys.executable, str(TOOL), "--garment", str(garment),
             "--body", str(body), "--directions", str(dirs),
             "--json", tmp, "--quiet"],
            capture_output=True, text=True, timeout=3600)
        if out.returncode != 0:
            raise SystemExit("metrica falhou em %s:\n%s"
                             % (garment.name, out.stderr[-2000:]))
        with open(tmp, encoding="utf-8") as fh:
            return json.load(fh)
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dirs", type=int, default=162)
    args = ap.parse_args()

    body = FIX / "telvanni_corpo.obj"
    faltando = [n for n, _ in ORDEM if not (FIX / n).exists()]
    if not body.exists():
        faltando.append(body.name)
    if faltando:
        raise SystemExit("fixtures ausentes: %s" % faltando)

    print("metrica: %s   direcoes: %d\n" % (TOOL.name, args.dirs))
    lidos = []
    for nome, veredito in ORDEM:
        rep = medir(FIX / nome, body, args.dirs)
        ch = rep["candidate"]["channels"]
        lidos.append((nome, ch["garment_only"], ch["with_body"]))
        print("  %-34s %s" % (nome, veredito))
        for rot, c in (("garment_only", ch["garment_only"]),
                       ("with_body   ", ch["with_body"])):
            print("      %s  solidAngle=%8.4f  p95=%8.4f  worst=%8.4f"
                  % (rot, c["solid_angle_curve"], c["p95_projected_curve"],
                     c["worst_projected_curve"]))

    falhas = []
    for eixo in EIXOS:
        for canal, idx in (("garment_only", 1), ("with_body", 2)):
            vals = [l[idx][eixo] for l in lidos]
            for a in range(len(vals) - 1):
                if not vals[a] > vals[a + 1]:
                    falhas.append("%s/%s: %s=%.4f nao e pior que %s=%.4f"
                                  % (canal, eixo, lidos[a][0], vals[a],
                                     lidos[a + 1][0], vals[a + 1]))
    print()
    if falhas:
        print("FALHOU - a metrica nao reproduz os vereditos humanos:")
        for f in falhas:
            print("   " + f)
        return 1
    print("OK - a metrica ordena as tres poses como o usuario julgou, "
          "nos dois canais e nos tres eixos.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
