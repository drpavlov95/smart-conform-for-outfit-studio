# PATH B — REAL CLOTH SIM PLAN (pós-crítica 2026-07-17)

Crítica do usuário, aceita integralmente: "eu pedi um cloth simulation e você
foi muito mais pro smart conform do que pra um cloth sim de verdade."
Contaminações mapeadas: (1) boundary conditions da pose vanilla-conform,
(2) floors radiais de estilo, (3) próteses geométricas (shadow, hull) toda vez
que a física decepcionou, (4) plano de fitar a transformada do sculpt offline
(cancelado — era o ápice do smart conform).

Evidência física: o sculpt tem assinatura de CASCA RÍGIDA (alavanca 1.9× no
bumbum, tenda pivotando no cinto, A-line 3.5-4u fora do corpo). O bending do
sim está mole (2e-2, amolecido para fugir de buckling) — tecido mole abraça o
corpo; couro não.

## Fase 0 — Evidência (REVISADA pós-review 2026-07-17)
PRIMEIRO os dois CONTROLES (mais baratos que uma célula de sweep, decidem
independente da qualidade do solver):
- C1 LIMITE RÍGIDO: rotação rígida best-fit do painel traseiro livre em torno
  da linha do cinto, empurrada pelo contato do bumbum — geometria pura. Se nem
  o limite rígido alcança 17.9-20.0, NENHUMA rigidez alcança e o A-line vem de
  outra coisa (flare da forma de descanso).
- C2 FINGERPRINT DIEDRAL: diedros do sculpt vs autorais no painel traseiro.
  Casca rígida ⇒ sculpt ≈ ângulos autorais; drape/prumo ⇒ ângulos esticados
  abaixo do ápice. (O 1.9×/0.5× é consistente com AMBAS as hipóteses; isto
  discrimina.)
SÓ ENTÃO o sweep, e apenas na configuração VÁLIDA (um NÃO fora dela não prova
nada — três mecanismos de falso-negativo):
- pins da Fase 1 (lei de contato ≤0.3 + skin-follow por binding AUTORAL),
  nunca o classificador amplo/alvos vanilla;
- α̃ = α/h² (contrato PERMANENTE de unidades — reestruturar o solver não pode
  mudar o material silenciosamente);
- colored-GS (marco 10) em vez de Jacobi (Jacobi satura rigidez: propaga ~1
  anel/iter; alavanca precisa de ~30 anéis); substeps pequenos × poucas iters
  (Small Steps) em vez de doses grandes × muitas iters;
- 1 célula BILATERAL in-plane (compressão = tensão): casca só alavanca se o
  lado côncavo resiste à compressão — compressão 10× mole absorve o momento
  em qualquer bend-compliance;
- telemetria de RESIDUAL na saída (max |diedro−rest|, max stretch): só
  interpretar runs onde o residual platôa (<~5°) ou dobrar iterações move a
  extensão <0.1u. Um candidato com residual grande "não é o material
  especificado" e não pode ser julgado visualmente como evidência.
Interpretação assimétrica: SIM confiável; NÃO exige certificação de
convergência antes de valer.

## Fase 1 — Lei de suporte FÍSICA (remove contaminação #1)
- Banda justa (authoredGap ≤ 0.3) = "segue a pele". JUSTIFICATIVA FÍSICA:
  força normal + fricção prendem cinematicamente tecido justo (attachment
  skinned, padrão em cloth sim de produção). IMPLEMENTAÇÃO CERTA (armadilha
  apontada no review): binding UMA VEZ na pose AUTORAL (triângulo do corpo +
  baricêntricas + offset normal autoral) e TRANSPORTE pelo frame do triângulo
  morfado (maquinaria surface-corot do S0.3e — falhou como lei do tecido
  LIVRE, é exatamente correta para a banda ≤0.3). Closest-point POR FRAME no
  corpo morfado seria re-projeção (deriva tangencial, flip no vinco) = smart
  conform voltando. O que sai: posições do conform VANILLA como alvo.
- Banda de transição 0.3-1.2: tecido LIVRE puro (só clamp de massa; sem soft
  anchor, sem rampa — rampas são onde o cola-no-corpo reentra).
- Tecido livre: NENHUMA âncora, NENHUM rest de fronteira vindo de pose
  vanilla. Arestas de fronteira: rest = comprimento AUTORAL (a tensão residual
  sobre o corpo crescido é física real, não artefato).
- Cinto (outer clamp): manter como suporte se a geometria do clamp existir
  (produção já computa belt support por geometria).

## Fase 2 — Material real (o núcleo; gated pela Fase 0)
- Bending rígido estável: XPBD com compliance grau-couro; rigidez via
  SUBSTEPS da continuação (morph steps 6→N com doses menores), não via
  iteração bruta; portar colored Gauss-Seidel do spike (marco 10) se Jacobi
  não converge rígido.
- Strain limit de tensão 1.10 (mantém — física de inextensibilidade).
- Fricção tangencial no contato (mata deslize/distorção de textura; física).
- Self-collision do hem (grade uniforme, repulsão vértice-vértice entre
  não-vizinhos): necessária para buckling rígido não interpenetrar.
- FALLBACK se rigidez walls na malha (review Q3: a malha NÃO é a parede — o
  solver e o buckling são): REGION SHAPE-MATCHING (Müller) — clusters
  sobrepostos (vizinhança 2-3 anéis) projetados ao rígido best-fit por
  iteração, blend — é literalmente a lei de casca rígida, incondicionalmente
  estável, determinística, e É simulação. Subdivisão NÃO é fallback de rigidez
  (dobra o diâmetro do grafo, 4× o custo de convergir) — fica no bolso pra
  qualidade de rugas do hem.
- DELETAR do solve: floors radiais, soft anchors. Gravity-shadow e drape hull
  só DEPOIS da Fase 0 resolver (se der NÃO certificado, a sombra volta como
  prótese DECLARADA, pela regra do próprio plano). Cleanup de faces da banda
  fixa (marco 12) → verify-only (com skin-follow autoral os leaks da banda
  somem por construção); deletar quando ficar em zero.
- Marco 12 anti-overfit: hem tape declarado MATERIAL (hem é tecido mais rígido
  de fato), autoral-dentro isento (design), reconciliação final (gate de
  segurança) — mantidos, declarados.

## Fase 3 — Contato (mantém, é física)
Vertex+face hard tier (espessura 0.15), clamp de corredor, sticky sides,
reconciliação final. Autoral-dentro isento (paridade de design).

## Fase 4 — Gates e escopo
1. PRIMÁRIO: perfil do sculpt (blacksmith z38-78; alvo 17-20 na traseira).
2. surf fwd/rev vs sculpt ≤ produção (0.76/0.79) desejável; sem colapso (rev).
3. leakmap classes ≤ produção; corpus OFF 18/18; determinismo byte-idêntico.
4. Renders corpo+roupa nos ângulos do usuário.
5. Cross-garment nos outros sculpts (vampire/yarl; falmer benched) ANTES de
   generalizar — a física certa não pode ser blacksmith-only.
6. Só então deploy + olho do usuário (que é o gate final).

## Fase 5 — Performance
Budget inicial ≤ 30s/conform (substeps custam); depois culling de queries +
colored-GS + paralelismo determinístico até ≤ 15s.

## Regras de processo (permanentes)
- Sculpt gate antes de QUALQUER deploy — COM guarda anti-overfit: UM conjunto
  global de parâmetros de material; gate cross-garment antes de deploy; o
  próprio gate primário jamais dirige knob por-garment (senão vira smart
  conform com passos extras).
- Ler o log live antes de teorizar sobre teste do usuário.
- Nenhuma prótese geométrica nova sem declarar que é prótese e por quê.
