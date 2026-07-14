# Path B — Static Shell Solve: plano de pesquisa e implementação (v2)

Data: 2026-07-14 · Branch: `wip/path-b-shell-solve` · Autor: Opus (planejamento)
v2 após peer-review do Codex (2026-07-14) — bloqueadores aceitos e verificados.
Revisor: Codex · Implementador pretendido: Sonnet

---

## 0. O que é este documento

Plano completo e auto-contido para o "Path B": transformar o Smart Conform de
pipeline **correction-mode** (conserta o conform vanilla com heurísticas) em um
**solve quase-estático de shell** onde a forma do tecido emerge de leis
materiais + contato + gravidade. Quem implementa não precisa reler o histórico.
Fases bite-sized, gates numéricos, regras de parada.

**Mudanças da v2 (bloqueadores do peer-review, todos verificados no código):**
1. H3 dividida em H3a/H3b/H3c — a alegação de "uma lei única" foi retirada; o
   stretch do spike JÁ é igualdade bilateral (resiste compressão de aresta):
   `tools/shell_spike.py:900` `c = length - rest`. Preflight do Codex:
   no Blacksmith o shell defeituoso NÃO tem arestas <0.95×rest e o SCULPT TEM
   ~90 (o sculpt faz PREGAS) ⇒ piso de compressão por aresta está **rejeitado
   como fix do under-butt** e não deve nem ser implementado sem passar o
   preflight da Fase 0.3.
2. Métricas separadas e renomeadas: `tools/error_map.py` é erro **POR ÍNDICE**
   (`norm(test-ref)` vértice a vértice), não superfície. Ambas importam
   (índice pega clump tangencial; superfície pega forma). O runner reporta as
   três: indexed, surface fwd (result→sculpt), surface rev (sculpt→result).
3. Ground truth do falmer sob suspeita de contaminação (OSD modificado após o
   snapshot, hoje ≈ MERGED — pode ter sido salvo de um resultado nosso).
   Fase 0.0 preserva as duas versões + proveniência e o USUÁRIO decide.
4. Fase 0 reproduzível: fixtures versionadas (não `tmp/`), manifest tracked,
   caminhos de captura corrigidos (só nomes `git ls-files`), parser do spike
   estendido para capturas v4 (Lemoria) — hoje aceita v1-v3
   (`shell_spike.py:113`).
5. Gates de integridade adicionados (faces/seams/self-intersection/pens/
   ativação de constraint/convergência/RM_Thin/painel inclinado) e a métrica de
   hem passa a usar **lista fixa de IDs autorais** (população por z<30 é
   manipulável pela própria gravidade).
6. Correções de tabela: os números `7.92°/10.82/6.41` eram do shell-base
   experimental (não da produção MERGED) — baselines do gate C serão re-medidos
   na Fase 0.2. A captura yarl citada era um arquivo local inexistente no
   corpus tracked.

Regra de ouro do projeto: **o hand-sculpt do usuário é a loss function**, mas
métrica escalar mente e métrica offline melhor JÁ falhou gate visual live
(marco 6 do spike). Todo candidato = números (3 métricas separadas) + renders
múltiplos ângulos + gates de integridade; o veredito final é o olho do usuário.

---

## 1. Contexto do produto

- **Drape / Smart Conform**: DLL C++ (`algo/conform_hook.cpp`, ~16k linhas) que
  hooka o Conform do OutfitStudio (BodySlide) para ajustar roupas de Skyrim a
  morphs extremos (`RM_Heavy`=100%). Será publicado como mod ⇒ **nada de
  solução por-garment/allowlist** (decisão explícita do usuário, 2026-07-13).
- **Harness offline**: `build/algo/RelWithDebInfo/drape-harness.exe`
  (`--replay cap.drcap --result-obj --body-obj --refbody-obj`). Corpus:
  `py -3 scripts/check_corpus.py` vs `corpus/golden/*.json` (18 shapes;
  goldens casam com o binário **RelWithDebInfo**, o default do script).
- **Pedido do usuário (2026-07-13)**: "está parecendo mais um 'conform all
  melhorado' do que um conform all com cloth simulation de verdade... o tecido
  não respeita a gravidade". Sintomas live: bump atrás da coxa em roupas
  penduradas (falmer/vampire/lemoria/fur; NÃO blacksmith/yarl), tecido livre
  herdando a silhueta do corpo à distância.

## 2. Evidência acumulada — o que JÁ SABEMOS (não re-derivar)

### 2.1 Raiz do defeito (medida)
O caminho de produção (correction-mode) usa a roupa vanilla-conformada como
posição E métrica de repouso (`conform_hook.cpp` ~5543) ⇒ tecido livre herda o
crescimento radial do corpo ("imprint"). Falmer Cloth: pano pendura a gap
~4.2u, corpo cresceu +5.29u ali. O sculpt cai RETO (~16.5 constante no perfil
back-thigh x∈[7,14]) enquanto o corpo vai 10.7→13.9.

### 2.2 Path A (religar o "verlet") — FALSIFICADO 2026-07-13
O caminho LOOSE_DRAPED é PBD quase-estático (sem momentum) com stretch
bilateral, seams, proximidade, colisão e bending por distância de vértices
opostos (NÃO diedro assinado; `conform_hook.cpp:348`). Insuficiências: bending
não-diedral mistura dobra/shear; sem self-collision; rest do vanilla assa o
imprint. Nota: no solver de PRODUÇÃO a compressão é null-space conhecido; no
SPIKE o stretch já é bilateral (v2 correção 1).

Erro **POR ÍNDICE** vs sculpt (`error_map.py`; comparação apples-to-apples —
é exatamente a métrica que pega clump de tiras) + superfície (medição Codex):

| garment | MERGED indexed | VERLET indexed | MERGED surface fwd/rev |
|---|---:|---:|---:|
| falmer Cloth | **2.490** | 4.42 | 0.926 / 1.275 |
| vampire robe | **1.186** | 2.18 | 0.503 / 0.555 |
| yarl Cloak | **2.046** | 2.10 | 1.218 / 0.904 |

Colapso real do verlet: tiras do hem do falmer amontoam (bandas ~0.27-0.53×
o arco base vs sculpt 0.87-1.07× — medição preflight do Codex). ⇒ **NÃO religar
nem tunar o verlet.** MERGED é o melhor baseline atual.

### 2.3 O spike JÁ existe e já falsificou hipóteses (`SHELL_SPIKE_LOG.md`)
`tools/shell_spike.py` (~1600 linhas; `--inspect/--contact-check/--solve`).
- **Validado**: parser v1-v3 do `.drcap`; continuação do morph
  `B(t)=B0+t(B1-B0)`; contato unilateral (broad phase determinística + closest
  point exato); stretch XPBD bilateral; diedro assinado autoral; weld de seam
  twins; mobilidade contínua (cutoff duro 0.20 abriu stretch 6-8x — usar a
  transição contínua); determinismo SHA-idêntico.
- **FALSIFICADO (marco 1)**: "contato + continuação + membrana/diedro locais
  reproduzem o sculpt fora de contato" — FALSO. Vale só: contato+continuação
  apaga a fenda horizontal sem ROI anatômica.
- **FALSIFICADOS (NÃO REPETIR)**: sweeps de rest (base/uniform/pre-tension/
  flat-panel/lowpass — melhor 0.9548 vs produção 0.7538 indexed); gravity
  shadow (todas as variantes); energia de meridiano; graph plumb uniforme
  (conserta Blacksmith 0.8779, REGRIDE yarl — painel abre p/ y~11.8-12 vs
  vanilla 10-11).
- **Marcos 4-6**: tension-seeded plumb portado ao C++
  (`ApplyTensionSeededGraphPlumb`, `DRAPE_TENSION_PLUMB`, default OFF):
  métricas do Blacksmith melhoram (0.7538→0.7306 indexed), **gate visual live
  FALHOU** e foi revertido. Mecanismo de suporte-por-tração continua candidato,
  insuficiente sozinho na porção baixa do under-butt.
- Infra parcial existente: `build_band_circumference_constraints` /
  `xpbd_band_circumference` (~:749/:790) — HOJE é (a) soma de arestas
  ~horizontais INCLUINDO diagonais internas, (b) componentes topológicos SEM
  unir seams, (c) agrupamento por Z global, (d) TETO (`arc ≤ rest`), não piso.
  Ou seja: o descriptor NÃO é uma circunferência e precisa de redesenho
  (Fase 1) — não basta inverter o sinal.

### 2.4 Hipóteses da etapa (v2 — separadas, cada uma falsificável)
- **H3a (modelo local assimétrico)**: teto de extensão + piso de compressão
  COM FOLGAS INDEPENDENTES vs a igualdade bilateral atual. Só entra se passar
  o preflight 0.3 (prior do Codex: no Blacksmith NÃO passa — o sculpt viola o
  piso; no falmer as tiras amontoadas podem passar).
- **H3b (suporte global de baixa frequência por bandas)**: piso de
  circunferência/folga autoral por banda MATERIAL (descriptor redesenhado:
  loops de superfície com seams unidos, sem diagonais, não dependente só de
  Z global). Prior: promissor no falmer (0.27-0.53 vs 0.87-1.07); no
  Blacksmith o piso de `arc_base` nem ativa ⇒ pode resolver falmer sem
  explicar o under-butt — aceitável, contanto que não regrida os outros.
- **H3c (anti-imprint)**: rest SEMPRE autoral (base) + continuação do morph +
  corpo só como obstáculo — já parcialmente validado (fenda horizontal);
  permanece como fundação de todos os candidatos.
- **Under-butt do Blacksmith permanece SEM candidato comprovado** após os
  marcos 1-6 (nem compressão, nem banda, nem plumb uniforme, nem tension-plumb
  sozinho). O preflight 0.3 deve caracterizá-lo ANTES de propor família nova
  (pistas: o sculpt TEM pregas — 90 arestas <0.95×rest — e sustenta o raio:
  a resposta provável combina suporte-por-tração + permitir prega SEM perder
  raio, não proibir compressão).

## 3. Garments-sentinela e dados (caminhos verificados por `git ls-files`)

Sentinelas (julgados SEMPRE juntos):
- `corpus/fur_blacksmith_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap`
- `corpus/falmer_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Falmer__Im_Physics___Cloth__RM_Heavy.drcap`
- `corpus/yarl_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Yarl_Clothes_2__No_Physics___Clothes__RM_Heavy.drcap`
  (“No Physics” é nome do projeto, não condição do solver. PENDÊNCIA de
  preflight: uma medição 2026-07-13 achou saída byte-idêntica com
  `DRAPE_MERGE_LOOSE=0/1` para este garment; o peer-review afirma que ele entra
  no solve. Resolver na Fase 0.2 via HLog do caminho executado e REGISTRAR.)

Secundários (gate final da Fase 2): vampire_robes_alt, yarl Cloak,
Lemoria SkirtA/B (`captures/universality/lemoria_rm_heavy_v4/`, formato v4 —
exige a extensão de parser da S0.1; se a extensão travar, adiar Lemoria para a
Fase 3 e registrar).

Sculpts/referências: ver Fase 0.0 (proveniência PRIMEIRO; nada de confiar em
`tmp/smval`/`tmp/beltlive` sem re-derivar e hashear).

## 4. Gates (valores preenchidos na Fase 0.2 pelo runner — não deste texto)

| gate | métrica | baseline (medir em 0.2) | critério do candidato |
|---|---|---|---|
| A. Blacksmith | indexed + surface fwd/rev vs sculpt | produção ≈0.7538 indexed (confirmar) | ≤ baseline em indexed E surface fwd; hard-fail se indexed >0.80 |
| B. falmer forma | indexed + surface fwd/rev vs sculpt decidido em 0.0 | MERGED ≈2.49 indexed / 0.926 surf | melhor que MERGED em indexed E surface fwd |
| B2. falmer queda | perfil back-thigh x∈[7,14]: bulge local máx vs sculpt (~16.5) | MERGED +2.6u | ≤ +0.8u |
| B3. falmer tiras | com IDs FIXOS de hem autoral: spread lateral std + distância média entre tiras não-adjacentes | medir | ∈ [0.75, 1.30] × sculpt |
| C. yarl Clothes | vs vanilla da captura: mean bend / worst stretch / máx vanilla→result / abertura do painel z44-60 | medir em 0.2 (produção) | nenhuma piora >10%; painel não abre além do baseline+0.5u |
| I. integridade | TODOS: faces invertidas=0 novas; área degenerada=0 novas; seams abertos=0; self-intersections novas=0; pens de corpo ≤ baseline; NaN=0 | medir | hard-fail |
| D. determinismo | SHA-256 de 2 runs | — | idêntico, sempre |
| E. corpus | check_corpus com feature OFF | verde | verde inalterado |
| F. convergência | mesmas conclusões em morphSteps {3,6,12} × iterations {5,10,20} | — | ranking de candidatos estável |
| G. contração | RM_Thin (ou slider de contração disponível) + 1 painel inclinado (falmer Strap ou lemoria) | medir | sem inversão/colapso; gates I e D valem |

## 5. Fases e tarefas (bite-sized, para Sonnet)

**Regras gerais**: branch `wip/path-b-shell-solve`; Fases 0-2 tocam SÓ
`tools/`, `fixtures/pathb/` e `tmp/pathb/`; proibido editar
`algo/conform_hook.cpp` e goldens; commits pequenos por marco com gates da
tarefa rodados; atualizar `SHELL_SPIKE_LOG.md` a cada marco; se um gate falhar
2 tentativas honestas: parar a tarefa, registrar como falsificado, seguir o
plano. NÃO fazer sweep por-garment. Saídas de execução em `tmp/pathb/`;
fixtures e manifests em `fixtures/pathb/` (TRACKED).

### FASE 0.0 — Proveniência do ground truth (BLOQUEIA tudo; ½ dia)
- **S0.0a**: inventariar TODAS as versões de sculpt existentes por garment
  (`tmp/smval/*_sculpt.obj`, `tmp/beltlive/*sculpt*.obj`,
  `tmp/shell_spike_blacksmith/cloth_sculpt.obj`, OSDs atuais via
  `tools/osd_extract.py`): para cada uma, SHA-256, data, origem provável, e
  distância indexed até (a) o MERGED atual e (b) as outras versões. NÃO
  sobrescrever nada.
- **S0.0b**: relatório curto por garment: "candidato X (velho) difere do OSD
  atual por N; OSD atual está a M do MERGED (suspeita de contaminação se
  M≈0)". Blacksmith e falmer primeiro.
- **S0.0c — DECISÃO DO USUÁRIO**: apresentar o relatório e ele aponta qual
  versão é o hand-sculpt verdadeiro por garment. Registrar a escolha + SHA em
  `fixtures/pathb/GROUND_TRUTH.md`. **Gate B fica bloqueado até esta decisão.**

### FASE 0.1 — Fixtures reproduzíveis (½ dia)
- **S0.1a**: criar `fixtures/pathb/` (tracked) com: os sculpts escolhidos
  (arquivos pequenos, OBJ), `MANIFEST.md` com — caminho+SHA de cada capture,
  OSD/DataName+SHA, proveniência do sculpt, commit da ferramenta, comando
  integral de export, SHA dos OBJs derivados. Derivados grandes (base/vanilla/
  corpos exportados do harness) ficam em `tmp/pathb/` mas com SHA registrado
  no manifest para verificação.
- **S0.1b**: estender o parser do spike para aceitar capturas v4 lendo o
  prefixo v3 (v4 é append-only; `UNIVERSALITY_HANDOFF.md`). Gate: `--inspect`
  numa captura Lemoria v4 valida contagens; numa v3 continua byte-igual ao
  comportamento atual (rodar `--inspect` no Blacksmith antes/depois e
  comparar a saída).
- **S0.1c**: congelar a LISTA DE IDs de hem/tiras do falmer (e Blacksmith) a
  partir da roupa BASE autoral (seleção geométrica documentada, ex.: verts de
  boundary com z autoral < limiar por componente), gravada no manifest.
  Gate: determinística (2 runs = mesma lista).

### FASE 0.2 — Métricas separadas + baseline real (½-1 dia)
- **S0.2a**: `tools/pathb_metrics.py` — por (garment, OBJ): indexed mean/p95;
  surface fwd/rev mean/p95 (closest-point-em-triângulo, reusar a máquina do
  spike; NUNCA rotular indexed de "surface"); perfil back-thigh; hem por IDs
  fixos (spread std + distância entre tiras não-adjacentes); integridade
  (faces invertidas vs base, área degenerada, seams abertos via
  exactSeamGroups, self-intersections não-adjacentes via broad phase, pens de
  corpo, NaN); SHA do OBJ. Uma linha-resumo por OBJ + bloco detalhado.
- **S0.2b**: `tools/pathb_gate.py` — roda a matriz (N capturas × preset de
  knobs), chama o metrics, emite scoreboard markdown; grava comando+SHAs.
- **S0.2c**: medir os BASELINES reais: produção MERGED atual e, para
  contexto, o shell-base do spike, nos 3 sentinelas + secundários. Preencher a
  tabela da seção 4 num commit ("fase 0.2: baselines"). Gate de sanidade:
  Blacksmith produção indexed ≈0.7538±0.01; reproduzir o marco 2 do log
  (graph-plumb 0.4614 → 0.8779±0.001).
- **S0.2d**: resolver a pendência do yarl Clothes (caminho de solve executado,
  via HLog `%TEMP%\drape_debug.log` com `DRAPE_MERGE_LOOSE=0/1`) e registrar.

### FASE 0.3 — Preflight de necessidade (o coração da v2; ½ dia)
Para cada família candidata (H3a piso-compressão-aresta; H3b piso-banda;
tension-seeded support marco-4; e qualquer nova), medir NOS DADOS (defeito
atual + sculpt escolhido + shell-base):
1. o resultado DEFEITUOSO viola a constraint? (se não: ela não conserta nada)
2. o SCULPT satisfaz a constraint? (se não: ela IMPEDE o alvo)
3. existe margem mensurável entre os dois? (tamanho do sinal)
Registrar a tabela verdade por garment×família no log ("marco 7: preflight").
Priors já medidos pelo Codex a confirmar: Blacksmith×H3a = REPROVA (sculpt tem
90 arestas <0.95×rest; defeito tem 0); falmer×H3b = aprova (0.27-0.53 vs
0.87-1.07). **Saída da fase: lista fechada de famílias a implementar, com
justificativa numérica. Só o que passar entra na Fase 1.**

### FASE 1 — Implementação das famílias aprovadas (1-1.5 dia)
- **S1.1 (se H3b aprovar)**: redesenhar o descriptor de banda: loops de
  superfície por componente-com-seams-unidos, SEM diagonais internas, eixo por
  "down" autoral do componente (não Z global — capas/mangas/bandoliers),
  piso-com-folga `arc ≥ (1-εb)·arc_base`. Testes de invariância obrigatórios:
  duplicação de seam, diagonais invertidas, subdivisão (construir 3 variantes
  sintéticas pequenas e verificar que o descriptor não muda além de ε).
- **S1.2 (se H3a aprovar em algum garment)**: separar o stretch em teto/piso
  com folgas independentes (`--stretch-ceiling-slack`, `--compression-slack`),
  mantendo o modo igualdade como default para A/B.
- **S1.3 (se preflight apontar)**: revisitar tension-seeded support DENTRO do
  loop XPBD do spike (não como projeção final) — o marco 4-6 indica que ele
  ajuda mas precisa conviver com as outras constraints, não vir depois.
- **S1.4 (opcional)**: self-proximity mínimo para tiras (repulsão de pares
  não-adjacentes < r_min via broad phase), flag própria, default off.
- Todos: contador de ATIVAÇÃO + residual por família por run (sai no metrics);
  determinismo; ordem fixa.

### FASE 2 — Matriz de falsificação (1-1.5 dia) ← VEREDITO
- **S2.1**: matriz nos 3 sentinelas: {famílias aprovadas, isoladas e
  combinadas} × folgas {2-3 valores por família} × gravity {0, leve};
  morph-steps=6, iterations=10; rest SEMPRE base. Proibido: shadow, plumb
  uniforme, rest sweeps.
- **S2.2**: scoreboard + renders (left E back) por célula; vencedor = passa
  A+B+B2+B3+C+I+D com UM conjunto de knobs.
- **S2.3**: vencedor roda gates F (convergência) e G (RM_Thin + inclinado) +
  secundários; 2 runs SHA-idênticos; "marco 8" no log com comando.
- **PARADA**: sem célula que passe A+B2+C+I juntos ⇒ registrar marco negativo
  (números+renders), commitar e PARAR — decisão volta ao usuário/Opus. NÃO
  improvisar famílias fora do preflight.

### FASE 3 — Port C++ isolado (1-2 dias; SÓ com Fase 2 verde)
- **S3.1**: `algo/static_shell_solver.h/.cpp` novo e isolado; em
  `conform_hook.cpp` SÓ: gate `DRAPE_SHELL_SOLVE` (default OFF) + flag live
  `shell_solve_on.flag` + chamada substituindo o resultado para presets
  Fitted/merged-Loose. Zero mudanças com flag OFF (corpus prova).
- **S3.2**: paridade Python↔C++ (mesmo garment/knobs: diferença média <0.05u,
  mesmos gates; bit-igualdade exigida ENTRE runs C++, não entre linguagens).
- **S3.3**: corpus OFF=verde intocado; ON: medir os 18 shapes e REGISTRAR (sem
  promover golden); tempo por shape ≤ ~5s (precedente aceito: 3.9s).
- **S3.4**: build RelWithDebInfo; verificar deploy nas 2 pastas (SHA idêntico
  das 3 cópias); flag OFF; commit + push.

### FASE 4 — Gate visual live (usuário) e promoção
Usuário liga `shell_solve_on.flag`, REINICIA o OutfitStudio (DLL carrega no
launch), reconforma sentinelas+secundários e julga. Só depois: default ON e
remoção de passes redundantes — um por vez, cada um com corpus + A/B próprios.

## 6. Armadilhas conhecidas (ler antes de cada fase)
1. `check_corpus.py` default RelWithDebInfo = goldens atuais; drift impossível
   ⇒ `git stash` e checar HEAD antes de crer/`--update`.
2. DLL carrega no LAUNCH do OS; flags `.flag` são lidos por conform.
3. Renders matplotlib enganam em features sutis; usar os perfis numéricos E
   múltiplos ângulos; o olho do usuário é o juiz final.
4. `error_map.py` = INDEXED. Nunca chamar de surface. `score_sculpt.py` lê
   caches velhos — não usar sem re-gerar.
5. OSD pode ter sido salvo DE um resultado nosso ⇒ proveniência antes de
   confiar em qualquer "sculpt" (Fase 0.0; vale para o futuro também).
6. Seam twins: weldar `exactSeamGroups` por iteração; tears invisíveis a
   métricas de stretch (detector: `tmp/beltlive/twincheck.py`).
7. Cutoff de mobilidade 0.20 duro ⇒ stretch 6-8x; usar transição contínua.
8. Vampire tem placas internas autorais: pens "profundas" podem ser placa
   escondida (12/13 no caso medido) — julgar por superfície visível.
9. O stretch do spike é IGUALDADE BILATERAL (`:900`) — não assumir compressão
   livre no spike; essa suposição custou a H3 da v1.
10. PowerShell 5.1 sem `&&`; heredocs Python via Bash tool (`py -3 -`).

## 7. Housekeeping incluído
- Fase 0.0/0.1 cobrem: sculpt falmer (duas versões preservadas), fixtures
  tracked, manifest completo, parser v4.
- Registrar a investigação Path A no `SHELL_SPIKE_LOG.md` ("marco 6.5") — 10
  linhas com a tabela §2.2 (indexed + surface, rotuladas corretamente).
- `UNIVERSALITY_HANDOFF.md` tem hash antigo de DLL — fora de escopo; anotado.

## 8. Referências
- Spike: `tools/shell_spike.py` (stretch bilateral `:886-900`; bandas
  `:749/:790`; parser v1-v3 `:113`; CLI `~:1525-1568`). Diário:
  `SHELL_SPIKE_LOG.md` (marcos 1-6). Render:
  `tools/render_shell_diagnostics.py`.
- Produção: bending por distância `algo/conform_hook.cpp:348`; merge
  Loose→Fitted `:7695`; rest do vanilla `~:5543`; tension-plumb
  `ApplyTensionSeededGraphPlumb` (`DRAPE_TENSION_PLUMB`, OFF).
- Falsificação Path A: memória `project-verlet-regresses-sculpt` + §2.2.
- Peer-reviews do Codex: 2026-07-13 (arquitetura) e 2026-07-14 (v1 deste
  plano — bloqueadores incorporados nesta v2).
