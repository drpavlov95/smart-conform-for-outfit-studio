# Static Shell Spike — Diário técnico

## Current live correction (2026-07-12)

Critical correction: the earlier live-OBJ analysis reversed the Y axis. `+Y` is
front and `-Y` is rear, so its tangential fan was unrelated to the glute cleft.
The actual defect remains the original low-frequency posterior surface tuck.
The corrected diagnosis, paired target, unilateral chord implementation, tests,
and live flags are documented in `TRUE_REAR_BLACKSMITH_HANDOFF.md`.
`DRAPE_TENSION_PLUMB` and `DRAPE_TANGENTIAL_REPAIR` remain default OFF.

## Objetivo

Testar, fora do caminho de produção, a hipótese central para `BlackSmithTorso`
em `RM_Heavy`:

> contato unilateral + continuação do morph + métrica/dobra autorais consegue
> produzir a ponte sobre a fenda glútea e o under-butt sem uma ROI anatômica?

O spike será Python/harness-only. Nenhuma etapa deve alterar o comportamento de
`conform_hook.cpp` ou desligar os passes atuais de produção.

## Regras do experimento

- Fonte autoral: roupa base e seus ângulos diedrais.
- O corpo é obstáculo unilateral: pode empurrar, nunca atrair.
- Corpo intermediário: `B(t) = B0 + t * (B1 - B0)`, com passos fixos.
- Sem detector de nádega/cofrinho.
- Iterações e ordem de constraints fixas.
- Todo candidato será executado duas vezes para testar determinismo.
- Comparação principal: renders lateral/grazing e distância superfície-a-superfície
  contra o hand-sculpt.

## Dataset congelado

Status: validado.

Capture candidato:

`corpus/fur_blacksmith_rm_heavy/drape_capture__Rley_3BA_Vanilla_-_Body_-_Blacksmith__Physics___BlackSmithTorso__RM_Heavy.drcap`

Referência de sculpt candidata:

`tmp/beltlive/today_sculpt.obj`

Exports congelados (gerados pelo harness, diretório ignorado pelo Git):

`tmp/shell_spike_blacksmith/`

Contrato confirmado:

- roupa: 2.316 vértices, 3.979 triângulos;
- corpo base e RM_Heavy: 13.651 vértices, 26.412 triângulos;
- base, vanilla, resultado atual, expected capturado e sculpt têm faces
  byte-identicamente ordenadas;
- corpo base e morphed têm topologia idêntica;
- duas execuções da baseline produziram o mesmo SHA-256;
- diferenças máximas OBJ vs capture (`~0.0005u`) são apenas arredondamento do
  export textual; o spike usará os floats binários do `.drcap`.

Topologia da roupa:

- 6.284 arestas únicas;
- 5.653 hinges de dois triângulos;
- 631 boundary edges;
- zero arestas non-manifold neste garment;
- 15 componentes conectados (maior: 1.534 vértices);
- 242 grupos de vértices exatamente coincidentes / 505 seam verts;
- classificador Fitted atual: 1.589 ancorados, 137 transição, 590 livres.

Antes de aceitar o dataset, confirmar:

- mesma contagem/ordem de vértices entre roupa base, vanilla e sculpt;
- mesma topologia onde aplicável;
- corpo base e morphed com mesma topologia;
- todos os arquivos no mesmo sistema de coordenadas;
- baseline do harness repetível.

## Plano do spike

1. Exportar do `.drcap`: base da roupa, vanilla, resultado atual, corpo base e
   corpo morphed.
2. Validar o contrato de dados e registrar hashes/contagens.
3. Implementar loader OBJ e estruturas de adjacency/hinges/seams.
4. Implementar continuação do corpo e contato unilateral.
5. Adicionar strain e bending diedral autoral.
6. Comparar três métricas de repouso:
   - base pura;
   - crescimento regional simples;
   - strain vanilla com low-pass.
7. Medir, renderizar, repetir e decidir se a hipótese merece port para C++.

## Registro cronológico

### 2026-07-11 — início

- Revisão do solver atual concluiu que o caminho efetivo `Fitted Cloth` usa a
  roupa vanilla como posição e métrica de repouso e não ativa o solver
  Verlet/bending.
- Confirmado anteriormente que o formato de capture v3 armazena `refBodyBVH` e
  que o harness expõe `--refbody-obj`.
- Próxima ação: exportar e validar o dataset congelado.

### 2026-07-11 — contrato de dados validado

- Criado `tools/shell_spike.py` com parser parcial do capture v3 e modo
  `--inspect`.
- O primeiro run detectou desalinhamento no parser experimental. Causa:
  `r.off += r.count() * size` em Python preservava o valor antigo do lado
  esquerdo e descartava os quatro bytes consumidos por `count()`. Corrigido
  separando leitura de count e avanço do offset.
- `--inspect` agora valida capture, exports, topologia, bboxes, componentes,
  hinges, seams e classificação livre/ancorada.
- Confirmado que o Blacksmith não é topologicamente trivial: embora não tenha
  non-manifold edges, possui 15 ilhas e 242 grupos de seam twins/coincidentes.
- Próxima ação: implementar as estruturas geométricas do solver e validar
  contato unilateral primeiro em `t=0`, onde roupa base e corpo base já formam
  um estado conhecido.

### 2026-07-11 — primeiro solver quase-estático

Implementado em `tools/shell_spike.py`:

- broad phase uniforme determinística + closest-point exato em triângulo;
- validação do broad phase contra brute force (32 amostras, erro zero);
- continuação `body0 -> body1` em passos fixos;
- contato unilateral com floor relativo ao estado autoral;
- stretch XPBD/Jacobi;
- bending por diedro assinado autoral (gradiente numérico vetorizado no spike);
- weld dos 242 grupos de seam twins;
- mobilidade contínua a partir dos bone profiles;
- anchors superiores seguindo o vanilla somente fora do painel livre;
- export OBJ e distância superfície-a-superfície contra o sculpt.

Descoberta de robustez: copiar o cutoff de mobilidade `0.20` da produção abriu
uma descontinuidade fixed/free em duas arestas de apenas `~0.2u`, levando-as a
`6-8x` stretch. O spike agora usa uma transição contínua; o pior stretch volta
ao defeito já existente nos anchors vanilla (`~4.065x`).

### 2026-07-11 — primeira matriz de hipóteses

Foram testados, sempre sem ROI anatômica:

1. `base`: comprimentos e diedros da roupa base;
2. `uniform`: crescimento isotrópico pela mediana do vanilla (`1.2186x`);
3. `base + gravity`;
4. `base + pre-tension` isotrópica (~2.2% e ~10%);
5. `flat-panel`: diedro de repouso zero no painel móvel;
6. `lowpass`: log-strain circunferencial do vanilla -> campo por vértice ->
   20 rounds de difusão no grafo -> rest lengths, sem alvo posicional.

Resultados provisórios:

- A continuação + contato apaga a fenda horizontal sem detector de nádega. A
  hipótese física é parcialmente confirmada.
- Nenhuma variante física reproduziu o under-butt do sculpt. Nas bandas
  `z~50-60`, o sculpt sustenta `y~10-10.5`; os shells ficam em `y~7-8`.
- Crescimento uniforme introduz folga/rugas e piora o erro.
- Gravidade baixa a malha mas não cria a tração/plumb desejada.
- Pre-tension isotrópica aumenta strain e quase não muda o perfil.
- `flat-panel` mostra que diedros locais não controlam sozinhos a concavidade de
  baixa frequência.
- `lowpass` é o melhor shell experimental até agora (surface mean
  result->sculpt `0.9548`, reverse `0.9930`), mas continua pior que o caminho
  atual no alvo lateral e não resolve o under-butt.

Interpretação atual:

- contato unilateral determina onde o corpo empurra e é suficiente para não
  copiar a fenda;
- ele não determina o volume/raio que tecido sem suporte deve manter;
- a roupa base 3D já contém a curva-S do corpo slim e não equivale ao padrão
  material plano;
- a regra do sculpt parece incluir uma propriedade global de pattern/silhueta
  (por exemplo, circunferência/raio que não recua abaixo do ápice), que não é
  identificável apenas por diedros locais;
- o próximo desenho deve tornar essa propriedade material explícita, em vez de
  procurar mais stiffness/gravity.

Artefatos principais (diretório ignorado pelo Git):

- `tmp/shell_spike_blacksmith/profiles_lowpass.png`;
- `tmp/shell_spike_blacksmith/shell_lowpass_butt.png`;
- `tmp/shell_spike_blacksmith/shell_lowpass.obj`;
- `tmp/shell_spike_blacksmith/shell_base_stiff.obj`.

Próximo gate: repetir `lowpass` com os mesmos argumentos e exigir SHA-256
idêntico antes de explorar uma métrica global de pattern/silhueta.

### 2026-07-11 — determinismo e energia de meridiano

- `lowpass` foi repetido com argumentos idênticos.
- Ambos os OBJs têm SHA-256
  `5CD6A7E24DB9B20F9AFE347858FE840145EA0B2972205C9CE045E0F30B59B218`.
- Gate de determinismo do spike: **PASS**.

Também foi testada uma propriedade material explícita de baixa frequência:

- 598 triplets de meridiano foram inferidos somente por topologia + eixo da
  gravidade;
- cada triplet penaliza curvatura horizontal entre vizinhos abaixo/centro/acima;
- nenhum dado do corpo ou ROI anatômica entra na seleção;
- compliance `0.01` e `0` foram testadas.

Resultado: mudanças pequenas, sem recuperar o raio do under-butt. A formulação
local de meridiano compete com stretch/contato, mas não cria a condição de
suporte longa necessária para manter o fall desde o ápice.

### Conclusão do marco 1

O spike falsificou a versão forte da hipótese original:

> `body obstacle + local membrane/bending + morph continuation` não contém
> informação suficiente para reproduzir sozinho o sculpt no espaço sem contato.

Confirmou uma versão mais limitada:

> contato unilateral + continuação evita copiar/entrar na fenda horizontal.

O próximo candidato não deve ser mais um rest-length sweep. Deve adicionar uma
**geometria unilateral de suporte gerada por contato**:

1. durante a continuação, marcar contatos reais onde o corpo empurrou o tecido;
2. varrer esses suportes na direção da gravidade (contact/gravity shadow);
3. usar o shadow como desigualdade unilateral dentro do mesmo loop XPBD;
4. deixar membrana/dobra resolver a transição entre colunas e contatos;
5. nunca puxar tecido para `closestPoint + clearance`;
6. sem ROI de nádega; a origem do suporte é o active set de contato.

Esse desenho reintroduz uma noção de envelope, mas derivada da física/histórico
de contato e aplicada como obstáculo, não como alvo posicional. O experimento
mostrou por que um body-SDF puro não basta no under-butt.

### Comandos reproduzíveis

Inspeção e contato:

```powershell
py -3 tools\shell_spike.py --inspect
py -3 tools\shell_spike.py --contact-check
```

Melhor candidato local atual:

```powershell
py -3 tools\shell_spike.py --solve `
  --rest lowpass --growth-blend 1.0 --bend-rest base `
  --morph-steps 6 --iterations 20 `
  --stretch-compliance 0 --bend-compliance 0.02 `
  --output tmp\shell_spike_blacksmith\shell_lowpass.obj
```

Diagnóstico visual:

```powershell
py -3 tools\render_shell_diagnostics.py `
  tmp\shell_spike_blacksmith\profiles_lowpass.png `
  --body tmp\shell_spike_blacksmith\body_morphed.obj `
  "SCULPT=tmp\shell_spike_blacksmith\cloth_sculpt.obj" `
  "CURRENT=tmp\shell_spike_blacksmith\cloth_current.obj" `
  "LOWPASS=tmp\shell_spike_blacksmith\shell_lowpass.obj"
```

## Estado para handoff

Marco 1 concluído. Dataset, parser, contato, solver local, matriz de rest metrics,
renders e determinismo estão prontos. A hipótese foi confirmada para a fenda
horizontal, mas falsificada para o under-butt sem suporte adicional. Próxima
implementação: contact/gravity shadow como obstáculo unilateral dentro do solve.

## 2026-07-12 — marco 2: suporte unilateral e plumb topológico

### Gravity shadow portada e testada

`GravityShadow` foi portada do protótipo C++ para o spike, com:

- raster determinístico `topZ(x,y)` do corpo;
- fonte `body` ou somente triângulos realmente tocados pelo active set;
- expansão geodésica configurável dos triângulos de contato;
- falloff binário ou linear entre anéis;
- busca da saída livre `nearest` ou radial;
- aplicação somente outward (nunca magnetiza/puxa tecido livre para o corpo).

Matriz de falsificação, sempre com shell `rest=base`, 6 morph steps, 10 iterações:

| variante | result→sculpt mean | sculpt→result mean | observação |
|---|---:|---:|---|
| shell sem shadow | 0.9951 | 1.0465 | não mantém o under-butt |
| shadow do corpo inteiro, nearest | 1.0571 | 1.1525 | superinfla o painel para `y~15` |
| contato exato, 0 anéis | 0.9812 | 1.0323 | suporte estreito demais |
| contato + 1 anel, binário/nearest | 0.9540 | 1.0052 | pequena melhora localizada |
| contato + 2 anéis, binário/nearest | 0.9776 | 0.9871 | aparecem picos laterais |
| contato + 3 anéis, binário/nearest | 0.9926 | 1.0243 | expansão descontínua e excessiva |

O falloff linear remove a descontinuidade de peso na borda do patch, mas não o
problema principal. A busca `nearest` deixa vértices centrais escaparem para os
lados; a busca `radial` elimina esse escape, porém torso/braços abaixo do teto do
garment passam a formar um guarda-chuva largo. O body shadow radial chegou a
`1.1388/1.1716` e foi descartado.

Conclusão: uma projeção de silhueta do corpo ainda mistura suportes que não são
conectados pelo tecido. Ajustar anéis/força não corrige estruturalmente isso.

### Desigualdade de plumb transmitida pelo grafo da roupa

Foi então implementado `apply_graph_plumb_constraint`, sem ROI anatômica e sem
consultar o sculpt:

1. ordena os vértices móveis de cima para baixo;
2. lê somente vizinhos de aresta que estão acima;
3. propaga o maior raio suportado, com `returnRate` opcional;
4. empurra somente para fora; nunca puxa para dentro;
5. stretch, diedro, seams e contato continuam resolvendo a transição.

Essa é a informação global que faltava ao shell local: o suporte viaja pelo
próprio tecido, em vez de ser inferido de qualquer corpo situado acima na mesma
coluna.

O parâmetro foi normalizado para representar correção total por morph step:

```text
iterationStrength = 1 - (1 - stepStrength)^(1 / iterations)
```

Com `stepStrength=0.4614`, `returnRate=0`, os cortes `z=44.50` e `z=52.58`
recuperam o volume do sculpt sem criar a fenda central. Sensibilidade ao número
de iterações:

| iterações | result→sculpt mean/p95 | sculpt→result mean/p95 |
|---:|---:|---:|
| 5 | 0.8793 / 2.8315 | 0.9024 / 2.3849 |
| 10 | 0.8779 / 2.8372 | 0.9032 / 2.4366 |
| 20 | 0.8839 / 2.8539 | 0.9135 / 2.5145 |

Contra o shell local sem plumb, o candidato de 10 iterações reduz a distância
média result→sculpt em ~11.8% e a reversa em ~13.7%. Ele ainda **não supera o
caminho de produção atual** (`0.7538/0.7868`), que contém a pilha heurística já
afinada; portanto ainda não há justificativa para editar `conform_hook.cpp`.

Repetibilidade: duas execuções do candidato de 10 iterações produziram o mesmo
SHA-256:

```text
7069852DE9DA09EC479F801DC946776950505BF162B44D23A54B8AE35EFA0322
```

### Comando do candidato do marco 2

```powershell
py -3 -B tools\shell_spike.py --solve `
  --rest base --bend-rest base `
  --morph-steps 6 --iterations 10 `
  --stretch-compliance 0 --bend-compliance 0.02 `
  --shadow-source none `
  --graph-plumb-strength 0.4614 --graph-plumb-return 0 `
  --output tmp\shell_spike_blacksmith\shell_graph_plumb_step46_i10.obj
```

Diagnóstico visual principal:

```text
tmp/shell_spike_blacksmith/profiles_graph_plumb_weak.png
```

### Estado atualizado para handoff

- `algo/conform_hook.cpp` continua intocado.
- O spike agora contém tanto as variantes de gravity shadow falsificadas quanto
  o candidato de plumb topológico.
- Candidato atual: shell base + graph plumb normalizado `0.4614`, sem shadow.
- Próximo gate antes de produção: testar o mesmo parâmetro em pelo menos mais um
  capture/roupa e medir regressões em frente/laterais, não apenas neste sculpt.
- Se generalizar, portar a desigualdade para o loop quasi-static/XPBD do C++ como
  constraint própria. Não reativar diretamente o `ApplyHangingChainPass` antigo:
  ele é uma projeção final rígida e não tem normalização por iteração.

## 2026-07-12 — marco 3: gate de generalização no Yarl Clothes

O spike passou a aceitar qualquer `.drcap` mesmo sem dataset OBJ externo. Nesse
caso ele exporta `cloth_base`, `cloth_vanilla`, `body_base` e `body_morphed`
diretamente da captura e usa o vanilla somente como referência diagnóstica. Isso
não substitui um sculpt humano, mas permite um teste conservador de regressão.

Segunda captura escolhida:

```text
Yarl Clothes 2 / Clothes / RM_Heavy
1777 verts, 4393 hinges, preset capturado Loose Draped
```

Resultados contra o vanilla da própria captura:

| variante | result→vanilla mean/p95 | vanilla→result mean/p95 |
|---|---:|---:|
| shell base | 0.3764 / 2.1686 | 0.5746 / 2.9175 |
| graph plumb 0.4614 | 0.3418 / 1.9997 | 0.5795 / 3.0071 |

A média unilateral melhora levemente, mas o render mostra uma regressão clara:
nas bandas `z=44.5–60.7`, o plumb abre o painel para `y~11.8–12.0`, enquanto o
vanilla fica em `y~10–11`. Também pioram:

- mean bend: `7.922° → 9.692°`;
- mean strain: `0.3401 → 0.3506`;
- worst stretch: `10.822 → 11.169`;
- máximo vanilla→result: `6.4100 → 8.7538`.

O mesmo parâmetro que corrige o Blacksmith, portanto, **não é neutro no segundo
garment**. Usar o raio vanilla como teto também não resolve de modo principled:
ele protegeria o Yarl, mas no Blacksmith o vanilla está cerca de `0.8–1.0u`
abaixo do sculpt exatamente nas bandas do under-butt. Um teto `vanilla + margem`
seria apenas outro threshold afinado por roupa.

Diagnóstico visual:

```text
tmp/shell_spike_yarl_clothes/profiles_graph_plumb.png
```

### Decisão após o gate

- **Não portar** gravity shadow nem graph plumb para `conform_hook.cpp` agora.
- A continuação + shell local continua validada para apagar a fenda horizontal.
- Under-butt exige informação material global que não está identificada apenas
  por corpo, contato, diedro, rest lengths locais ou propagação topológica.
- Próxima pesquisa, se retomada: formular explicitamente conservação de
  circunferência/folga do pattern por componente ou faixa, e testá-la ao mesmo
  tempo no Blacksmith e Yarl. Não fazer novo sweep de força/margem por garment.
- O código offline e todos os resultados negativos ficam preservados para que o
  próximo agente não repita os mesmos experimentos.

## 2026-07-12 — marco 4: contact-tension plumb portado para C++

O gate anterior foi reaberto após substituir a propagação uniforme por suporte
iniciado em contato e transmitido somente por arestas em tração. A intensidade
do caminho é limitada pela menor extensão fracionária encontrada. Isso resolveu
o problema estrutural observado no Yarl: uma aresta folgada não transmite uma
silhueta rígida para o restante do componente.

A formulação foi portada para `algo/conform_hook.cpp` como
`ApplyTensionSeededGraphPlumb`, atrás de `DRAPE_TENSION_PLUMB` e desligada por
padrão. Integração final inclui:

- escopo somente para preset originalmente `Fitted Cloth`;
- outer-layer protection de 10u, expandida por exact seam groups;
- trust region de 1.25u por vértice;
- weld de seams após a projeção;
- true-final body floor preservado como último passe;
- enable live por `tension_plumb_on.flag`;
- rollback removendo flag/env, sem recompilar.

No replay C++ do Blacksmith, o candidato limitado reduz surface mean
result→sculpt `0.753753 → 0.726675` e reverse `0.786777 → 0.754031`, sem mudar
worst edge stretch (`4.0651`) nem counts de layer residual (`2/10`). SHA-256 de
duas execuções: `C2E205552C139479A7D4F4A201BBD30D41AC6C00D3247D0295E581B391151F15`.

Com a feature OFF, os 18 shapes do corpus continuam golden. Com a feature ON,
Falmer/Vampire/Yarl continuam golden; BlackSmithTorso muda intencionalmente e
Fur Pants muda indiretamente por receber o Torso como collider. Por isso a
feature fica opt-in até inspeção live conjunta Torso+Pants.

Handoff operacional completo: `TENSION_PLUMB_HANDOFF.md`.

## 2026-07-12 — marco 5: isolamento de layers e promoção default-on

O drift indireto do Pants foi eliminado ampliando o contrato interno do solve:

- `outSolvedVerts`: geometria visual final, usada por métricas/export/writeback;
- `outColliderVerts`: proxy opcional usada somente como collider downstream.

O tension-plumb agora executa depois do true-final body floor. A cópia de entrada
é, portanto, o baseline golden exato e vira `outColliderVerts`; a correção outward
fica apenas em `outSolvedVerts`. Isso impede que a melhoria local do Torso altere
Pants, belts ou outros garments resolvidos depois.

Gate final:

- BlackSmithTorso melhora contra sculpt: result→sculpt `0.753753→0.730634`,
  reverse `0.786777→0.752858`;
- body/layer counts e piores gaps permanecem idênticos ao baseline;
- SHA repetido: `010ECC9F32A990B80DA256E348F26618932C52CB191801F49A296ACD4D846114`;
- os outros 17 shapes permanecem no baseline;
- golden atualizado somente em `corpus/golden/fur_blacksmith_rm_heavy.json`.

Decisão: `DRAPE_TENSION_PLUMB` passa a default ON. Rollback live por
`tension_plumb_off.flag`; A/B de harness por `DRAPE_TENSION_PLUMB=0`.

## 2026-07-12 — marco 6: gate visual live falhou

O usuário testou a build live e informou que o defeito perceptível não foi
resolvido. Os hashes das DLLs build/deploy eram idênticos, portanto não foi uma
DLL antiga ou cópia bloqueada. A promoção baseada em métricas foi revertida:

- `DRAPE_TENSION_PLUMB` volta a default OFF;
- golden Blacksmith volta ao baseline;
- `tension_plumb_on.flag` permanece disponível somente para experimento;
- código, proxy de layer e resultados ficam preservados para diagnóstico.

O render explica a discrepância provável: o contact-tension melhora sobretudo a
faixa média (`z~52`) e perde suporte antes da porção baixa do under-butt; a
variante persistente alcança mais baixo, mas não reproduz o perfil do sculpt com
força suficiente sem degradar outras métricas. Próximo trabalho depende de
localizar visualmente qual sintoma continua: fenda central, under-butt/perfil, ou
ausência total de mudança live. Métrica escalar não decide esse ramo.

## 2026-07-13 — marco 6.5: Path A (religar o PBD loose) falsificado contra os sculpts

Investigação Opus, registrada aqui para o handoff (rótulos de métrica CORRETOS):
religar o caminho LOOSE_DRAPED (`DRAPE_MERGE_LOOSE=0`) foi medido contra os
hand-sculpts nos garments loose. Erro POR ÍNDICE (`tools/error_map.py`):

| garment | MERGED | PBD loose ("verlet") |
|---|---:|---:|
| falmer Cloth | 2.490 | 4.42 |
| vampire robe | 1.186 | 2.18 |
| yarl Cloak | 2.046 | 2.10 |

O PBD loose amontoa as tiras do hem do falmer (banda z<30: 8 verts, std radial
0.28, vs sculpt 44 verts, std 4.63) — compressão lateral e ausência de
estrutura de spread. Determinístico (SHA-idêntico), sem colapso de maxDisp.
Decisão: NÃO religar; MERGED segue como baseline. O caminho é este spike
(Path B). Plano: `PATH_B_SHELL_SOLVE_PLAN.md` (v2.1).

## 2026-07-14 — fase 0.0: proveniência dos ground truths (S0.0a/b)

Inventário completo em `fixtures/pathb/GROUND_TRUTH_CANDIDATES.md` (tracked),
candidatos preservados em `fixtures/pathb/sculpt_candidates/`. Achado central:
o OSD do falmer foi salvo em 2026-07-12 21:26 com conteúdo a 0.211 da nossa
produção (contaminado por um Smart Conform salvo); o hand-sculpt verdadeiro
sobrevive só nos snapshots OBJ de 2026-07-09 (`1b52898da8fd`, duas cópias
byte-idênticas). Blacksmith/yarl/vampire: candidatos concordam (limpos).
S0.0c (decisão do usuário) pendente.

## 2026-07-14 — fase 0.2/0.3: infra de métricas, baselines, preflight H3a

Ferramentas novas (branch wip/path-b-shell-solve): `tools/pathb_metrics.py`
(indexed + surface fwd/rev EXATO via KD + point-triangle, com gate de exatidão
vs brute; integridade vs base; hem por IDs fixos) e `tools/pathb_gate.py`
(runner + scoreboard). Baselines de produção MERGED em `fixtures/pathb/BASELINES.md`.

Sentinelas atuais (falmer benchado): blacksmith, yarl Clothes, yarl Cloak.
yarl Clothes CONFIRMADO entra no solve (`merged-preset: Loose Draped -> Fitted`).

**Preflight H3a (piso de compressão de aresta <0.95×rest_base) = REJEITADO.**
O sculpt tem MAIS arestas comprimidas que o defeito em TODOS os sentinelas
(blacksmith sculpt 958 / prod-defeito 859 / shell-defeito 546; yarl_clothes
857/793; yarl_cloak 232/231). O sculpt faz PREGAS ⇒ um piso que impede
compressão brigaria com o alvo. Robusto a defeito-produção vs defeito-shell e
à contagem exata (a intuição do Codex, generalizada aos 3).

**Contexto shell-base (rest=base, 6 steps, 10 iters) vs produção:**

| garment | shell surf_fwd | prod surf_fwd | shell indexed | prod indexed |
|---|---:|---:|---:|---:|
| blacksmith | 0.997 | 0.756 | 1.851 | 1.544 |
| yarl_clothes | 0.481 | 0.259 | 1.191 | 0.839 |
| yarl_cloak | **1.194** | 1.227 | **1.934** | 2.046 |

Achado central: **o shell-base JÁ SUPERA a produção no yarl_cloak** (garment de
queda-pura/gravidade) — o mais forte sinal a favor do Path B até agora. Perde
nos garments com estrutura fitted/larga (blacksmith, clothes), que é onde H3b
(suporte de baixa freq) e a integração das outras leis precisam fechar o gap.
Além disso o shell-base tem topologia mais limpa (0 flips no blacksmith vs 1 da
produção). PENDENTE: S0.3a descriptor de banda (H3b) + S0.3c ablação de anchors.

## 2026-07-14 — marco 7: preflight de necessidade — H3a E H3b REJEITADOS

Ferramenta nova: `tools/pathb_band_descriptor.py` — circunferência por banda
CORRETA (iso-linha material: mesma fração de aresta em base/mesh; invariante a
diagonais e subdivisão — selftest de cilindro passa; seams unidos; eixo PCA por
componente). Substitui o descriptor antigo (somava diagonais, por Z global).

**H3a (piso de compressão de aresta) = REJEITADO.** O sculpt tem MAIS arestas
<0.95×rest_base que o defeito em TODOS os sentinelas (blacksmith 958 vs 859
prod / 546 shell; clothes 857 vs 793; cloak 232 vs 231). O sculpt faz PREGAS ⇒
um piso de compressão briga com o alvo.

**H3b (piso de circunferência por banda) = REJEITADO.** Nenhum defeito COLAPSA
(bandas <0.9× = 0 em todos). Os garments EXPANDEM (guarda-chuva), não colapsam
— um piso não ativa. E crucialmente a circunferência por banda do SCULPT ≈ a do
DEFEITO (blacksmith defeito 1.10→1.55 vs sculpt 1.10→1.68 ao longo de s; o
sculpt expande TANTO ou MAIS). Nem piso nem teto de circunferência separam
sculpt de defeito.

**Conclusão do preflight (fecha a §2.4 do plano):** as duas leis materiais
escalares propostas (compressão, circunferência) NÃO capturam a diferença
sculpt-vs-defeito. A diferença NÃO está no comprimento de arco por banda (quase
iguais) — está na DISTRIBUIÇÃO/POSIÇÃO do pano dentro da banda (o sculpt pendura
reto/plumb; o defeito segue o corpo). Isso reconfirma o marco 1 ("propriedade
global de pattern/silhueta, não identificável por leis locais") e aponta para o
mecanismo de SUPORTE/PLUMB (tension-seeded, marco 4 — melhorou métrica mas
falhou visual) + tratamento de ANCHORS/REST (H3c) como os únicos leverS vivos.

**Sinal a favor do Path B:** shell-base JÁ SUPERA a produção no yarl_cloak
(gravidade pura). O gap está nos garments fitted/largos, onde o suporte-plumb
precisa entrar SEM as leis escalares rejeitadas.

PENDENTE: S0.3c ablação de anchors (quanto do imprint entra por anchor-vanilla
vs rest vs contato). Depois: Fase 1 REFORMULADA — não implementar compressão
nem circunferência; investigar suporte-plumb dentro do loop + rest autoral.
