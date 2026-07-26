# Relatório para o Codex — estado do Drape após a sessão de 24-26/07

Branch `wip/path-b-shell-solve`, HEAD `f5528e4`, pushado. Corpus 18/18 verde.
Ponto de restauração: tag `known-good-arm-butt-2026-07-22`.

**Estado do live:** motor DESLIGADO (`shell_solve_off.flag`). O usuário testou o
build com o motor universal e o veredito foi *"terrível e extremamente
inconsistente — algumas outfits aceitáveis, outras médias, outras terríveis,
nenhuma boa de verdade"*. Não religar sem os consertos abaixo.

---

## 1. O que motivou a sessão

O usuário descobriu que o gate do motor era:

```cpp
(pathBAllShapes || shapeName == "BlackSmithTorso") && sliderName == "RM_Heavy"
```

**Consequência silenciosa:** todas as outras outfits que ele vinha testando por
semanas rodavam **só o caminho de produção, sem simulação nenhuma**. Os
resultados dele nessas peças nunca disseram nada sobre este solver.

A regra dele (repetida duas vezes, é a alma do projeto): **o Drape tem que ser
universal — qualquer slider, qualquer mesh.** BlackSmithTorso e RM_Heavy são
*apenas casos de teste*. Nada de allowlist por nome.

## 2. Hardcodes removidos (commits `0085e71`, `5150f26`)

Havia **6**. Em cada caso o nome substituía uma propriedade que **já estava
expressa de forma geral ali perto**:

| passe | o nome dizia | critério geral que já existia |
|---|---|---|
| gate do motor | blacksmith + RM_Heavy | preset de material (tecido vs rígido/justo/tubular) |
| log scope-skip | só RM_Heavy | nada — só escondia informação |
| layer-order | `vampire_robes_alt` | "é tecido **e** tem colisores internos" (linha seguinte) |
| posterior-chord | `BlackSmithTorso` | opt-in + tecido justo + protege rígidos/pinados por medição |
| anti-clip guard | `vampire_robes_alt` + RM_Heavy | assinatura composta de falha (normais invertidas **e** área≈0) |
| scaffolding harness | `vampire_robes_alt` | o lookup do mapa já desiste sem relação |

Sobrou **zero** verificação por identidade. Os literais restantes no arquivo são
apenas comentários documentando o que foi removido.

**Exceção honesta — precisa do seu olhar:** o piso legado (~linha 20301) tinha
razão **medida**: no Thalmor o piso topology-safe deixava penetrações mais fundas
que o loop push/cap antigo. Um nome é a forma errada de expressar isso. A forma
correta está documentada no código: **medir a penetração residual e cair pro
outro método quando estiver funda**. Não implementei. Enquanto não existe, o
opt-in aplica onde for ligado (default off).

## 3. Diagnóstico novo: `DRAPE-REPORT` (commit `e6af3b9`)

Uma linha por shape, sempre ligada, sem mudar comportamento. Ela separa os dois
modos de falha que eu antes só adivinhava:

```
DRAPE-REPORT shape='X' slider='Y' preset=N path=SHELL|legacy(...)
  mask(fixed=.. trans=.. free=.. rigid=.. of ..)
  comZ(entry=.. shell=+.. final=+..)  tail(rms=.. max=..)
  final(flips=.. pen=.. worstPen=..)
```

Primeira rodada no corpus (motor ligado):

| descoberta | número |
|---|---|
| peças que **nem entram** no solver | **11 de 17** (`legacy(out-of-scope)`, queda do shell +0.00) |
| Cloak: rabo apaga | **91%** (shell +1.81 → final +0.17) |
| vampire robes | **100%** (+0.26 → −0.03) |
| BlackSmithTorso | 55% (+1.29 → +0.58) |
| deslocamento do rabo | rms **1.5–3.5u** |

## 4. A medição mais importante da sessão

Cloak, mudança do centro de massa (entrada → shell):

```
COM gravidade:   +1.81   ← o pano SOBE
SEM gravidade:   +1.83
```

**A contribuição total da gravidade é 0.02u.** O solve **levanta** o tecido
~1.8u — o contato empurra radialmente pra fora e num corpo convexo "pra fora"
tem componente pra cima (a mesma subida medida no bumbum do blacksmith) — e a
gravidade é **~90× fraca demais** pra responder.

**Este solver é movido por CONTATO, não por gravidade.** É a causa-raiz do "não
parece cloth sim".

Dois corolários que reordenam o trabalho:

1. Subir a gravidade até o pano realmente cair (carga 9 ≈ 100×) move o resultado
   **para longe** do hand-sculpt do usuário (2.94 → 3.19). O sculpt dele **não é
   um caimento puro de gravidade** — é a forma artística dele. "Mais gravidade"
   não é o alvo.
2. Mesmo com gravidade 100×, o rabo ainda apaga **78%** (shell −1.24 → final
   −0.27).

→ **Ajustar a dose de gravidade é inútil antes de o shell ser autoritativo.**

## 5. Gravidade como carga (commit `1c6cc96`, default off)

`DRAPE_SHELL_GRAVITY_MODE=1`: aplica a carga **uma vez por load step**, converge
as constraints sem carga, admite o próximo load step. Substitui o `x[i].z -=
gravity` por iteração.

Dois bugs achados ao validar, vale registrar:

- Derivar o espaçamento dos loads de `kMaxIters` está errado, porque o loop
  **quebra cedo** (todos os 6 morph steps estolam perto da iteração 50). Um teto
  de 600 aplicava **um** load e um teto de 60 aplicava quatro — o oposto de
  independência de iterações. Agora o espaçamento é fixo
  (`DRAPE_SHELL_ITERS_PER_LOAD`).
- O break de convergência/estol precisa ser **retido** enquanto há loads
  pendentes: resíduo assentado significa "esta carga foi absorvida", não "o solve
  acabou".

## 6. Máscara única de classificação (commit `f5528e4`, ligada)

Era **o** bug estrutural. O solver classificava "vestido vs livre" pelo gap
autoral; o pipeline classificava a mesma coisa por **ossos**:

```cpp
drape = (pelve + pernas) * (1 − (braço + mão))
```

Isso significa *"tecido solto é pesado na pelve e nas pernas"*, e o termo do
braço **suprime** o caimento. Verdade pra saia, **falso pra capa** (pendura do
ombro, é pesada no torso/braço). Resultado medido no Cloak: **505 de 553 verts
classificados como ancorados** — uma capa tratada como tecido justo, o que torna
inerte todo hand-off de "o motor manda no tecido livre".

Agora existe **uma** máscara (`DrapeClass FIXED/TRANSITION/FREE/RIGID`), montada
uma vez com regras geométricas ou de material, entregue ao solver
(`DRAPE_SHELL_SHARED_MASK`, ligada) e reportada por shape.

Consistência (% livre+transição nas peças de tecido):

```
antes:  Cloak 9 · Cuirass_Torso 10 · BlackSmithTorso 31 · Clothes 33 · vampire 37 · Cloth 100
depois: Cloak 47 · Cuirass_Torso 36 · BlackSmithTorso 34 · Clothes 34 · vampire 49 · Cloth 100
```

Uma capa finalmente é reconhecida como ~metade tecido livre. **Qualidade é neutra
por si só** (Cuirass_Torso 0.44→0.39 melhor, Cloth 1.50→1.70 pior, resto igual) —
como previsto, porque nada a jusante do shell importa enquanto o rabo apaga.

## 7. Negativo que define como consertar o item que falta

O free-cloth restore na fronteira de publicação estava limitado a 57 de 553
verts **pela máscara quebrada**. Com a máscara única ele alcança **260**. E
**piora**: sculpt 2.93 → 3.56, **flips 8 → 19**.

**Motivo:** restaurar metade da malha do shell enquanto os vizinhos mantêm a pose
do rabo **rasga a fronteira entre as duas**. Não se costura meia malha do solver
A com meia do solver B.

→ **"Tornar o shell autoritativo" não pode ser feito por restore tardio.** Os
passes de segurança têm que virar **constraints dentro do solve**.

## 8. O que falta — e é onde eu passo pra você

**Item 4: shell autoritativo.** É um refactor de várias sessões e eu
deliberadamente **não** o entreguei pela metade. O que a medição já estabeleceu
sobre ele:

- O rabo move o pano **1.5–3.5u rms** depois do solve.
- **Impedir passes individuais não funciona**: o `panel-flatten` é onde o número
  salta (+1.00 no prumo do Cloak, todo o resto ≤0.19), mas desligá-lo **piora**
  (prumo 7.88, flips 9→18) — o rabo **re-converge** pro mesmo atrator faça o que
  fizer.
- Restaurar no fim rasga a fronteira (item 7).

Logo o caminho é mover as invariantes de segurança para dentro do loop de
constraints: piso de corpo (já está lá), **contato entre camadas/colisores**
(hoje é o layer-tuck, fora), **self-collision** (não existe — o
`final-selfcollision-tuck` é geométrico e posterior), e **fechamento de
costura**. Prioridade por dano medido: layer/inter-garment contact primeiro.

**Outros abertos, com evidência:**

- **11 de 17 peças fora de escopo.** O gate é material
  (`FITTED_CLOTH || LOOSE_DRAPED`). Peças com preset belt/rigid/tubular/auto
  nunca recebem física. Se isso é intencional, ok; se não, o gate precisa
  revisão — mas note que ligar o motor pra tudo **piorou 5 peças** contra os
  sculpts.
- **Loose→Fitted merge**: o Cloak é `Loose Draped` na origem e roda como
  `Fitted`, o que o joga no caminho de ossos. O merge foi uma decisão medida
  (commit `b7c18dc`), mas é o que faz uma capa não ser tratada como capa.
- **Métrica de repouso parte do corpo slim** (`gradedRest` off). Em RM_Heavy o
  hand-sculpt mostra p50 ≈ 1.10× e p95 ≈ 2.06× o comprimento base, então o solver
  tenta **contrair** a roupa Heavy de volta à métrica slim.
- **Bending default é Jacobi**, descrito no próprio código como quase inerte
  (~0.45/valência por correção). O GS colorido existe mas piorou nos testes
  isolados — precisa entrar junto com métrica de repouso e contato corrigidos.

## 9. Ferramentas de validação (use-as, elas pegam o que métrica escalar não pega)

- `tools/pathb_render_sheet.py` — **a autoridade para forma/vincos**. Render
  suave dos ângulos do usuário, resultado / produção / sculpt lado a lado.
  Nenhuma métrica escalar bate com o olho dele: distância de superfície perde
  vincos, por-vértice pune slide invisível, dihedral dilui dobra longa e rasa.
- `tools/pathb_sculpt_sweep.py` — distância de **superfície** por região vs
  sculpt (não use por-vértice: ela pune deslize tangencial invisível e me fez
  perseguir um defeito que não existia).
- `tools/pathb_defect_gate.py` — bloqueia deploy; mede a classe **visível**
  (tecido autoral-fora enterrado >0.3u), não a contagem crua.
- `DRAPE-REPORT` no log — sempre ligada, uma linha por shape.

## 10. Erros meus nesta sessão, pra você não repetir

1. **Deixei o allowlist por nome vivo** por muito tempo, e ele silenciosamente
   invalidou semanas de teste do usuário em outras outfits.
2. **Confiei em métrica agregada** e disse "está bom" várias vezes enquanto o
   olho dele via defeito. A folha de render existe por isso.
3. **Chamei um defeito real de artefato de renderização** (o vinco do bumbum).
   Era real. Render com normais suaves antes de crer — flat-shading low-poly
   inventa vincos, e média de região esconde pinçamento local.
4. **Rodei testes com binário velho** duas vezes (build falhou por diretório
   errado e não percebi). Números bit-idênticos entre configs diferentes são o
   sintoma — desconfie deles.
5. **Um probe meu não imprimia o nome da peça**, e quase culpei o passe errado
   com contagens que vinham de outra shape. Sempre imprima `shapeName`.
