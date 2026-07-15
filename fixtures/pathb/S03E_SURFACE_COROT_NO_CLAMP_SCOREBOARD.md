# scoreboard: S03E_SURFACE_COROT_CLAMP_OFF  (mode: `shell:--rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-clamp off --soft-anchor on --body-mode continuation`)

| garment | role | verdict | indexed_mean | indexed_p95 | surf_fwd_mean | surf_fwd_p95 | surf_rev_mean | surf_rev_p95 | int_flipped_faces | int_degenerate_faces | int_worst_seam_split | int_nan |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| blacksmith | sentinel:under-butt | FAIL | 1.921 | 5.0785 | 0.9995 | 3.1632 | 1.046 | 2.9451 | 0 | 0 | 0.0 | 0 |
| yarl_clothes | sentinel:anti-umbrella | FAIL | 1.3215 | 4.8145 | 0.5443 | 2.4184 | 0.6238 | 3.1794 | 15 | 1 | 0.0 | 0 |
| yarl_cloak | sentinel:section-position/anti-imprint (not gravity-validated) | FAIL | 2.4608 | 7.9952 | 1.4769 | 4.252 | 0.9908 | 3.4672 | 50 | 1 | 0.0 | 0 |

## FAIL detail
- blacksmith: indexed_mean 1.921>1.6213, surf_fwd_mean 0.9995>0.7941, surf_rev_mean 1.046>0.8311
- yarl_clothes: indexed_mean 1.3215>0.8807, surf_fwd_mean 0.5443>0.272, surf_rev_mean 0.6238>0.3174, flips 15>7
- yarl_cloak: indexed_mean 2.4608>2.1486, surf_fwd_mean 1.4769>1.2878, surf_rev_mean 0.9908>0.9612, flips 50>10

## journal (commands + result hashes)

- `tools: pathb_gateSha=98d31b3cc621 shell_spikeSha=1d9332b9af57 pathb_metricsSha=c92929c80ba8`
- `blacksmith: rc=0 sha=a837e5ab2c47 cmd=C:\Users\T-Gamer\AppData\Local\Python\pythoncore-3.14-64\python.exe E:\SkyrimDev\Drape\tools\shell_spike.py --capture E:\SkyrimDev\Drape\corpus\fur_blacksmith_rm_heavy\drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap --dataset E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_OFF\ds_blacksmith --solve --output E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_OFF\cand_blacksmith.obj --rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-clamp off --soft-anchor on --body-mode continuation`
- `blacksmith: captureSha=4c182319a0e9 groundTruthSha=c55998911fa2 baseSha=3a71a704936d`
- `yarl_clothes: rc=0 sha=b2650313f840 cmd=C:\Users\T-Gamer\AppData\Local\Python\pythoncore-3.14-64\python.exe E:\SkyrimDev\Drape\tools\shell_spike.py --capture E:\SkyrimDev\Drape\corpus\yarl_rm_heavy\drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Clothes__RM_Heavy.drcap --dataset E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_OFF\ds_yarl_clothes --solve --output E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_OFF\cand_yarl_clothes.obj --rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-clamp off --soft-anchor on --body-mode continuation`
- `yarl_clothes: captureSha=3a2e940385cb groundTruthSha=31110901c2a7 baseSha=95ae65d42af4`
- `yarl_cloak: rc=0 sha=25e89bc89262 cmd=C:\Users\T-Gamer\AppData\Local\Python\pythoncore-3.14-64\python.exe E:\SkyrimDev\Drape\tools\shell_spike.py --capture E:\SkyrimDev\Drape\corpus\yarl_rm_heavy\drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Cloak__RM_Heavy.drcap --dataset E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_OFF\ds_yarl_cloak --solve --output E:\SkyrimDev\Drape\tmp\pathb\gate_S03E_SURFACE_COROT_CLAMP_OFF\cand_yarl_cloak.obj --rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target surface-corot --surface-corot-clamp off --soft-anchor on --body-mode continuation`
- `yarl_cloak: captureSha=7f1df88df028 groundTruthSha=28576b6ccaee baseSha=3d10426b7a7c`
