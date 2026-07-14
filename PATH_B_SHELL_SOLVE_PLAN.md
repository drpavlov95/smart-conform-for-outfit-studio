# Path B — Static Shell Solve: plano de pesquisa e implementação

Data: 2026-07-14 · Branch: `wip/path-b-shell-solve` · Autor: Opus (planejamento)
Revisor pretendido: Codex (peer review) · Implementador pretendido: Sonnet

---

## 0. O que é este documento

Plano completo e auto-contido para a próxima etapa do "Path B": transformar o
Smart Conform de um pipeline **correction-mode** (conserta o conform vanilla com
passes heurísticos) em um **solve quase-estático de shell** onde a forma do
tecido emerge de leis materiais + contato + gravidade. Este arquivo carrega TODO
o contexto necessário: quem for implementar não precisa reler o histórico do
projeto. As fases são bite-sized, com gates numéricos e regras de parada, para
implementação por um modelo mais econômico (Sonnet) sem re-derivação.

Regra de ouro herdada do projeto: **o hand-sculpt do usuário é a loss function**
(refs em `tmp/smval/*_sculpt.obj` e datasets congelados). Métrica escalar mente;
todo candidato é julgado por superfície-a-superfície CONTRA O SCULPT **e** por
renders em múltiplos ângulos **e**, no final, pelo gate visual live do usuário
(marco 6 do spike provou que métricas offline podem melhorar e o live falhar).

---

## 1. Contexto do produto

- **Drape / Smart Conform**: DLL C++ (`algo/conform_hook.cpp`, ~16k linhas) que
  hooka o Conform do OutfitStudio (BodySlide) para ajustar roupas de Skyrim a
  morphs extremos de corpo (slider `RM_Heavy` a 100%). Será publicado como mod —
  logo **nenhuma solução por-garment/allowlist é aceitável** (decisão explícita
  do usuário 2026-07-13).
- **Harness offline**: `build/algo/RelWithDebInfo/drape-harness.exe` faz replay
  determinístico de capturas `.drcap` (roupa base, conform vanilla, corpo base E
  morphado — topologias corpo base/morph idênticas, diff por vértice). Corpus de
  regressão: `scripts/check_corpus.py` vs `corpus/golden/*.json` (18 shapes, 4
  outfits). Goldens atuais casam com o binário **RelWithDebInfo** (default do
  script).
- **Pedido do usuário (verbatim, 2026-07-13)**: "está parecendo mais um
  'conform all melhorado' do que um conform all com cloth simulation de
  verdade... o tecido não respeita a gravidade". Sintomas concretos ao vivo:
  bump atrás da coxa em roupas penduradas (falmer, vampire, lemoria, fur 1;
  NÃO blacksmith/yarl), tecido livre "herdando" a silhueta do corpo à distância.

## 2. Evidência acumulada — o que JÁ SABEMOS (não re-derivar)

### 2.1 Por que o caminho atual erra (raiz, medida)
O caminho de produção (`Fitted Cloth`, correction-mode) parte do conform
vanilla e usa a roupa **já conformada** como posição E métrica de repouso
(rest lengths do vanilla: `conform_hook.cpp` ~5543; anchors para supportVerts).
O vanilla preserva o gap autoral sobre o corpo MORPHADO ⇒ tecido livre herda o
crescimento radial do corpo ("imprint"). Medido no falmer Cloth: na coxa o
tecido pendura a gap ~4.2u do corpo, mas o corpo cresceu +5.29u ali, e o pano
carrega parte disso (o bump). O sculpt do usuário, ao contrário, cai **reto**:
perfil back-thigh (x∈[7,14]) constante ~16.5 enquanto o corpo vai 10.7→13.9.

### 2.2 Path A (religar o verlet) — FALSIFICADO 2026-07-13
O caminho LOOSE_DRAPED ("verlet") é, na verdade, **relaxação PBD quase-estática**
(sem momentum) com stretch, seams, proximidade, colisão e bending aproximado
(distância entre vértices opostos, NÃO diedro assinado — `conform_hook.cpp:348`).
Correção do peer-review do Codex aceita: não é "física crua sem estrutura"; é
estrutura **insuficiente**:
- stretch resiste à extensão, **compressão é grátis** (null-space conhecido);
- "bending" por distância mistura dobra/shear, não preserva diedro autoral;
- sem self-collision geral;
- rest metrics do vanilla assam o imprint no repouso.

Resultado medido (erro superfície vs sculpt, `tools/error_map.py`):

| garment | MERGED (produção) | VERLET | veredito |
|---|---:|---:|---|
| falmer Cloth | **2.49** | 4.42 | verlet colapsa as tiras do hem (8 verts std 0.28 vs sculpt 44 verts std 4.63) |
| vampire robe | **1.19** | 2.18 | idem |
| yarl Cloak | **2.05** | 2.10 | ~empate |
| yarl Clothes | = | = | "No Physics", nunca entra no verlet |

⇒ **NÃO religar/tunar o verlet.** O merged é o melhor match atual; seu resíduo
é o bump (visualmente saliente, numericamente pequeno: remover só o bump por
plumb geométrico move 2.49→2.47).

### 2.3 O spike do Path B JÁ EXISTE e já falsificou hipóteses
`tools/shell_spike.py` (1612 linhas; CLI: `--inspect`, `--contact-check`,
`--solve` + knobs) + diário `SHELL_SPIKE_LOG.md`. Estado ao fim do marco 6:

- **Validado**: parser do `.drcap`; continuação do morph `B(t)=B0+t(B1-B0)`
  em passos fixos; contato unilateral (broad phase determinística + closest
  point exato); stretch XPBD; **bending por diedro assinado autoral**; weld dos
  seam twins; mobilidade contínua (cutoff duro 0.20 da produção abriu stretch
  6-8x — usar transição contínua); determinismo (SHA-256 idêntico em reruns).
- **FALSIFICADO (marco 1)**: a hipótese forte — "obstáculo corporal + membrana/
  dobra locais + continuação do morph reproduzem o sculpt no espaço sem
  contato" — é FALSA. Confirmado apenas: contato+continuação **apaga a fenda
  horizontal** (não copia o cofrinho) sem ROI anatômica.
- **FALSIFICADOS (marcos 1-3, NÃO REPETIR):** sweep de rest metrics (base /
  uniform growth / pre-tension isotrópica / flat-panel / lowpass — lowpass foi
  o melhor shell local, 0.9548, ainda pior que produção 0.7538); gravity-shadow
  em todas as variantes (corpo inteiro / contato exato / 1-3 anéis / falloff /
  radial — melhor 0.9540, superinfla ou faz picos); energia de meridiano local;
  **graph plumb uniforme** (0.8779 no Blacksmith mas REGRIDE o yarl Clothes:
  abre o painel para y~11.8-12 vs vanilla 10-11, piora bend 7.9°→9.7°, strain,
  worst stretch — mesma lição: parâmetro que conserta um garment quebra outro).
- **Marcos 4-6 (tension-plumb)**: versão semeada por contato e transmitida só
  por arestas em tração foi portada pro C++ (`ApplyTensionSeededGraphPlumb`,
  `DRAPE_TENSION_PLUMB`, hoje default OFF). Melhorou métricas do Blacksmith
  (0.7538→0.7306) sem regressões, **mas o gate visual live FALHOU** (usuário
  não viu o defeito resolvido) e a promoção foi revertida. Lição dupla: (a)
  métrica offline melhora ≠ visual resolve; (b) o mecanismo de suporte por
  tração é promissor mas insuficiente sozinho na porção baixa do under-butt.
- Infra parcial que JÁ EXISTE no spike e deve ser auditada antes de codar:
  `build_band_circumference_constraints`/`xpbd_band_circumference` (linhas
  ~749/790, knobs `--band-circumference-compliance` (default -1=off),
  `--band-z-cell`, `--band-rest base|vanilla`) e `--gravity` — **sem matriz de
  falsificação registrada no log** para essas famílias.

### 2.4 Conclusão do peer-review (Codex, 2026-07-13) — aceita
Path A descartado; Path B vale como **pesquisa harness-first**; diedro+gravidade
sozinhos NÃO resolvem (já falsificado); é preciso adicionar **preservação
global de pattern/suporte, resistência à compressão, proximidade/self-contact e
mobilidade contínua**. Gate offline multi-garment ANTES de qualquer port a DLL.
Estimativa honesta: 2-4 dias para candidato sério no harness; 1-2 semanas para
integração live experimental; 2-4 semanas para universal.

## 3. Hipótese central desta etapa (H3) e fundamentação

**H3:** Sobre o shell já validado (continuação do morph + contato unilateral +
stretch XPBD + diedro assinado autoral + seams + mobilidade contínua + gravidade),
adicionar **resistência à compressão in-plane** (arestas têm piso de comprimento,
não só teto) e sua forma integrada de baixa frequência, **conservação de
circunferência/folga autoral por banda**, é suficiente para reproduzir
simultaneamente — em Blacksmith E falmer E yarl, sem ROI anatômica e sem tuning
por-garment — as propriedades do sculpt no espaço sem contato:
queda reta sob gravidade, sem imprint do corpo, tiras/hem espalhadas, under-butt
sustentado.

**Por que compressão é a lei que falta (síntese dos 3 sintomas):**
1. *Tuck do under-butt* — abaixo do ápice o tubo de tecido só consegue "afinar"
   se as arestas circunferenciais ENCURTAREM. Com compressão grátis (estado
   atual do PBD), afinar é de graça e o closest-point/relaxação puxa pra dentro.
   Com piso de compressão, o excesso vira prega/queda reta — que é o que tecido
   real faz e o que o sculpt mostra.
2. *Clump das tiras do falmer (o colapso real do verlet)* — as tiras se amontoam
   porque compressão lateral é grátis. Piso de compressão + conservação de arco
   por banda mantém a distribuição autoral.
3. *Imprint/bump* — rest SEMPRE da roupa BASE (autoral), nunca do vanilla
   conformado; o corpo só participa como obstáculo (já é regra do spike). O que
   mantém o pano afastado reto abaixo do suporte é a inextensibilidade vertical
   + gravidade + dobra; o que impede de "afinar de volta" é a compressão.

**Riscos conhecidos de H3 (honestidade com o marco 3):** conservação de
circunferência com rest=base pode ser FORTE demais em painéis largos autorais
(yarl) e criar guarda-chuva. Por isso a família entra como **desigualdade com
folga** (piso `(1-εc)·rest`, εc varrido) e é julgada **simultaneamente** nos
três garments-sentinela — nunca tunada num só. Se H3 falhar nesse critério, o
plano PARA e registra (ver regras de parada), sem sweep infinito.

## 4. Garments-sentinela, dados e alvos numéricos (gates)

Sentinelas (capturas já no repo):
- `corpus/fur_blacksmith_rm_heavy/...BlackSmithTorso__RM_Heavy.drcap` (fitted justo; alvo: cofrinho/under-butt)
- `corpus/falmer_rm_heavy/...Cloth__RM_Heavy.drcap` (penduradas + tiras; alvo: queda reta + spread)
- `corpus/yarl_rm_heavy/...Clothes...161653.drcap` (painel largo; alvo: NÃO regredir — sentinela anti-guarda-chuva)
- Secundários no gate final: vampire_robes_alt, yarl Cloak, Lemoria SkirtA/B (`captures/universality/lemoria_rm_heavy_v4/`).

Referências de sculpt: `tmp/smval/falmer_rm_heavy_Cloth_sculpt.obj`,
`tmp/smval/vampire_rm_heavy_vampire_robes_alt_sculpt.obj`,
`tmp/smval/yarl_rm_heavy_Cloak_sculpt.obj`, Blacksmith:
`tmp/shell_spike_blacksmith/cloth_sculpt.obj`. **Atenção (housekeeping do
peer-review): o OSD do falmer foi modificado depois do snapshot** ⇒ Fase 0
re-extrai e CONGELA todos os sculpts com SHA-256 num manifest.

Números a bater (baseline = produção MERGED atual, medidos 2026-07-13):

| gate | métrica | baseline | alvo do candidato |
|---|---|---:|---|
| A. Blacksmith sculpt | surf mean result→sculpt / reverse | 0.7538 / 0.7868 | ≤ baseline (meta ≤0.73); hard-fail >0.80 |
| B. falmer sculpt | surf mean vs sculpt | 2.49 | < 2.49 |
| B2. falmer queda reta | perfil back-thigh x∈[7,14]: máx bulge local vs sculpt (~16.5 const; merged tem 19.1 em z44) | +2.6u | ≤ +0.8u |
| B3. falmer tiras | hem z<30: nº verts na banda / std radial (sculpt 44/4.63) | 36/6.90 | ≥30 / ∈[3.5, 6.0] |
| C. yarl Clothes | vs vanilla da captura (sem sculpt): mean bend / worst stretch / máx vanilla→result | 7.92° / 10.82 / 6.41 | sem piora >10% em qualquer; painel NÃO abre além de y~11 nas bandas z44-60 |
| D. determinismo | SHA-256 de 2 runs idênticos | — | idêntico, sempre |
| E. corpus | check_corpus com feature OFF | verde | verde inalterado |

## 5. Fases e tarefas (bite-sized, para Sonnet)

**Regras gerais de engenharia (valem para TODAS as tarefas):**
- Branch `wip/path-b-shell-solve`; commits pequenos por marco, mensagem em EN,
  rodando os gates da tarefa antes de commitar. NUNCA tocar `master`.
- Fases 0-2 são **somente** `tools/` + `tmp/pathb/` (gitignored): proibido
  editar `algo/conform_hook.cpp` ou goldens.
- Todo resultado: número + render (`tools/render_shell_diagnostics.py` com body
  + sculpt + candidato; ângulos left E back no mínimo) + SHA de determinismo.
- Saídas em `tmp/pathb/<garment>/...`; atualizar `SHELL_SPIKE_LOG.md` a cada
  marco (o diário é parte do produto).
- Se um gate falhar 2 tentativas de ajuste honesto: PARAR a tarefa, registrar
  no log, marcar a hipótese específica como falsificada e seguir o plano (ou
  parar o plano, se for gate de parada). NÃO fazer sweep por-garment.

### FASE 0 — Fundações e baseline congelado (½ dia)
- **S0.1 Congelar datasets**: para cada sentinela+secundário, exportar via
  harness (`--replay <cap> --result-obj ... --body-obj ... --refbody-obj ...`)
  base/vanilla/corpo-base/corpo-morph/produção-atual; re-extrair sculpts do OSD
  (`tools/osd_extract.py` — NÃO usar caches de `tmp/smval` sem re-gerar;
  verificar contagem de verts == captura); gravar tudo + SHA-256 em
  `tmp/pathb/MANIFEST.txt`. Gate: contagens batem; 2 exports = mesmo SHA.
- **S0.2 Métricas do plano**: criar `tools/pathb_metrics.py` que imprime UMA
  linha por garment com: surf mean/p95 (ambas direções, reusar lógica do
  error_map), perfil back-thigh (máx bulge local vs sculpt), hem-spread
  (nº verts/std na banda z<30), mean bend vs autoral, worst stretch, SHA do OBJ.
  Determinístico. Gate: rodar no baseline MERGED e reproduzir (±0.01) os
  números da tabela da seção 4.
- **S0.3 Runner da matriz**: criar `tools/pathb_gate.py` que roda shell_spike
  em N capturas com um preset de knobs e emite a tabela markdown do scoreboard
  (uma linha por garment × candidato). Gate: reproduz o candidato "marco 2"
  do log (graph-plumb 0.4614 no Blacksmith → 0.8779±0.001).

### FASE 1 — Famílias de constraint novas no spike (1-1.5 dia)
- **S1.1 AUDITORIA (antes de codar)**: ler `build_band_circumference_constraints`
  + `xpbd_band_circumference` (shell_spike.py ~749/790) e documentar no log o
  que fazem hoje (igualdade? desigualdade? por componente?). Se já forem
  desigualdade-com-folga utilizável, S1.3 vira só exposição de knobs.
- **S1.2 Compressão in-plane (piso de aresta)**: no loop XPBD, adicionar
  constraint bilateral inferior: `len ≥ (1-εc)·rest_base`, compliance própria
  (`--compression-compliance`, default off), εc knob (`--compression-slack`,
  default 0.05). Aplicar no MESMO loop (Gauss-Seidel/Jacobi consistente com o
  existente), ordem fixa. Gate: determinismo; com εc=0.05 no falmer, hem-spread
  std sobe vs shell-base (direção certa) sem worst-stretch explodir (>1.3×
  baseline da variante).
- **S1.3 Circunferência por banda como DESIGUALDADE com folga**: piso
  `arc ≥ (1-εb)·arc_base` por banda-z por componente (usar/estender o código
  auditado em S1.1; `--band-floor-slack`, default off). SEM teto novo (teto é
  o stretch normal). Gate: determinismo; no Blacksmith, bandas sob o ápice não
  encurtam >εb (relatório da métrica), sem inflar yarl (gate C não piora).
- **S1.4 Proximidade/self-contact mínimo (opcional, só se S1.2 deixar tiras
  coladas)**: repulsão de pares não-adjacentes < r_min reusando a broad phase
  existente. Flag própria, default off. Gate: determinismo + custo <2× tempo.

### FASE 2 — Matriz de falsificação de H3 (1-1.5 dia) ← COMEÇA O VEREDITO
- **S2.1 Matriz principal** (via pathb_gate.py, SEMPRE nos 3 sentinelas):
  {shell-base} × {+compressão} × {+banda} × {+ambas} × εc∈{0.03,0.05,0.10} ×
  εb∈{0.05,0.10} × gravity∈{0, leve}, morph-steps=6, iterations=10. Rest
  SEMPRE base. Proibido: shadow, plumb uniforme, rest sweeps (§2.3).
- **S2.2 Leitura**: para cada célula, scoreboard + renders left/back dos 3.
  Candidato vencedor = passa gates A+B+B2+B3+C+D com UM conjunto de knobs.
- **S2.3 Se houver vencedor**: rodar nos secundários (vampire, cloak, lemoria
  SkirtA/B) e registrar; 2 runs SHA-idênticos; escrever "marco 7" no log com
  comando reproduzível.
- **REGRA DE PARADA DA FASE 2**: se nenhuma célula passa A+B2+C juntos
  (Blacksmith não piora / falmer cai reto / yarl não abre), H3 está
  falsificada. Escrever o marco negativo no log (números + renders), commitar,
  e PARAR — a decisão do próximo passo volta ao usuário/Opus. NÃO improvisar
  famílias novas fora deste plano.

### FASE 3 — Port C++ isolado (1-2 dias; SÓ com Fase 2 verde)
- **S3.1** Criar `algo/static_shell_solver.h/.cpp` (novo, isolado): dados de
  entrada explícitos (base cloth, corpo base+diff, edges/hinges/seams/mobilidade),
  o loop XPBD com as famílias vencedoras, saída de verts. `conform_hook.cpp`
  ganha SÓ: gate `DRAPE_SHELL_SOLVE` (env; default OFF) + flag live
  `shell_solve_on.flag` + chamada que substitui o resultado do caminho atual
  pelo do solver para presets Fitted/merged-Loose. Zero mudanças com flag OFF.
- **S3.2** Paridade Python↔C++: mesmo garment, mesmos knobs → diferença média
  <0.05u e mesmos gates passando (não exigir bit-igualdade entre linguagens;
  exigir bit-igualdade entre 2 runs C++).
- **S3.3** Gates de release do harness: corpus OFF=verde intocado; com ON,
  medir os 18 shapes e registrar (sem promover golden); determinismo; tempo
  por shape ≤ ~5s (temos 3.9s de precedente aceito no anticlip).
- **S3.4** Deploy A/B: build RelWithDebInfo, verificar deploy automático nas
  duas pastas (SHA idênticos das 3 cópias), flag OFF por default. Commit + push.

### FASE 4 — Gate visual live (usuário) e decisão de promoção
- Usuário liga `shell_solve_on.flag`, reconforma sentinelas + secundários no
  OutfitStudio (REINICIAR o OS antes: a DLL carrega no launch) e julga: queda
  com gravidade, sem bump, sem clump, cofrinho ok, nada regrediu. Só depois
  disso se discute default ON, remoção de passes redundantes (cleft-bridge,
  plumb-guard etc.) — cada remoção com corpus + A/B próprios, NUNCA em bloco.

## 6. Armadilhas conhecidas (lidas antes de cada fase; custaram dias)

1. `check_corpus.py` default = **RelWithDebInfo** e os goldens atuais são
   RelWithDebInfo (rebaked 2026-07-13). Se um dia o drift parecer impossível:
   `git stash` e checar HEAD antes de acreditar/`--update`.
2. A DLL carrega no LAUNCH do OutfitStudio: teste live exige reiniciar o OS;
   flags `.flag` são lidos por conform (não exigem restart).
3. Renders matplotlib low-poly ENGANAM em features sutis (me enganaram 3× no
   bump): usar os perfis numéricos de `pathb_metrics.py` + múltiplos ângulos, e
   tratar o olho do usuário como juiz final.
4. `score_sculpt.py` lê caches velhos de `tmp/smval` — não usar sem re-gerar.
5. Seam twins: qualquer pass posicional deve weldar `exactSeamGroups` por
   iteração (spike já faz; manter no C++). Tears de twins são invisíveis a
   métricas de stretch (detector manual: `tmp/beltlive/twincheck.py`).
6. Cutoff de mobilidade 0.20 duro → stretch 6-8x em arestas na fronteira; usar
   a transição contínua do spike.
7. Yarl Clothes é "No Physics" e preset FITTED nato para o solver — continua
   entrando no shell solve pelo preset merged; é sentinela, não exceção.
8. Vampire tem placas internas autorais: penetração de vértice "profunda" pode
   ser placa escondida (12/13 no caso medido) — julgar por superfície visível.
9. PowerShell 5.1: sem `&&`; heredocs Python via Bash tool com `py -3 -`.

## 7. Housekeeping pendente (peer-review), incluído na Fase 0
- Congelar sculpt do falmer (OSD mudou pós-snapshot) — S0.1.
- Commitar/pushar esta branch com este plano (primeiro commit da branch).
- Registrar a investigação Path A no diário (`SHELL_SPIKE_LOG.md` "marco 6.5:
  verlet falsificado contra sculpts" com a tabela §2.2) — 10 linhas, S0.1.
- `UNIVERSALITY_HANDOFF.md` menciona hash antigo de DLL; NÃO atualizar agora
  (fora do escopo desta branch; anotar apenas).

## 8. Referências rápidas
- Solver PBD atual (loose path): `algo/conform_hook.cpp:9019` região; bending
  por distância: `:348`; merge Loose→Fitted: `:7695`; rest do vanilla: `:5543`.
- Spike: `tools/shell_spike.py` (CLI linhas ~1525-1568); diário:
  `SHELL_SPIKE_LOG.md`; render: `tools/render_shell_diagnostics.py`.
- Harness: `--replay --result-obj --body-obj --refbody-obj`; corpus:
  `py -3 scripts/check_corpus.py`.
- Falsificação Path A + números: memória do projeto
  (`project-verlet-regresses-sculpt`) e §2.2 deste plano.
- Peer-review do Codex (2026-07-13): verlet=PBD com estrutura insuficiente;
  spike já cobre parte do Path B; dihedral sozinho não basta; gate offline
  multi-garment antes de port; estimativas 2-4d/1-2sem/2-4sem.
