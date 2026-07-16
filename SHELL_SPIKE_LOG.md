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

### 2026-07-14 — CORREÇÃO ao marco 7 (peer-review Codex, verificada)

**RETRATAÇÃO: o "ganho por gravidade" do yarl Cloak NÃO existe.**
1. O shell-base foi rodado SEM gravidade — `--gravity` default é `0.0`
   (`shell_spike.py:1572`) e eu não o passei. Não era um resultado "de
   gravidade"; era shell+continuação com g=0.
2. E nem vence o gate completo: indexed 2.0463→1.9344 (melhora) e surf fwd
   1.2265→1.1940 (melhora), MAS surf **rev 0.9154→0.9439 piora**, fwd **p95
   3.4131→3.8348 piora** e **flips 10→19 pioram**. É sinal MISTO do
   shell/continuação, não evidência a favor de gravidade.
Conclusão anterior corrigida: não há, até agora, evidência positiva de que a
gravidade ajude; há um sinal misto do shell.

**Escopo correto das rejeições:**
- H3a morta apenas como **piso GLOBAL UNIFORME de compressão de aresta**.
- H3b morta apenas como **piso/teto de ARCO TOTAL por banda vs `arc_base`**,
  nos três sentinelas ATUAIS. NÃO elimina: distribuição angular, arcos
  parciais, centro/forma da seção projetada, suporte transportado entre
  bandas, anisotropia, atrito de contato, rest/anchors. E como o falmer está
  benchado, H3b **não** foi falsificada para o caso original das tiras.

**Outras correções aceitas:**
- `apply_graph_plumb_constraint` JÁ roda dentro do loop de iterações
  (`shell_spike.py:1283`) — "colocá-lo no loop" não é hipótese nova. O sinal
  tension-seeded atual é frágil: usa `len-rest` DEPOIS de o stretch tentar
  levar `len→rest`; se retomado, deve usar reação física (multiplicador XPBD /
  impulso de contato), não strain residual.
- Infra é PARCIAL: `pathb_gate.py` ignorava return codes (risco de OBJ stale),
  sem thresholds/PASS-FAIL, sem log de comandos/hashes; `pathb_metrics.py` não
  tem self-intersection nem body-penetration; hem `mean_nn=0` porque os IDs
  incluem seam twins duplicados. Status honesto: "Fase 0.0-0.3b cientificamente
  útil, infraestrutura parcial".
- **RM_Thin JÁ EXISTE**: `captures/yarl/...Yarl_Clothes_2...{Clothes,Cloak,Belt,
  Pants}__RM_Thin.drcap` (smoke: Clothes 1777 verts, replay determinístico).
  O gate G não precisa de sessão de captura live — só validar/hashear/fixturar.

## 2026-07-14 — S0.3c: ablação causal de anchors/contato (desenho do Codex)

Adicionados ao spike 3 fatores ortogonais (default = comportamento atual;
`--anchor-target vanilla|authored`, `--soft-anchor on|off`, `--body-mode
continuation|static`). Gate: default == flags-explícitos byte-idêntico; cada
fator muda a saída (1889/436/736 verts movidos). Hard anchors NUNCA soltos
(sem eles o problema fica sem condição de contorno).

Fatorial 2×2×2 × 3 sentinelas, rest=base, gravity=0, 6 steps, 10 iters.
Medido na REGIÃO LIVRE (authored gap >2u ao ref-body = nunca tocou o corpo) —
a métrica de garment inteiro estava confundida pelo deslocamento da região
ancorada e foi descartada.

**Efeitos principais na região livre (Δ indexed vs sculpt; + = piora):**

| fator | blacksmith | yarl_clothes | yarl_cloak |
|---|---:|---:|---:|
| anchor authored−vanilla | +0.147 | +0.251 | +2.313 |
| soft off−on | +0.014 | +0.078 | +0.002 |
| body static−continuation | +0.439 | +0.511 | +0.038 |

**Leituras:**
1. **soft-anchor é irrelevante** (≤0.08 em tudo) — pode sair do desenho.
2. **contato/continuação é NECESSÁRIO e benéfico** (static piora 0.44/0.51 em
   blacksmith/clothes) — o corpo empurrando o tecido faz trabalho real; contato
   não é o vilão do imprint. No cloak é irrelevante (+0.04): ele quase não toca.
3. **anchors não são a fonte do imprint** onde importa: o efeito na região livre
   de blacksmith/clothes é PEQUENO (+0.15/+0.25). No cloak é grande (+2.31) mas
   por POSICIONAMENTO (ele pendura dos ombros; anchor autoral deixa a peça toda
   fora do lugar), não por imprint.
4. **NENHUM fator domina o erro da região livre**: ele continua grande
   (clothes ~2.8-3.3, cloak ~2.6) em TODAS as 8 células. Anchors, soft-anchors e
   contato NÃO explicam a diferença de forma do tecido livre vs sculpt.

⇒ H3c (anti-imprint via rest/anchors/contato) NÃO tem alavanca: a informação
que falta não está nesses fatores. Isso converge com o marco 7 (a diferença está
na DISTRIBUIÇÃO dentro da banda, não no arco) e com o marco 1.

**Próximo indicado (branch "se nenhum dominar" do Codex): S0.3d
measurement-only da DISTRIBUIÇÃO por banda** — centro, largura e quantis
direcionais da seção — para caracterizar o que de fato difere ANTES de
implementar qualquer constraint nova. NÃO implementar constraint sem isso.

## 2026-07-14 — S0.3d: distribuição por banda (measurement-only) — RESULTADO UNIVERSAL

`tools/pathb_band_distribution.py` decompõe a diferença sculpt-vs-defeito por
banda material (iso-linha validada) em: centro da seção (translação), largura
(escala) e raios direcionais normalizados (forma, após remover centro+escala).

| garment | shape_rms | w_ratio | dCenter (máx) | componente dominante |
|---|---|---|---:|---|
| blacksmith | 0.028–0.070 | 0.97–1.08 | 1.23 (s=0.38-0.50 = under-butt) | perp 0.84 + axial 1.13 |
| yarl_clothes | 0.003–0.069 | 0.99–1.01 | 1.45 (s=0.50-0.62) | axial 1.09/-1.39 |
| yarl_cloak | 0.016–0.249 | 0.70–1.02 | 4.52 (s=0.15-0.50) | **axial 3.3–4.4** |

**ACHADO UNIVERSAL: a FORMA da seção já está certa.** Depois de remover centro e
escala, os raios direcionais do sculpt ≈ os do defeito (shape_rms ≤0.07 em quase
tudo). A diferença é quase inteiramente **ONDE a seção fica** (translação) +
escala secundária. O tecido tem a seção transversal correta no lugar errado.

Isso fecha a busca por leis materiais: nem compressão (H3a), nem arco (H3b), nem
anchors/contato (H3c) — e agora sabemos porquê: **nenhuma delas controla a
POSIÇÃO da seção**. A informação que falta é a TRAJETÓRIA DO CENTRO da seção por
banda, com duas componentes:
- **axial** (quão longe o pano desce ao longo do eixo) — grande, dominante no
  cloak (3.3-4.4u): o sculpt pendura MUITO mais pra baixo e mais estreito
  (w_ratio 0.70-0.96). É literalmente "não respeita a gravidade", medido;
- **perpendicular** (onde a seção fica no plano) — até 1.7 no cloak, 0.84 no
  under-butt do blacksmith: "pendura a prumo vs segue o corpo".

**Consequência experimental imediata:** a GRAVIDADE nunca foi testada de fato
(`--gravity` default 0.0; todos os marcos anteriores rodaram com g=0). A
componente axial agora MOTIVA testá-la — não como palpite, mas como a variável
que a medição aponta. Próximo: sweep de gravidade normalizada por continuação
(g_step = G_total/morph_steps) nos 3 sentinelas, medindo se o gap axial fecha
SEM abrir os outros gates.

## 2026-07-14 — gravidade testada de verdade: REJEITADA (e o que isso significa)

Sweep normalizado (`g_step = G_total/morph_steps`, protocolo v2.1), rest=base,
6 steps, 10 iters, 3 sentinelas. indexed vs sculpt na REGIÃO LIVRE:

| garment | G=0 | G=0.5 | G=1.0 | G=2.0 |
|---|---:|---:|---:|---:|
| blacksmith | 1.8195 | 1.8178 | 1.8870 | 2.1746 |
| yarl_clothes | 2.4851 | 2.4793 | 2.5378 | 2.8156 |
| yarl_cloak | 2.6419 | 2.6490 | 2.6563 | 2.6717 |

**REJEITADA.** No cloak — que tem o MAIOR gap axial (3.3-4.4u) e onde a
gravidade deveria ser decisiva — ela piora MONOTONICAMENTE. Nos outros dois,
G=0.5 melhora 0.005 (ruído) e G≥1 piora. A componente axial identificada em
S0.3d NÃO é o pano caindo por gravidade.

**CAVEAT honesto:** a gravidade do spike é crua — `x[:,2] -= g*inv_mass` uma vez
por morph step, e as constraints (stretch/bend/contato) relaxam em seguida. Não
é equilíbrio quase-estático de energia (potencial gravitacional vs membrana/
dobra); com stretch quase-inextensível e anchors no topo, o pano não "cai": a
gravidade só injeta um viés que o stretch desfaz, e mais gravidade = mais
distorção. Portanto o que está rejeitado é ESTA gravidade, não gravidade em
princípio. Um equilíbrio energético de verdade é uma mudança de solver, não um
knob.

### Estado do preflight (Fase 0.3 COMPLETA) — todas as alavancas propostas caíram

| hipótese | veredito | por quê (medido) |
|---|---|---|
| H3a piso de compressão | REJEITADA | o sculpt faz PREGAS: tem mais arestas <0.95×rest que o defeito |
| H3b piso/teto de arco | REJEITADA | arco por banda do sculpt ≈ do defeito; nada colapsa |
| H3c anchors/soft/contato | SEM ALAVANCA | nenhum fator domina o erro da região livre (8 células) |
| gravidade (crua) | REJEITADA | piora monotonicamente no garment de maior gap axial |

E **S0.3d explica todas de uma vez**: a FORMA da seção já está correta
(shape_rms ≤0.07); a diferença é a POSIÇÃO da seção (translação axial +
perpendicular). Nenhuma das leis testadas controla posição de seção.

**Encruzilhada estratégica (decisão do usuário/Codex):** a única família que
ataca posição-de-seção é suporte/plumb (marco 4), que melhorou métrica e FALHOU
o gate visual (marco 6). Agora temos o alvo exato (dC_perp e dC_axis por banda),
mas mirar posição diretamente é alvo POSICIONAL — exatamente o que o Path B
queria substituir. As opções honestas:
(a) equilíbrio energético de verdade (gravidade como energia, não deslocamento) —
    mudança de solver, ainda dentro da física;
(b) suporte/plumb mirado pela caracterização S0.3d — funciona, mas é heurística
    posicional (o que já temos na produção);
(c) aceitar que o sculpt pode não ser fisicamente reproduzível pelo modelo atual
    e reavaliar o alvo.
NÃO implementar nada antes dessa decisão — a regra do plano é não inventar
família fora do preflight.

## 2026-07-15 — S0.3e: preflight corrigido + suporte co-rotacional falsificado

### Retratação formal dos marcos S0.3c/S0.3d/gravidade

Peer review seguido de reprodução encontrou três erros que mudavam o veredito:

1. **S0.3d mediu produção, não Path B.** Os números publicados eram
   `prod_* -> sculpt`; contra o shell-base, Blacksmith chega a `dCenter=3.17`,
   Yarl Clothes a `3.94` e Yarl Cloak a `shape_rms=0.464` na métrica antiga.
   Logo “o shell já tem a forma correta” não estava demonstrado.
2. **Sinal PCA não era gravidade.** O eixo do Cloak usado no marco tinha
   `Z=+0.957`; `dC_axis>0` significava sculpt mais alto. Em world-Z, o sculpt
   está `2.17–4.50u` acima da produção nas bandas críticas, enquanto o spike
   aplicou força em `-Z`. A piora monotônica era esperada. O sweep permanece
   registrado, mas seu significado passa de “gravidade rejeitada” para
   **direcionalmente incompatível/inconclusivo**.
3. **A região S0.3c não era livre.** `authored gap>2` não era histórico de
   contato e incluía hard anchors — 305/361 pontos no Cloak. `body-mode` também
   mantinha colisão nos dois níveis; comparava corpo estático vs continuado,
   não contato off/on.

Portanto as frases “Fase 0.3 completa”, “resultado universal”, “contato não é o
vilão” e a encruzilhada obrigatória `(a)/(b)/(c)` no marco anterior ficam
**SUPERADAS por este marco**. H3a e H3b preservam apenas seus escopos estreitos:
piso global uniforme de compressão e piso/teto de arco total testado.

### Infra S0.3e implementada

- `pathb_band_distribution.py`: eixo material orientado em world `+Z`; delta
  `sculpt-candidate` em XYZ; `dDown=-dZ`; integração de cada iso-segmento com
  quadratura Gauss 2 pontos ponderada por comprimento material autoral;
  correspondência material após centro/escala; cobertura setorial explícita;
  múltiplos `--candidate`; JSON fail-closed; selftest de sinal, translação,
  escala, diagonal e subdivisão.
- `UniformBodyCollider.closest`: closest point/distância unsigned com bound
  global exato por KD de centroides e raio máximo de triângulo, mantendo a
  convenção pseudoassinada existente; fallback de grade de cubo expansível
  provado. No
  caso distante auditado do Cloak, a busca antiga errava 43/175 bindings
  (`1.833u` no pior); a nova consulta deu erro `0` vs brute force nas amostras
  regulares + mais distantes.
- `shell_spike.py --trace-json`: grava fixed/drape/inverse-mass, gap de
  superfície autoral, contagem real de correções de colisão, passos corrigidos,
  gap mínimo e final por vértice.
- `pathb_s03e_h3c.py`: fatorial 2×2×2 reproduzível, comandos/hashes, todas as
  células avaliadas sobre máscaras comuns `never_corrected` e `always_clear`.
- `pathb_s03e_sections.py`: gera produção + shell fail-closed e caracteriza os
  dois lado a lado, sem tuning por garment.

Selftests: band-distribution PASS; attachment closure `0`; movimento rígido
`9.93e-16`; amostra de closest-distance do Cloak `worstDistanceError=0`. Com corpo estático,
`surface-corot` e `authored` geram OBJ byte a byte idêntico
(`3d10426b7a7c673e...`), inclusive com clamp habilitado.

### Seções corrigidas — produção e shell não têm uma única falha posicional

`dDown>0` significa “o sculpt está mais baixo”; `dDown<0`, “mais alto”.

| garment/candidato | faixa dDown | width ratio | shapeP máx | shapeA máx | cobertura incompleta |
|---|---:|---:|---:|---:|---:|
| Blacksmith produção | -1.056…+0.397 | 0.973…1.076 | 0.133 | 0.088 | 0/7 |
| Blacksmith shell | +0.311…+3.419 | 1.030…1.175 | 0.133 | 0.098 | 0/7 |
| Yarl Clothes produção | -1.791…+0.676 | 0.976…1.011 | 0.158 | 0.081 | 0/7 |
| Yarl Clothes shell | -0.008…+3.928 | 1.000…1.170 | 0.205 | 0.111 | 0/7 |
| Yarl Cloak produção | **-4.367…-0.012** | 0.692…1.015 | 0.346 | 0.164 | **6/7** |
| Yarl Cloak shell | **-2.574…-0.067** | 0.672…1.127 | 0.347 | 0.304 | **6/7** |

Conclusão: há sinais mistos por banda; forma/escala também diferem, sobretudo
no shell e no Cloak aberto. O Cloak pede correção para **cima** em todas as
bandas medidas e deixa de ser sentinela de gravidade.

### H3c refeito com histórico real

Máscara primária = móvel + `drape>=0.55` +
`minimum_observed_gap > clearance+1e-6` em todas as oito células
(`fully_free_always_clear`). Ela coincide neste corpus com a máscara
`fully_free_never_corrected`. Tamanhos: Blacksmith `254`, Clothes `350`, Cloak
`10` (o Cloak é pouco informativo por estar 497/553 hard-fixed). Como a máscara
é definida após observar as oito células, ela descreve essa subpopulação
conservadora; não identifica causalidade no restante do pano.

Efeitos principais na máscara primária, `Δ indexed` (+ = pior):

| fator | Blacksmith | Yarl Clothes | Yarl Cloak |
|---|---:|---:|---:|
| anchor authored−vanilla | +0.0047 | +0.0102 | +0.0294 |
| soft off−on | +0.0054 | +0.0150 | +0.1159 |
| body static−continuation | +0.0585 | +0.0651 | +0.0365 |

Leitura permitida: nenhum desses switches fecha o erro **médio global** do pano
móvel sempre afastado. Leitura proibida: concluir daí que anchor/contato não
causam um bump localizado — não há ROI/métrica localizada nesse fatorial. Em
tecido fully-free incluindo vértices contatados, continuação corporal ainda é
uma alavanca grande (`+1.645/+0.470/+0.495` ao trocar para estático).

### S0.3e surface-corot — hipótese física testada e REJEITADA no suporte atual

Foi implementado no spike um terceiro target de anchor:

- closest point exato no corpo-base + barycentria material;
- offset autoral completo;
- rotação própria Kabsch/co-rotacional por triângulo, sem copiar stretch/shear;
- clamp unilateral dos hard targets ao contact floor;
- nenhum sculpt ou posição-alvo entra na lei.

O classificador, porém, chama de suporte uma fração enorme do garment:

| garment | fixed / total | bindings pedidos | distância p95/máx | bindings >4u / >8u |
|---|---:|---:|---:|---:|
| Blacksmith | 1444/2316 | 1726 | 5.726 / 17.101 | 137 / 48 |
| Yarl Clothes | 1145/1777 | 1269 | 2.721 / 14.587 | 42 / 35 |
| Yarl Cloak | 497/553 | 526 | 11.453 / 15.747 | 176 / 68 |

Gate implementado de três médias + flips, uma configuração universal, sem
sweep (p95, degenerados, seams e NaN são reportados, mas nem todos têm threshold
no runner atual):

| garment | indexed | surf fwd/rev | flips candidato/baseline | veredito |
|---|---:|---:|---:|---|
| Blacksmith | 1.9378 | 1.0010 / 1.0478 | 13 / 1 | FAIL |
| Yarl Clothes | 1.3224 | 0.5440 / 0.6241 | 16 / 7 | FAIL |
| Yarl Cloak | 2.4608 | 1.4769 / 0.9908 | 50 / 10 | FAIL |

O controle estático inicialmente revelou um clamp instável; após adicionar
tolerância e retorno exato no estado autoral, a equivalência estática passou
byte a byte. O rerun acima é o resultado corrigido. A ablação sem clamp também
falha as três médias: Blacksmith `1.9210 / 0.9995 / 1.0460` com `0` flips;
Clothes `1.3215 / 0.5443 / 0.6238` com `15`; Cloak permanece
`2.4608 / 1.4769 / 0.9908` com `50`.

Regra de parada aplicada: **sem tuning, sem RM_Thin, sem port C++** depois do
hard-fail dos gates implementados, reproduzido com clamp on/off. O modo
permanece no spike somente para reprodução. A hipótese estreita “transporte
co-rotacional com binding independente ao triângulo mais próximo, support mask
amplo e parâmetros baseline atuais passa os gates” morreu. Isso não falsifica a
família inteira sem sweep. A classificação/condição de contorno de suporte passa
a ser o próximo objeto de pesquisa, não mais uma lei escalar ou um target de
anchor.

### Estado final deste checkpoint

- Produção `MERGED`, DLL e flags live: **intocados**.
- A correção do closest muda legitimamente o default **offline** pré-S0.3e em
  Yarl Clothes (`4cb5cca466a4` antigo → `50cbe35e44be` exato); Blacksmith e
  Cloak permanecem `1bc183425f8b` / `eacf522f53e3`. Depois dessa correção, a
  adição do modo `surface-corot` deixa o default corrigido byte-idêntico.
- Artefatos rastreados:
  - `S03E_H3C_RESULTS.json` SHA-256 `4714aa2eaf413814338fc2610b29d0c94fed5f5f4fd0a00e871e152cd5aa1cb3`;
  - `S03E_SECTION_RESULTS.json` SHA-256 `94dcf7128d8a3a7682c45dd6bba93abfe19cde04ef86ab521f75c6c749b16511`;
  - `S03E_SURFACE_COROT_SCOREBOARD.md` SHA-256 `5c3591cc11bd689f3894dbd875f10511ad43a8d48d2d33c3889ec544ab9d6e0b`;
  - `S03E_SURFACE_COROT_NO_CLAMP_SCOREBOARD.md` SHA-256 `078ffd3e372e0cda30a80365de7afe517c11ca6ec156acdceba5c0adbacd2adc`.
- Handoff operacional: `PATH_B_HANDOFF.md`.

Próximo passo permitido: preflight da **classificação/fronteira de suporte**
(por que Cloak tem 90% fixed; distinguir fitted material de fronteira realmente
presa) e/ou aquisição de ground truth gravitacional limpo. Não construir solver
energético nem plumb mirado antes disso.

## Marco 9 - S04 support-boundary preflight e Heavy gate (2026-07-15)

O passo permitido no marco anterior foi executado sem tocar produção, DLL ou
flags live. A pesquisa separou duas coisas que o classificador antigo misturava:

- mobilidade/`fixed`/soft anchors continuam exatamente como no shell;
- uma máscara decide apenas quais anchors recebem target `surface-corot`.

### Preflight `coherent-v1`

Entradas permitidas: capture, topologia material, corpo base/morph, target raw e
target corot. Sculpt, ground truth, nomes de garment e IDs escolhidos à mão não
entram no classificador.

Contrato congelado: seis morph steps, continuation, `soft-anchor=on`, fallback
`body-vanilla`, clamp unilateral ligado e clearance `0.02`. O gate roda em cada
passo, não apenas no frame final. Ele exige:

- lever/body-edge <= 2 e >=75% de coerência nas arestas incidentes;
- zero flip novo e zero seam regression;
- nenhuma expansão pior que `max(2x, raw)`;
- nenhuma contração pior que `min(0.5x, raw)`;
- nenhuma área de face pior que `min(0.1x, raw)`;
- depois do pruning, todo componente welded por exact seam contém hard support
  e pelo menos uma face material completa.

O peer review encontrou e corrigiu antes do Heavy: um flip intermediário em
Clothes 4/6, clamp vazando para vértices fora da máscara, fallback static
incorreto, consumo de artefato sem provenance, ilhas pós-pruning e ausência de
gate de contração/área. O loader final rejeita schema, tools, ambiente, capture,
contrato, IDs/count/hash ou approval divergentes.

Máscara final (broad -> coherent): Blacksmith `1726 -> 998`, Clothes
`1269 -> 590`, Cloak `526 -> 11`. Todos os selecionados acabaram hard-fixed.
Isso é seguro, mas extremamente conservador no Cloak (2.1% de coverage), logo
não prova uma soft frontier universal.

Todos os 18 registros garment x morph-step passam os gates de target. O
preflight regenera byte a byte (`587da6ee2a060809...`). Static closure é
byte-idêntica a authored nos três garments; continuation repetida também é
byte-determinística; hard supports selecionados têm zero violação final de
contact floor.

### Gate Heavy estrito - REJEITADO

O strict gate agora exige shell rc=0, base fresca capture-paired, finitude,
topologia exata, seis métricas mean+p95 <= baseline x1.05 e integridade sem
piora. Resultado `coherent-v1 + corot + clamp`:

| garment | indexed | surface fwd/rev | flips | veredito |
|---|---:|---:|---:|---|
| Blacksmith | 1.8650 | 0.9930 / 1.0424 | 0 | FAIL |
| Yarl Clothes | 1.2320 | 0.5013 / 0.6000 | 9 | FAIL |
| Yarl Cloak | 1.9342 | 1.1943 / 0.9422 | 19 | FAIL |

Controle shell-vanilla pareado: Blacksmith `1.8508 / 0.9969 / 1.0488`, Clothes
`1.1980 / 0.4832 / 0.5906`, Cloak `1.9344 / 1.1940 / 0.9439`. O pacote fica
misto e <1% no Blacksmith, piora todas as means em Clothes (1.59-3.75%) e vira
quase no-op no Cloak. Integridade é igual ao controle.

Conclusão estreita: o pacote universal que poderia avançar - máscara segura +
surface-corot + clamp - falha 3/3 Heavy. Regra de parada aplicada: **sem
RM_Thin, sem tuning adicional, sem C++ e sem deploy**. Produção `MERGED`
permanece intacta.

Artefatos autoritativos:

- `S04_SUPPORT_PREFLIGHT.json` `587da6ee2a060809cb7ff262133f94e4e4c76b28d43bcc69d1506dd7081e5a13`;
- `S04_COHERENT_V1_STRICT_SCOREBOARD.md` `99505aeaabba77b497f2b4f0839c2655093033f0afd86ac6adef6f3f17993f2a`;
- `S04_SHELL_VANILLA_CONTROL_STRICT_SCOREBOARD.md` `df9416a2c227988a7e603672b5938db4383d5f0d31bcb7b1a4f4398c2d46518b`.

Próximo desbloqueio: ground truth gravitacional/free-hang limpo e uma métrica
local do defeito visual antes de propor outra família física.

## 2026-07-15 — S0.4a: hinge-fold (dobra de placas até a vertical) FALSIFICADO no preflight

Candidato do usuário (`PATH_B_HINGE_FOLD_CANDIDATE.md`): placas rígidas +
dobradiças, dobrando do suporte para baixo até contato ou vertical (90°).

**Instrumentação que NÃO funcionou (registrada para não repetir):** duas
tentativas de "fold ideal" por teleporte independente (plumb sob a âncora
geodésica, com e sem âncoras de contato; `tools/pathb_s04a_fold_preflight.py`)
produziram alvos a ~12u tanto do defeito quanto do sculpt — sendo que S0.3e
mostra que defeito e sculpt diferem entre si por ≤4.5u. Idealização inválida
(colapso lateral pro meridiano da âncora + geodésica errante + âncoras de
preset/bodice dominando); não adjudica a família.

**O teste model-independent que decidiu (pergunta 2 do trio):** ângulo das
arestas-meridiano PENDURADAS (u≥0.55, gap>0.6) com a vertical:

| garment | PRODUÇÃO mean/p50/p90 | SCULPT mean/p50/p90 |
|---|---|---|
| blacksmith | 22.0° / 19.1° / 46.6° | **24.0° / 19.3° / 50.4°** |
| yarl_cloak | 17.1° / 12.7° / 32.3° | **22.2° / 19.5° / 37.6°** |

O SCULPT está IGUAL OU MAIS LONGE da vertical que o defeito, nos dois
garments. "Dobrar até a vertical" move o tecido PARA LONGE do alvo. Trio:
(1) defeito viola a vertical? sim (22°); (2) sculpt satisfaz? **NÃO** (24°);
(3) margem? negativa. **Família "fold-até-vertical" morta no preflight** —
quinto candidato consecutivo que o próprio sculpt rejeita.

**O que sobrevive da ideia:** o insight mecânico (rotação em dobradiça é o
movimento kinematicamente admissível; translação é desfeita pelo stretch)
continua válido e explica a falha da gravidade crua. O que morreu é o ALVO
("vertical"). Sem um campo de ângulos de equilíbrio conhecido, o fold vira
"gire até um ângulo-alvo por região" = heurística posicional com passos extras.

**Padrão acumulado do preflight (nomeá-lo é o resultado):** compressão, arco,
anchors/contato, gravidade-translação e agora gravidade-rotação-até-vertical —
TODOS falsificados PELO SCULPT. Os hand-sculpts codificam um alvo artístico
(~20-24° de queda com flare/pregas/camadas), não uma lei física simples. Isso
fortalece: (i) o caminho S05 do Codex (o IMPRINT LOCAL é objetivo separado,
mensurável, e é o que o usuário VÊ como bump), e (ii) a necessidade de um
ground truth de GRAVIDADE de verdade (re-sculpt do falmer) antes de qualquer
nova tentativa de "queda global".

## 2026-07-15 — P1 (hinge-fold em tira 1D como refino): NEGATIVO em todas as frentes

`tools/pathb_p1_strip_fold.py`: cadeias meridianas por steepest-ascent na base
autoral; FK do suporte (preset+contato 0.6) para baixo; cada aresta mantém o
comprimento ATUAL e gira a direção rumo à vertical por fração λ (nlerp), com
parada por contato (bisseção); weld de seam twins ao fim. Rodado sobre a
PRODUÇÃO do blacksmith, λ ∈ {0.25, 0.5, 0.75, 1.0}.

| λ | imprint (juiz S05) | flips vs base | stretch lateral p95/max | perfil coxa |
|---:|---|---:|---|---|
| prod | 0.08447 / corr .340 | 1 | — | 16.64 → 16.08 |
| 0.25 | **+16.8%** / .338 | 1 | 1.095 / 5.03 | ~igual |
| 0.50 | +31.8% / .259 | 5 | 1.218 / 9.54 | ~igual |
| 0.75 | +43.3% / .199 | 7 | 1.349 / 14.30 | ~igual |
| 1.00 | +49.3% / .160 | 31 | 1.476 / 18.95 | 15.94 |

Leituras:
1. **O imprint PIORA monotonicamente** com λ — o fold-1D não remove o carimbo;
   ele injeta energia de alta frequência NOVA (o caos lateral entre colunas
   vizinhas domina o sinal). Mesmo no λ mais suave (0.25), +16.8%.
2. **Stretch lateral explode** (máx 5→19×) exatamente como previsto: colunas
   vizinhas dobram ângulos diferentes e a fronteira rasga. A esperança "refino
   residual ⇒ problema lateral encolhe" NÃO se confirmou: já em λ=0.25 há
   arestas 5× (localizadas em seams/fronteiras de painel).
3. **O bump visível quase não muda** (perfil da coxa ~16.6→15.9 só em λ=1, ao
   custo de 31 flips). Causa estrutural: o nlerp por-aresta gira TUDO rumo à
   vertical — inclusive o drape que já estava certo — em vez de remover
   seletivamente a componente corpo-seguidora.

**Veredito: a idealização 1-coluna não é só incompleta — ela envenena o
próprio teste.** O fold não pode ser avaliado (muito menos shipado) sem
coerência lateral (P2), e P2 ≈ reconstruir o shell solver com placas rígidas —
a família de custo que os preflights vêm rejeitando. Registrado como negativo;
decisão de investir em P2 vs levers-S05 volta ao usuário.

## 2026-07-16 — marco 8: gravidade quase-estática + fios só-tração (candidato "sim v2")

Direção decidida pelo usuário: cloth sim de verdade (gravidade + toque/não-toque),
julgado por INVARIANTES FÍSICOS + olho do usuário; distância-ao-sculpt vira
informativa (o próprio usuário: o sculpt não é referência fina de queda).

Duas mudanças no spike, default-preserving (byte-idêntico sem os flags):
- `--gravity-mode iteration`: gravidade aplicada A CADA iteração junto das
  projeções (quase-estático XPBD), em vez de 1×/morph-step (o teste antigo
  aplicava 6 doses de gravidade contra 60 de constraints — falha estrutural
  do experimento, não do conceito).
- `--stretch-mode tension`: fios SÓ-TRAÇÃO (c>0) — tecido real não resiste
  compressão (dobra). É o que converte o puxão da gravidade em ROTAÇÃO em
  torno do suporte (o pêndulo emerge; a coerência lateral é resolvida pelo
  próprio solver — vizinhos compartilham vértices), exatamente a mecânica que
  o usuário especificou e que o P1 (colunas independentes) não tinha.

Probe blacksmith (g_iter ∈ {0.02,0.05,0.10}, 6 steps × 10 iters):
- **Invariantes limpos**: flips 0 (produção tem 1); stretch p95 1.86-1.88
  (produção 1.94); penetrações reais (gap<0) = 186 IDÊNTICAS ao shell-base
  sem gravidade (140 âncoras-vanilla + 46 pré-existentes do spike; a produção
  as corrige no floor final) ⇒ **zero penetração nova**. Determinístico.
- imprintE sobe (+27%..+159%) — SUSPEITO DE CONFUSÃO: a métrica lê energia de
  alta frequência projetada no morph; PREGAS legítimas de gravidade são alta
  frequência. Flag pro Codex: validar a métrica em candidatos enrugados.
- indexed vs sculpt sobe com g (informativo; sculpt não é juiz de queda).
- Render: primeira vez que um candidato PARECE tecido — queda reta, sem
  guarda-chuva, SEM rasgos, e o pinch central das costas da produção sumiu.

Pacote visual pro gate do usuário: `tmp/simv2_visual/` (4 garments ×
{produção, g=0.02, g=0.05}), determinístico. Aguardando o olho dele no OS.
