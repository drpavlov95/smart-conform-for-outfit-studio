# scoreboard: S03E_SURFACE_COROT_CLAMP_ON  (mode: `shell:--rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-clamp on --soft-anchor on --body-mode continuation`)

| garment | role | verdict | indexed_mean | indexed_p95 | surf_fwd_mean | surf_fwd_p95 | surf_rev_mean | surf_rev_p95 | int_flipped_faces | int_degenerate_faces | int_worst_seam_split | int_nan |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| blacksmith | sentinel:under-butt | FAIL | 1.9378 | 5.1611 | 1.001 | 3.1632 | 1.0478 | 2.9451 | 13 | 0 | 0.0 | 0 |
| yarl_clothes | sentinel:anti-umbrella | FAIL | 1.3224 | 4.8145 | 0.544 | 2.4184 | 0.6241 | 3.1794 | 16 | 1 | 0.0 | 0 |
| yarl_cloak | sentinel:section-position/anti-imprint (not gravity-validated) | FAIL | 2.4608 | 7.9952 | 1.4769 | 4.252 | 0.9908 | 3.4672 | 50 | 1 | 0.0 | 0 |

## FAIL detail
- blacksmith: indexed_mean 1.9378>1.6213, surf_fwd_mean 1.001>0.7941, surf_rev_mean 1.0478>0.8311, flips 13>1
- yarl_clothes: indexed_mean 1.3224>0.8807, surf_fwd_mean 0.544>0.272, surf_rev_mean 0.6241>0.3174, flips 16>7
- yarl_cloak: indexed_mean 2.4608>2.1486, surf_fwd_mean 1.4769>1.2878, surf_rev_mean 0.9908>0.9612, flips 50>10

## journal (commands + result hashes)

- `blacksmith: rc=0 sha=335d3c811800 cmd=C:\Users\T-Gamer\AppData\Local\Python\pythoncore-3.14-64\python.exe E:\SkyrimDev\Drape\tools\shell_spike.py --capture E:\SkyrimDev\Drape\corpus\fur_blacksmith_rm_heavy\drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap --dataset E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_ON\ds_blacksmith --solve --output E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_ON\cand_blacksmith.obj --rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-clamp on --soft-anchor on --body-mode continuation`
- `yarl_clothes: rc=0 sha=2927bcf54710 cmd=C:\Users\T-Gamer\AppData\Local\Python\pythoncore-3.14-64\python.exe E:\SkyrimDev\Drape\tools\shell_spike.py --capture E:\SkyrimDev\Drape\corpus\yarl_rm_heavy\drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Clothes__RM_Heavy.drcap --dataset E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_ON\ds_yarl_clothes --solve --output E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_ON\cand_yarl_clothes.obj --rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-clamp on --soft-anchor on --body-mode continuation`
- `yarl_cloak: rc=0 sha=25e89bc89262 cmd=C:\Users\T-Gamer\AppData\Local\Python\pythoncore-3.14-64\python.exe E:\SkyrimDev\Drape\tools\shell_spike.py --capture E:\SkyrimDev\Drape\corpus\yarl_rm_heavy\drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Cloak__RM_Heavy.drcap --dataset E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_ON\ds_yarl_cloak --solve --output E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_ON\cand_yarl_cloak.obj --rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-clamp on --soft-anchor on --body-mode continuation`
