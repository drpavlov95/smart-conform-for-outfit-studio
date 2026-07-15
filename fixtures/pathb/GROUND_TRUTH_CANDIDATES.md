# Path B — S0.0a/b: inventário e proveniência dos ground truths

Data: 2026-07-14 · Executor: Opus · Plano: `PATH_B_SHELL_SOLVE_PLAN.md` (v2.1)
Status: **S0.0c PENDENTE — decisão do usuário no fim deste documento.**

## Método

Para cada garment-sentinela/secundário: exportadas a roupa base e o vanilla da
captura (`tools/shell_spike.py --solve`, morph-steps=1), a produção MERGED
atual (`drape-harness --replay --result-obj`), e o estado ATUAL do OSD
(`tools/osd_extract.py <osd> <Shape>RM_Heavy <base> <out>`). Cada candidato:
SHA-256 (12 hex), mtime, contagem de verts, distância POR ÍNDICE média até a
produção e entre candidatos. Cópias preservadas em
`fixtures/pathb/sculpt_candidates/` (rastreadas).

## Resultados

### blacksmith (BlackSmithTorso, 2316 verts) — LIMPO ✓

| candidato | sha | mtime | dist→PROD | notas |
|---|---|---|---:|---|
| spike_frozen (`tmp/shell_spike_blacksmith/cloth_sculpt.obj`) | c55998911fa2 | 07-10 | 1.5441 | == beltlive_today (byte-idêntico) |
| OSD ATUAL (`Blacksmith.osd`, mtime 07-09 12:13) | c8073f861634 | — | 1.5441 | difere dos snapshots por 0.0001 (arredondamento) |

Todos os candidatos concordam. `dist→PROD = 1.5441` reproduz exatamente o
baseline indexed documentado (TENSION_PLUMB_HANDOFF). **Sem contaminação.**

### falmer_cloth (Cloth, 752 verts) — CONTAMINADO ⚠️

| candidato | sha | mtime | dist→PROD | dist→vanilla | dist→base |
|---|---|---|---:|---:|---:|
| sculpt ANTIGO (smval 07-09 == beltlive_fc, byte-idênticos) | 1b52898da8fd | 07-09 | 2.4897 | 2.407 | 5.933 |
| OSD ATUAL (`Falmer.osd` salvo **2026-07-12 21:26**) | 3e30aecbedea | — | **0.2111** | 1.380 | — |

O OSD atual está a **0.21 da nossa produção** (e o arquivo foi salvo no meio da
janela de testes live de 07-12) ⇒ quase certamente o projeto foi salvo DEPOIS
de um Smart Conform — o slider hoje contém um RESULTADO NOSSO, não o
hand-sculpt. O sculpt antigo é um sculpt real e independente (2.41 do vanilla,
5.93 da base) e sobrevive apenas nos snapshots OBJ (duas cópias byte-idênticas,
de fontes diferentes). **Recomendação: usar o sculpt ANTIGO
(`1b52898da8fd`) como ground truth do falmer.**

### yarl_clothes (Clothes, 1777 verts) — LIMPO ✓

Todos os candidatos (smval == beltlive == loose, byte-idênticos; OSD atual a
0.0001) concordam; OSD do projeto intocado desde 2026-06-02. dist→PROD =
0.8388. (`loose_yc_m` tem 15428 verts — outro mesh, descartado do inventário.)

### yarl_cloak (Cloak, 553 verts) — LIMPO ✓

smval == OSD atual (0.0001); dist→PROD = 2.0463.

### vampire (vampire_robes_alt, 4005 verts) — LIMPO ✓

smval == OSD atual (0.0001); OSD intocado desde 2026-06-02; dist→PROD = 1.1858.

## Observações de baseline (confirmam a tabela §4 do plano)

As distâncias indexed→PROD medidas aqui reproduzem os baselines conhecidos:
blacksmith 1.5441, falmer 2.4897, vampire 1.1858, cloak 2.0463 — sanidade do
pipeline de medição ✓.

## S0.0c — DECISÃO (registrada 2026-07-14)

- **falmer_cloth**: **BENCHED** por decisão do usuário — não usar em nenhum
  gate/teste até re-esculpir (ground truth contaminado). Fixtures preservadas
  para quando voltar.
- **blacksmith / yarl_clothes / yarl_cloak / vampire**: ground truths LIMPOS
  (todos os candidatos concordam dentro de 1e-4). Auto-resolvidos com o
  candidato representativo (menor mtime / snapshot congelado) — o usuário pode
  sobrepor se quiser. Escolhas em `MANIFEST.md`.

S0.1a: sculpts escolhidos copiados como `<garment>__GROUND_TRUTH.obj` e
registrados em `fixtures/pathb/MANIFEST.md`.

## Correção de papel físico — S0.3e (2026-07-15)

“Ground truth limpo” significa apenas que o OBJ é um hand-sculpt independente
e estável; não significa que ele represente um equilíbrio gravitacional.
Medição material em world-Z mostrou que o sculpt do Yarl Cloak está **acima**
da produção e do shell em todas as sete bandas (`dDown<0`). Portanto:

- Yarl Cloak passa a ser sentinela de **posição de seção/anti-imprint**;
- ele **não** valida força gravitacional para baixo;
- a vaga de sentinela de gravidade fica aberta;
- Falmer continua benchado até existir um novo sculpt limpo confirmado pelo
  usuário.
