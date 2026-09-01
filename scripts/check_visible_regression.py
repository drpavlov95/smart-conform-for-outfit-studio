#!/usr/bin/env python3
"""Portao de auto-interseccao VISIVEL, por peca e como DELTA.

Por que existe: contar pares de triangulos que se cruzam nao corresponde ao que
o usuario ve - uma mudanca ja foi entregue com o total quase igual e ele viu o
clipping voltar. E valor absoluto tambem nao julga: o vampire_robes_alt nasce
com ~339 de auto-interseccao visivel na malha do artista, enquanto o Cloak
autorado tem 0.05. Portanto o unico julgamento honesto e "esta peca piorou em
relacao a ela mesma".

O baseline guarda, por peca: a malha AUTORADA, o conform do HOST e a nossa
saida no momento do congelamento. As duas primeiras sao contexto (mostram
quanto do defeito e herdado); a terceira e a referencia do delta.

Uso:
    python scripts/check_visible_regression.py --update   # congela baseline
    python scripts/check_visible_regression.py            # verifica
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "pathb_visible_selfint.py"
BASE = ROOT / "fixtures" / "visible_selfint" / "baseline.json"
HARNESS = ROOT / "build" / "algo" / "Release" / "drape-harness.exe"

# Configuracao de flags da CENA VIVA do usuario. O corpus roda sem elas e ja
# me fez tirar conclusao errada duas vezes; aqui o padrao e o que ele usa.
LIVE_ENV = {
    "DRAPE_STRICT": "1",
    "DRAPE_SHELL_SOLVE": "1",
    "DRAPE_SHELL_TRANSACTIONS": "0",
    "DRAPE_SHELL_BODY_EVICT": "1",
    "DRAPE_POSTERIOR_CHORD": "1",
    "DRAPE_LATE_LAYER_TUCK": "1",
    "DRAPE_ORIENT_REPAIR": "1",
}

# Conjuntos de captura a medir. Cada entrada e (rotulo, caminho).
CAPTURE_SETS = [
    ("telvanni", ROOT / "fixtures" / "visible_selfint" / "captures" / "telvanni"),
    ("blacksmith", ROOT / "corpus" / "fur_blacksmith_rm_heavy"),
    ("yarl", ROOT / "corpus" / "yarl_rm_heavy"),
    ("vampire", ROOT / "corpus" / "vampire_rm_heavy"),
]

EIXOS = ("solid_angle_curve", "p95_projected_curve", "worst_projected_curve")
# Tolerancia relativa: abaixo disto e ruido de reordenacao de ponto flutuante.
TOL_REL = 0.02


def rodar_harness(cap_dir: Path, dump_dir: Path, dirs_env=None) -> None:
    env = dict(os.environ)
    env.update(LIVE_ENV)
    env["DRAPE_DUMP_DIR"] = str(dump_dir)
    if dirs_env:
        env.update(dirs_env)
    subprocess.run([str(HARNESS), "--replay-dir", str(cap_dir)],
                   capture_output=True, text=True, timeout=3600, env=env)


def medir(garment: Path, body: Path, dirs: int) -> dict:
    fd, tmp = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    try:
        cmd = [sys.executable, str(TOOL), "--garment", str(garment),
               "--directions", str(dirs), "--json", tmp, "--quiet"]
        if body and body.exists():
            cmd += ["--body", str(body)]
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
        if out.returncode != 0:
            raise SystemExit("metrica falhou em %s:\n%s"
                             % (garment.name, out.stderr[-1500:]))
        with open(tmp, encoding="utf-8") as fh:
            rep = json.load(fh)
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass
    ch = rep["candidate"]["channels"]
    return {c: {e: ch[c][e] for e in EIXOS} for c in ("garment_only", "with_body")}


def coletar(dirs: int) -> dict:
    dados = {}
    with tempfile.TemporaryDirectory() as td:
        dump = Path(td)
        for rotulo, cap in CAPTURE_SETS:
            if not cap.exists():
                print("  (pulando %s: %s nao existe)" % (rotulo, cap))
                continue
            rodar_harness(cap, dump)
        for final in sorted(dump.glob("*__final.obj")):
            shape = final.name[:-len("__final.obj")]
            if shape.startswith("drape_live"):
                continue
            body = dump / (shape + "__body.obj")
            entry = {"nosso": medir(final, body, dirs)}
            for rot, suf in (("autorado", "__authored.obj"),
                             ("host", "__production.obj")):
                p = dump / (shape + suf)
                if p.exists():
                    entry[rot] = medir(p, body, dirs)
            dados[shape] = entry
            g = entry["nosso"]["garment_only"]["solid_angle_curve"]
            print("  %-22s nosso solidAngle=%8.4f" % (shape, g))
    return dados


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--dirs", type=int, default=42)
    args = ap.parse_args()

    if not HARNESS.exists():
        raise SystemExit("harness Release ausente: %s" % HARNESS)

    print("coletando (direcoes=%d)..." % args.dirs)
    atual = coletar(args.dirs)
    if not atual:
        raise SystemExit("nenhuma peca medida - conferir capturas")

    if args.update:
        BASE.parent.mkdir(parents=True, exist_ok=True)
        BASE.write_text(json.dumps(
            {"directions": args.dirs, "shapes": atual}, indent=2), encoding="utf-8")
        print("\nbaseline congelado em %s (%d pecas)" % (BASE, len(atual)))
        return 0

    if not BASE.exists():
        raise SystemExit("sem baseline; rode com --update primeiro")
    ref = json.loads(BASE.read_text(encoding="utf-8"))
    if ref.get("directions") != args.dirs:
        raise SystemExit("baseline foi congelado com direcoes=%s; use --dirs %s"
                         % (ref.get("directions"), ref.get("directions")))

    piores, novas = [], []
    print()
    for shape, entry in sorted(atual.items()):
        if shape not in ref["shapes"]:
            novas.append(shape)
            continue
        for canal in ("garment_only", "with_body"):
            for eixo in EIXOS:
                a = ref["shapes"][shape]["nosso"][canal][eixo]
                b = entry["nosso"][canal][eixo]
                if b > a * (1.0 + TOL_REL) + 1e-9:
                    piores.append((shape, canal, eixo, a, b))
        d = (entry["nosso"]["garment_only"]["solid_angle_curve"]
             - ref["shapes"][shape]["nosso"]["garment_only"]["solid_angle_curve"])
        print("  %-22s delta solidAngle = %+8.4f" % (shape, d))

    if novas:
        print("\n  pecas sem baseline (ignoradas): %s" % ", ".join(novas))
    if piores:
        print("\nREGRESSAO VISIVEL:")
        for shape, canal, eixo, a, b in piores:
            print("   %-20s %s/%s  %.4f -> %.4f  (+%.1f%%)"
                  % (shape, canal, eixo, a, b, 100.0 * (b - a) / max(a, 1e-9)))
        return 1
    print("\nOK - nenhuma peca piorou em auto-interseccao visivel.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
