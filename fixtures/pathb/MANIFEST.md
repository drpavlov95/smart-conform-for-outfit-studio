# Path B fixtures manifest (S0.1a) — 2026-07-14

Ground truth chosen per garment (S0.0c). SHA-256 full for verification.

| garment | role | status | capture (sha256_12) | ground_truth (sha256_12) |
|---|---|---|---|---|
| blacksmith | sentinel: under-butt/cofrinho | CLEAN | `4c182319a0e9` | `blacksmith__GROUND_TRUTH.obj` `c55998911fa2` |
| yarl_clothes | sentinel: anti-umbrella | CLEAN | `3a2e940385cb` | `yarl_clothes__GROUND_TRUTH.obj` `31110901c2a7` |
| yarl_cloak | sentinel: section-position/anti-imprint; NOT gravity-validated | CLEAN | `7f1df88df028` | `yarl_cloak__GROUND_TRUTH.obj` `28576b6ccaee` |
| vampire | secondary | CLEAN | `9b4d6577b1ff` | `vampire__GROUND_TRUTH.obj` `28cc517e1e8f` |
| falmer_cloth | BENCHED (contaminated GT) | BENCHED | `a6cabe6c53ec` | `falmer_cloth__sculpt_2026-07-09__1b52898da8fd.obj` `1b52898da8fd` |

## Full SHA-256

- blacksmith capture: `4c182319a0e9589b2cb667037057045e0a1dcd77b67f78f15baa4265ca48a780`
  corpus/fur_blacksmith_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap
- blacksmith ground_truth: `c55998911fa28cca23325a922e8434672bf8884f21e77d74d586ad45ceca0dac`
  fixtures/pathb/blacksmith__GROUND_TRUTH.obj
- yarl_clothes capture: `3a2e940385cb10e413b455557380ba89e949359d7ed5e5f6e403d8f0d6537c76`
  corpus/yarl_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Clothes__RM_Heavy.drcap
- yarl_clothes ground_truth: `31110901c2a7a5e8de81fea32fcb42c033609ef9aa72ba0ed2e4d3cf380322ab`
  fixtures/pathb/yarl_clothes__GROUND_TRUTH.obj
- yarl_cloak capture: `7f1df88df028e774591be018dd0e4a1c64dc69e32838c1b668df5101f0275559`
  corpus/yarl_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Cloak__RM_Heavy.drcap
- yarl_cloak ground_truth: `28576b6ccaeebbc181eac904c91ec7dfb8e6eead1c700d0c4641382c933f9709`
  fixtures/pathb/yarl_cloak__GROUND_TRUTH.obj
- vampire capture: `9b4d6577b1ff473d454335a4c7d526516faa462f30c66db7246cb7197d909890`
  corpus/vampire_rm_heavy/drape_capture__Rley_3BA_Dawnguard_-_Body_-_Vampire_Armor__Im_Physics___vampire_robes_alt__RM_Heavy.drcap
- vampire ground_truth: `28cc517e1e8f8a8362af4e25b470b60cc438ed3d5751b5d0f880d2c1709ea6eb`
  fixtures/pathb/vampire__GROUND_TRUTH.obj
- falmer_cloth capture: `a6cabe6c53ec2e58ba937bb0bf51b104a2cfbe416bbde4e98bd3e8f0b019cbf2`
  corpus/falmer_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Falmer__Im_Physics___Cloth__RM_Heavy.drcap
- falmer_cloth ground_truth: `1b52898da8fd7ba0d3962d81c2160ca3b8361b46dd3b84742ba818dedb499bc3`
  fixtures/pathb/sculpt_candidates/falmer_cloth__sculpt_2026-07-09__1b52898da8fd.obj

## Frozen hem IDs

- hem_ids_falmer_cloth.txt: `dd84810ae443`
- hem_ids_blacksmith.txt: `2f8302b7221c`

## Provenance commits

- Historical S0.1 fixture generation: `a5c6ca7`.
- S0.3e exact tool hashes are embedded in the two JSON artifacts. The
  authoritative research checkpoint is the commit containing
  `PATH_B_HANDOFF.md`.

## RM_Thin captures (gate G — contração; já existiam, S0.1d dispensa sessão live)
- `captures/yarl_cloak__RM_Thin.drcap` sha256=`4f98da8f3134b22bf76fd4aa2d8aa4e0b6b0bbad07ea1f9633d7230ec91189c8`
- `captures/yarl_clothes__RM_Thin.drcap` sha256=`75a89c6cd7b97989adf30e8c45d8e24cecc963cf2212bbd04c0793409231f294`

## S0.3e corrected-preflight artifacts (2026-07-15)

- `S03E_H3C_RESULTS.json`
  SHA-256 `3e3eb3f267301ef1a525a7e75a6b38d37f536b3ee566dc31c591b549d09e4ef6`
  — complete 2×2×2 cells, commands/output/trace hashes and common
  `never_corrected`/`always_clear` masks.
- `S03E_SECTION_RESULTS.json`
  SHA-256 `f5b52bf3278e9aeaef2d00bf5a3ae185daa2d25d40f32ac4c5845c09eb74c399`
  — production + corrected shell, explicit world XYZ/dDown and paired
  material-section metrics.
- `S03E_SURFACE_COROT_SCOREBOARD.md`
  SHA-256 `e710f5ca51c61313f1aefccb3c7a8684ea2d5b32d4dd6f73897b39e9977c770f`
  — clamp on; FAIL in the three implemented mean gates and excess flips.
- `S03E_SURFACE_COROT_NO_CLAMP_SCOREBOARD.md`
  SHA-256 `683479894751fa03b051090775e02c352de20efc7cd8aba6f8577d1ccae5cc01`
  — clamp off; the three means still fail. Together these block RM_Thin and C++.

Ground-truth cleanliness and physical role are distinct: Yarl Cloak remains a
clean hand-sculpt but its sculpt is above both candidates in every measured
band, so it cannot validate downward gravity. The gravity-sentinel slot is open.
