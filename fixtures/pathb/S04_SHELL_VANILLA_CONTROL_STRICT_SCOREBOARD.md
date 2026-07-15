# scoreboard: S04_SHELL_VANILLA_CONTROL [STRICT]  (mode: `shell:--rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target vanilla --soft-anchor on --body-mode continuation`)

| garment | role | verdict | indexed_mean | indexed_p95 | surf_fwd_mean | surf_fwd_p95 | surf_rev_mean | surf_rev_p95 | int_flipped_faces | int_degenerate_faces | int_worst_seam_split | int_nan |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| blacksmith | sentinel:under-butt | FAIL | 1.8508 | 5.0617 | 0.9969 | 3.1641 | 1.0488 | 2.9505 | 0 | 0 | 0.0 | 0 |
| yarl_clothes | sentinel:anti-umbrella | FAIL | 1.198 | 4.7641 | 0.4832 | 2.4244 | 0.5906 | 3.1883 | 9 | 0 | 0.0 | 0 |
| yarl_cloak | sentinel:section-position/anti-imprint (not gravity-validated) | FAIL | 1.9344 | 5.1412 | 1.194 | 3.8348 | 0.9439 | 3.2132 | 19 | 0 | 0.0 | 0 |

## FAIL detail
- blacksmith: indexed_mean 1.8508>1.6213, surf_fwd_mean 0.9969>0.7941, surf_rev_mean 1.0488>0.8311, indexed_p95 5.0617>4.0951, surf_fwd_p95 3.1641>2.8942, surf_rev_p95 2.9505>2.0758
- yarl_clothes: indexed_mean 1.198>0.8807, surf_fwd_mean 0.4832>0.272, surf_rev_mean 0.5906>0.3174, indexed_p95 4.7641>3.2289, surf_fwd_p95 2.4244>1.0097, surf_rev_p95 3.1883>1.3617, flips 9>7
- yarl_cloak: surf_fwd_p95 3.8348>3.5838, flips 19>10

## journal (commands + result hashes)

- `tools: pathb_gateSha=98d31b3cc621 shell_spikeSha=1d9332b9af57 pathb_metricsSha=c92929c80ba8`
- `blacksmith: rc=0 sha=1bc183425f8b cmd=C:\Users\T-Gamer\AppData\Local\Python\pythoncore-3.14-64\python.exe E:\SkyrimDev\Drape\tools\shell_spike.py --capture E:\SkyrimDev\Drape\corpus\fur_blacksmith_rm_heavy\drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap --dataset E:\SkyrimDev\Drape\tmp\pathb\gate_S04_SHELL_VANILLA_CONTROL\ds_blacksmith --solve --output E:\SkyrimDev\Drape\tmp\pathb\gate_S04_SHELL_VANILLA_CONTROL\cand_blacksmith.obj --rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target vanilla --soft-anchor on --body-mode continuation`
- `blacksmith: captureSha=4c182319a0e9 groundTruthSha=c55998911fa2 baseSha=94b698802a92`
- `yarl_clothes: rc=0 sha=50cbe35e44be cmd=C:\Users\T-Gamer\AppData\Local\Python\pythoncore-3.14-64\python.exe E:\SkyrimDev\Drape\tools\shell_spike.py --capture E:\SkyrimDev\Drape\corpus\yarl_rm_heavy\drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Clothes__RM_Heavy.drcap --dataset E:\SkyrimDev\Drape\tmp\pathb\gate_S04_SHELL_VANILLA_CONTROL\ds_yarl_clothes --solve --output E:\SkyrimDev\Drape\tmp\pathb\gate_S04_SHELL_VANILLA_CONTROL\cand_yarl_clothes.obj --rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target vanilla --soft-anchor on --body-mode continuation`
- `yarl_clothes: captureSha=3a2e940385cb groundTruthSha=31110901c2a7 baseSha=95ae65d42af4`
- `yarl_cloak: rc=0 sha=eacf522f53e3 cmd=C:\Users\T-Gamer\AppData\Local\Python\pythoncore-3.14-64\python.exe E:\SkyrimDev\Drape\tools\shell_spike.py --capture E:\SkyrimDev\Drape\corpus\yarl_rm_heavy\drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Cloak__RM_Heavy.drcap --dataset E:\SkyrimDev\Drape\tmp\pathb\gate_S04_SHELL_VANILLA_CONTROL\ds_yarl_cloak --solve --output E:\SkyrimDev\Drape\tmp\pathb\gate_S04_SHELL_VANILLA_CONTROL\cand_yarl_cloak.obj --rest base --morph-steps 6 --iterations 10 --gravity 0 --anchor-target vanilla --soft-anchor on --body-mode continuation`
- `yarl_cloak: captureSha=7f1df88df028 groundTruthSha=28576b6ccaee baseSha=3d10426b7a7c`
