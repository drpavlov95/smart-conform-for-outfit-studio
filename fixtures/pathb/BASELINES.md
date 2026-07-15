# scoreboard: prod  (mode: `production`)

| garment | role | indexed_mean | indexed_p95 | surf_fwd_mean | surf_fwd_p95 | surf_rev_mean | surf_rev_p95 | int_flipped_faces | int_degenerate_faces | int_worst_seam_split | int_nan |
|---|---|---|---|---|---|---|---|---|---|---|---|
| blacksmith | sentinel:under-butt | 1.5441 | 3.9001 | 0.7563 | 2.7564 | 0.7915 | 1.977 | 1 | 3 | 0.0001 | 0 |
| yarl_clothes | sentinel:anti-umbrella | 0.8388 | 3.0751 | 0.259 | 0.9616 | 0.3023 | 1.2969 | 7 | 0 | 0.0 | 0 |
| yarl_cloak | sentinel:section-position/anti-imprint (not gravity-validated) | 2.0463 | 5.3611 | 1.2265 | 3.4131 | 0.9154 | 3.1823 | 10 | 0 | 0.0001 | 0 |
| vampire | secondary | 1.1858 | 2.8337 | 0.5099 | 1.7602 | 0.5591 | 1.834 | 34 | 0 | 0.0 | 0 |

## Notas de baseline (S0.2c, 2026-07-14)

- `int_flipped_faces` é vs a BASE autoral; a produção já tem alguns
  (blacksmith 1, yarl_clothes 7, yarl_cloak 10, vampire 34). Os gates de
  integridade comparam candidato vs ESTE baseline (0 NOVOS), não vs zero.
- Surface fwd/rev reproduzem os números do Codex (yarl Cloak fwd 1.2265 ≈
  1.218; yarl Clothes fwd 0.259 = bem casado) e o indexed reproduz os
  baselines conhecidos (blacksmith 1.5441 etc). Pipeline validado.
- yarl Clothes já está BEM casado pela produção (surf fwd 0.259): o papel
  dele é anti-guarda-chuva = candidato NÃO pode regredir indexed/surf.
