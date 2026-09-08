# Handoff: manto Telvanni — bolha atrás da coxa + auto-clipping residual

## O defeito, em uma frase

O `TelvanniBody_female` (manto, 2785 verts / 4242 tris, slider `RM_Heavy`)
atravessa **a si mesmo**, e a correção que reduziu isso criou uma **bolha atrás
da coxa**. O usuário testou e diz: "ainda está com um cadinho de balão e
clipping". Os dois defeitos são reais e estão em tensão medida.

## Laço de reprodução (leia isto antes de qualquer coisa)

Não meça pelo corpus: ele roda numa configuração de flags que o usuário não usa.
O replay abaixo reproduz a pose publicada dele **byte a byte** (já verificado
contra dois NIFs que ele exportou: diferença 0.00001).

```
python  = C:\Users\T-Gamer\AppData\Local\Programs\Python\Python311\python.exe
harness = build/algo/Release/drape-harness.exe      (Release, NÃO RelWithDebInfo)

env DRAPE_DUMP_DIR=<dir> \
    DRAPE_STRICT=1 DRAPE_SHELL_SOLVE=1 DRAPE_SHELL_TRANSACTIONS=0 \
    DRAPE_SHELL_BODY_EVICT=1 DRAPE_POSTERIOR_CHORD=1 \
    DRAPE_LATE_LAYER_TUCK=1 DRAPE_ORIENT_REPAIR=1 \
    build/algo/Release/drape-harness.exe --replay-dir <dir-com-.drcap>
```

Capturas da cena viva dele: `captures/yarl/*Telvanni*2113*.drcap` (manto + calça).
O harness **ignora arquivos .flag por construção** — as flags viram env vars.

`DRAPE_DUMP_DIR` faz o motor despejar, por peça:
`<shape>__authored.obj`, `__production.obj` (conform do host), `__body.obj`,
`__final.obj` (o que é publicado), `__candidate.obj`/`__shellentry.obj`.

`DRAPE_PATHB_TAIL_TRACE=1` despeja a malha em **cada estágio** da cauda
(`drape_tail_trace_<peça>_NN_<estágio>.obj`) — é o que localiza qual passe
causa o quê.

Regressão: `python scripts/check_corpus.py --harness build/algo/Release/drape-harness.exe`

## O que já está medido

Auto-interseções do manto (pares de triângulos da própria peça que se cruzam):

| | |
|---|---|
| malha autorada | 190 |
| conform do host | 520 |
| nós, antes do conserto | 576 |
| **nós, hoje** | **394** |

Traço por estágio, novas em relação ao host:

```
00-shell .. 14-self-line   409 total,  25 novas   <- MELHOR que o host
15-panel (panel-flatten)   528 total, 315 novas   +290
16-cleft (cleft-bridge)    578 total, 391 novas   +76
```

**O panel-flatten é o dono.** Ele achata cada painel pendurado em direção à
curvatura autorada, movendo o vértice pela própria normal, 8 iterações, sem
saber que existe outro painel da mesma peça a ~2u.

Assinatura dos cruzamentos: os dois triângulos ficam **perto no espaço**
(2.66u de distância na malha autorada, mediana) e **longe na superfície**
(13 saltos de face). É tecido em camada, não ruga local nem painel distante.

**Desligar o panel-flatten não é opção:** sem ele o BlackSmithTorso vai de 4.07
para 12.70 de esticamento e o Cloak de 3.53 para 13.31 — os dois acima do teto
de descarte (8.0) e portanto publicados como conform do host.

## O conserto que está no código hoje

Dentro do panel-flatten (`DRAPE_PANELS_FREE_SPACE`, ligado, escopado ao caminho
do shell):

1. **Piso pela separação autorada** — um vértice não pode chegar mais perto de
   outro painel da mesma peça do que o autor os deixou. Só a componente que
   *aproxima* é limitada. Gêmeos de costura exata entram na vizinhança (sem
   isso, a escada de gêmeos coincidentes do Blacksmith conta como "outro painel
   a distância zero" e congela o passe).
2. **Só o que o passe causou** (`DRAPE_PANELS_ONLY_CAUSED`) — a correção só age
   se a distância ao parceiro *diminuiu* desde a entrada do passe.
3. **Cap de aresta iterado** (`DRAPE_PANELS_CAP_ROUNDS`, 12) — era uma varredura
   Gauss-Seidel única e não absorvia o degrau que o limite cria.
4. **Escada de degradação** — se o esticamento piorar vs antes do passe, **ou**
   ficar acima do teto 8.0, o passe é refeito sem o limite.

E fora dele: **cada ciclo do piso final de corpo virou transação**
(`DRAPE_FINAL_FLOOR_TXN`) — ele empurrava/capeava/soldava 3× sem nunca verificar,
e amplificava esticamento até derrubar peças.

## O que sobra

Região z 20–60, costas (atrás da coxa), 165 vértices. Deslocamento **para fora**
comparado à entrada do panel-flatten:

| | |
|---|---|
| panel-flatten sozinho, sem o limite | +0.112 de média, 59 verts > 0.3u |
| a correção do limite | +0.100 de média, 28 verts, pico 1.10u |

Ou seja **metade da bolha é do próprio passe** e já existia antes (o usuário
não reclamava dela); a metade nova é da correção.

## Já tentado e REFUTADO (não repita sem motivo novo)

Todas medidas no replay dele; "cai" = `wouldPublish 0`, peça sai como conform
do host.

| tentativa | manto | custo |
|---|---|---|
| repulsão vértice-triângulo pós-fato | 225 → **275** cruz | piora: depois de embaraçado o ponto mais próximo está do lado errado |
| fechamento por vértice na saída do passe | 576 → 462 | Blacksmith 4.07→12.06 e Cloak→9.77, **caem** |
| reduzir força global (`DRAPE_PANELS_SMOOTH`) | 576 → 503 | esticamento 4.87 → 7.37 |
| espaço livre = metade do vão | 576 → 508 | Blacksmith **cai** |
| piso = min(autorado, separação de entrada) | 31 verts bolha | 441 cruz (pior) |
| dividir a separação 50/50 entre os dois painéis | 47 verts (espalha) | 402 cruz, Blacksmith 4.12→7.37 |
| correção limitada ao avanço do passo | 24 verts, pico 0.57 | 544 cruz (perde o conserto) |
| avanço acumulado desde a entrada | 45 verts | 541 cruz (pior nos dois) |
| **mover o painel interno (escondido)** | **24 verts, pico 0.73** | 431 cruz — **o usuário viu o clipping VOLTAR** |
| suavizar o campo de correção | — | manto 4.87→9.06, calça pen 88→141 |
| piso de corpo por amostra de face, sem teto de passo | — | manto 4.87→9.06, pen 5→36 |
| guardas existentes do passe (component / blend) | — | são por COMPONENTE; o manto tem 2 após soldar costura, então revertem a malha inteira (`rolled=2/2`) ou aceitam `alpha=0.000977` |

## O problema de método que eu não resolvi

Na última tentativa o contador global subiu pouco (394 → 431 cruzamentos) e eu
julguei aceitável — o usuário abriu e **viu o clipping voltar**. A mudança movia
um conjunto **diferente** de vértices, então o total parecido escondia uma troca
de lugar: provavelmente cruzamentos escondidos foram trocados por cruzamentos
**visíveis** (a saia tem uma fenda frontal por onde se enxerga o interior).

**Falta uma métrica de auto-interseção VISÍVEL** — só o que aparece pela abertura
da saia e no lado externo dos painéis. Sem ela, qualquer escolha entre variantes
é no escuro. Se você tiver uma ideia melhor de como medir isso, comece por aí.

## Perguntas abertas

1. Como medir auto-interseção **visível** em vez de total?
2. O panel-flatten precisa mesmo mover o vértice pela normal? Existe uma
   formulação que achate o painel sem aproximar painéis vizinhos —
   por exemplo resolvendo achatamento e separação **juntos**, em vez de
   achatar e depois corrigir?
3. Os +0.112 de bolha que o próprio panel-flatten produz atrás da coxa são
   necessários para o conserto do thigh-bump que ele faz, ou são efeito
   colateral?

## Regras do dono do projeto (importantes)

- Nada de patch específico por outfit ou nome de mesh — o Drape tem de ser
  universal.
- Diagnosticar a causa, não ajustar constante. Uma recusa medida volta ao
  diagnóstico.
- O sculpt à mão dele é **piso**, não alvo.
- Contadores enganam: já aconteceu 4 vezes de um número melhorar e o olho dele
  ver pior.

## 2026-08-20: metrica offline de auto-interseccao exterior

Foi adicionado `tools/pathb_visible_selfint.py` (somente offline; ainda nao esta
integrado ao gate live). Ele mede o **comprimento da curva de interseccao que
tem linha de visao ate o exterior**, em vez de chamar todo par de triangulos de
"visivel". Nao usa nome de outfit/mesh, winding, volume fechado ou modelo
radial, portanto serve para roupas abertas e em camadas.

O script sempre publica o canal `garment_only`; `--body body.obj` acrescenta
`with_body` como segundo canal opaco sem substituir o primeiro. Tem comparacao
opcional contra `--baseline`, JSON deterministico com SHA-256 dos inputs e OBJ
das curvas expostas na pior direcao. Uso:

```
python tools/pathb_visible_selfint.py --garment candidate.obj \
  --baseline current.obj --body body.obj --json report.json \
  --curve-obj visible_curves.obj
python tools/pathb_visible_selfint.py --selftest
```

Validacao sintetica verde: mesmo cruzamento fechado vs exposto por abertura,
corpo opcional, invariancia de escala/winding, costura exata e contato coplanar.

Validacao Telvanni, 162 direcoes, cap-only final contra current final:

| canal | current | cap-only | delta |
|---|---:|---:|---:|
| garment-only solid-angle curve | 12.1217 | 14.6749 | +2.5532 |
| garment-only worst view | 16.4248 | 23.3447 | +6.9199 |
| body-occluded solid-angle curve | 11.4113 | 13.0346 | +1.6233 |
| body-occluded worst view | 15.9853 | 20.5681 | +4.5828 |

O cap-only e **pior**, inclusive com o corpo ocluindo. Ele tem menos pares
"novos vs host" em algumas contagens, mas +42% no pior angulo garment-only.
Isso comprova que identidade/total de pares nao pode ser o gate final. Proximo
passo recomendado: congelar esta metrica em fixtures multi-garment e so depois
adiciona-la como eixo independente ao gate de publicacao; nao chama-la dentro
de cada iteracao do solver ainda.

## 2026-08-20: auditoria causal do stage 15 e sondas de contato

### Baseline reproduzido

O replay Release dos dois captures Telvanni foi congelado com os mesmos flags
do Outfit Studio. Para `TelvanniBody_female / RM_Heavy`, o resultado atual
publica com stretch 4.871, bodyPen 5 e a sequencia de auto-interseccoes e:

```
authored 190 -> host 520 -> 14-self-line 409 -> 15-panel 429
             -> cleft 437 -> final 394
```

O shell solicitado e rejeitado; o caminho efetivo e `legacy(shell-rejected)`.
O authored-gap/free-space do panel e aceito (stretch 8.289 -> 4.493). No Cloak
ele tambem e necessario para publicar; no Blacksmith ele e recusado e o passe e
refeito sem o limite.

### Ablacao que separou o efeito util da bolha

`cap-only` (smooth/radial/azimuth/free-space desligados) quase nao move o manto
no stage 15, mas termina pior: stretch 7.483 e 471 crossings. Blacksmith ainda
publica; Cloak cai com stretch 13.313. Separando os termos, o normal-smooth e o
principal criador imediato de crossings no Telvanni, mas sua combinacao com
radial ou azimuth e o que recupera o Cloak. Portanto simplesmente desligar o
passe ou reduzir uma constante nao e universal.

### Causa estrutural

O stage 15 atual e um operador fracionado: restaura uma curvatura escalar por
normal, depois projeta uma distancia Euclidiana sem sinal contra um parceiro
fixo, depois aplica cap de extensao, body floor e weld. Cada projecao pode desfazer
a anterior. Atravessar outra camada e um caminho legal para o primeiro termo;
depois da travessia, a distancia sem sinal perdeu qual era o lado correto e a
repulsao unilateral empurra o vertice consultor para fora. Esse e o mecanismo
da bolha, nao uma constante especifica do Telvanni.

### Sondas refutadas (nao promover nem ajustar)

1. `DRAPE_PANELS_CROSS_GUARD_MODE=1`: line-search com um alpha global. No
   Telvanni, iteracao 0 aceitou apenas 1/128; as outras sete rejeitaram ate
   1/2048. O resultado degenerou para cap-only.
2. `MODE=2`: alpha local por vertice/active-set. Demora ~70 s por iteracao sem
   swept sampling, nao converge e repete a mesma tentativa depois de rollback.
   Alem disso, interpolar vertices com alphas diferentes pode rasgar uma aresta
   mesmo quando entry e proposal sao duas poses rigidamente coerentes; tambem
   desfaz body floor e weld. O final conhecido ficou pior no gate visual
   (with-body p95 +2.497, worst +2.695) e falhou por arm flips.

Identidade exata de par de triangulos continua util como diagnostico conservador
interno, mas nao e gate visual: uma curva fisica deslizando sobre uma aresta troca
IDs sem piorar, e o mesmo ID pode aprofundar sem ser considerado novo.

### Contato assinado com parceiro fixo: REFUTADO

`DRAPE_PANELS_CONTACT_SOLVE=1` (somente harness, default off) substituiu o
authored-gap por contato assinado vertex-face. O lado da face foi congelado na
entrada; a folga foi `min(entryGap, fraction * localEdge)`; a reacao foi
distribuida ao vertice e aos tres vertices da face. Contact, edge cap, body floor
e seam weld foram alternados no mesmo loop.

Os tres sentinelas publicaram, mas a sonda falhou nos eixos de integridade:

| garment | stretch | regressao relevante |
|---|---:|---|
| Telvanni | 4.853 | stage 15 criou 208 pares novos; final 523 crossings vs 394; p95 visivel with-body +0.303 |
| Blacksmith | 4.065 | 27 pares novos; bodyPen 30 -> 32; worst support edge 1.953 -> 3.624 |
| Cloak | 3.525 | 14 pares novos; self final 11 -> 29; max displacement 9.89 -> 13.11 |

O log do Telvanni terminou com 2717 correcoes, 3906 violacoes ainda abertas e
violacao maxima 6.957; Blacksmith 694/671/7.179; Cloak 465/2045/6.648. Portanto
o ganho de stretch nao representa convergencia de contato.

Dois defeitos estruturais explicam a falha. Primeiro, o parceiro e escolhido
uma vez na entrada: depois que a geometria se move nao ha re-pair, broadphase
dinamico nem contato edge-edge. Segundo, o acumulador Jacobi divide cada
vertice pelo seu proprio numero de contribuicoes; isso desfaz a acao-reacao e
torna a resposta dependente de valencia/densidade. O gate transacional da sonda
olhava apenas worst stretch e por isso aprovava regressao em crossing, flip,
collapse, body e suporte.

**Nao promover, nao ajustar constantes e remover antes de qualquer commit.**
Contato so volta a ser candidato como solve acoplado de pares dinamicos VF+EE,
com passos protegidos contra atravessamento e gates completos. A proxima sonda
menor e substituir apenas o umbrella normal-smooth por bending diedral com rest
autorado, mantendo os demais termos e medindo Telvanni/Blacksmith/Cloak.

## 2026-09-01: sonda de bending diedral v3 — REFUTADA

Foi implementada somente no harness uma substituicao do umbrella-normal do
stage 15 por diedros assinados em repouso. A sonda tentou tratar copias de UV
como um unico ponto material apenas quando duas bordas de fronteira coincidentes
formavam uma costura nao ambigua; tambem tinha rollback por iteracao para
stretch, faces degeneradas, flips e penetracao de corpo.

O primeiro replay revelou um defeito no proprio probe: a normalizacao
deterministica do union-find sobrescrevia `parent` enquanto ainda fazia
`findRoot`, podendo produzir `UINT32_MAX` como indice. Isso causou uma excecao
contida antes do stage 15. O defeito foi corrigido localmente para diagnosticar
o experimento e **nao foi mantido**.

Com esse defeito removido, o replay canonico do manto Telvanni, com
`DRAPE_PANELS_FREE_SPACE=0` para isolar a hipotese, chegou ao probe e foi
rejeitado na primeira iteracao:

```
activeMaterial=1208, hinges=3940, batches=23
HINGE-REJECT iter=0 stretch=32.2834 ceiling=8.29047
PROBE-REJECT (restored stage entry)
```

Isto falsifica a substituicao isolada: bending diedral sem uma membrana e
contato acoplados permite uma grande mudanca angular comprar deformacao
in-plane. Nao e um problema de parametro do Telvanni; diminuir a forca apenas
transformaria a sonda num patch de ganho sem resolver a falta estrutural. Todo
o codigo v3 foi removido apos a medicao; producao continua no baseline anterior.

`tools/pathb_visible_selfint.py` esta disponivel como diagnostico offline
experimental de auto-interseccao visivel. A versao atual une apenas pares de
bordas coincidentes ao comparar, mas ainda nao e gate de publicacao: malhas com
paineis distintos coincidentes na borda podem exigir uma semantica material que
o arquivo nao conhece. Use-o para comparar candidato e controle depois dos
gates de integridade, nunca para aceitar uma mudanca sozinho.

## 2026-09-01: a inversao nasce no salto dos anchors â€” CONFIRMADO

O problema seguinte a atacar nao e uma roupa, uma colisao, nem um parametro de
gravidade. `PathBSimV2Solve` calcula o `anchorTarget` do passo de morph inteiro,
aplica-o imediatamente a todos os vertices fixos e so entao inicia o loop de
stretch/bending/contact nos vertices livres. Isso cria uma condicao inicial
invalida: parte da superficie ja esta no suporte Heavy e a parte livre ainda
esta na pose anterior.

Foi adicionada uma sonda **desligada por padrao**,
`DRAPE_SHELL_ITERATION_ORIENT_BARRIER=1`. Ela testa o resultado acoplado de
uma iteracao completa contra a orientacao e a area autoradas (sem ler nome de
outfit, coordenadas, corpo ou regiao), e faz line-search global de volta ao
ultimo estado se a proposta inverter uma face ou reduzir sua area abaixo de
10%. E deliberadamente uma sonda de diagnostico, nao um reparo para publicar.

O replay mostrou que a barreira ainda recebe faces ruins antes da primeira
iteracao. Com as flags canonicas (`strict`, shell, body-evict, posterior chord,
late-layer tuck, orient repair; sem pit/panels free-space):

| sentinela | flips na entrada do passo 1 | flips apos passo 6 com barreira |
|---|---:|---:|
| TelvanniBody_female RM_Heavy | 55 (2 colapsadas) | 284 |
| BlackSmithTorso RM_Heavy | 8 | 52 |
| Cloak RM_Heavy | 14 | 43 |

No Telvanni sem a barreira o primeiro passo terminava em 166 flips; com ela
termina em 55. Portanto ela impede 111 inversoes novas, mas nao pode desfazer
as 55 que ja estavam presentes quando o loop comecou. Em todos os tres casos a
causa e global: o salto de condicoes de contorno dos anchors. A sonda tambem
continua rejeitada pelo gate comparativo, logo **nada foi deployado**.

### Proximo experimento correto: continuacao conjunta de suporte e pano

Substituir, no harness e atras de uma flag, o teleporte `x[fixed] =
anchorTarget[fixed]` por uma continuacao de carga:

1. preservar os anchors da etapa anterior;
2. calcular o alvo final exatamente como hoje (incluindo skin binding e limpeza
   de contato);
3. interpolar apenas o **campo de suporte** em subpassos, enquanto o mesmo loop
   resolve membrana, bending, contato e costuras;
4. aceitar cada incremento apenas se a barreira de orientacao/area permitir;
   se nao, reduzir o incremento de suporte, e nunca corrigir vertices ou
   regioes individualmente;
5. somente depois medir os tres sentinelas e os gates completos.

Isso e uma continuacao numerica de condicoes de Dirichlet, nao um bridge nem
um hardcode. Tambem exige que a transferencia de metrica use o mesmo nivel de
carga do suporte, em vez de usar o alvo Heavy integral enquanto os anchors
ainda estao a meio caminho. Nao implementar a versao live ate que o harness
complete o caminho sem flips novos e sem regressao de stretch/penetracao.

### Primeira tentativa de continuacao de suporte: REFUTADA E REMOVIDA

Uma sonda `DRAPE_SHELL_SUPPORT_CONTINUATION=1` foi testada somente no harness.
Ela preservava os anchors da etapa anterior, introduzia o alvo final em oito
incrementos globais, validava cada incremento pela orientacao autorada e
atualizava a metrica das arestas fixas junto com os endpoints.

Ela confirmou a parte causal: Telvanni entrou com **0 flips** e permaneceu com
**0 flips** em todos os seis passos, em vez de 55 ja na entrada. Porem falhou
o requisito mais importante de uma continuacao: completar o caminho. A barreira
recusou quase todas as propostas apos poucos incrementos; por exemplo, o passo
1 parou em suporte 0.316 (4/8 cargas), o passo 2 em 0.105 (6/8), e os passos
4--6 praticamente nao sairam de 0. Cada etapa atingiu o teto de 600 iteracoes;
o replay levou 106.6 s e ainda terminou com `flipsLeft=335` no finish, stretch
8.66x e rejeicao pelo gate comparativo.

Isso nao deve ser "ajustado" aumentando tentativas, afinando oito cargas, ou
relaxando a barreira. A sonda aplicava o incremento de suporte antes de os
termos XPBD/membrana poderem responder; o line-search posterior so transforma a
continuidade em uma sequencia de tentativas recusadas. O estado dual (lambdas),
a metrica e o suporte precisam participar de um unico solve incremental, ou a
continuacao nao e numericamente consistente. Todo o codigo dessa tentativa foi
removido antes do checkpoint; permanece apenas a sonda generica de orientacao
desligada por padrao e a evidencia acima.

## 2026-09-06: duas definicoes de suporte incompativeis - CAUSA ISOLADA

O proximo diagnostico separou a saida material da FASE1 da entrada do shell
fisico, sempre no capture canonico Telvanni. Nenhuma roupa, vertice ou regiao
foi reconhecida pelo codigo. Todas as sondas desta secao foram removidas depois
das medicoes e nenhuma DLL foi deployada.

A FASE1 termina no load Heavy com 9 faces ruins dentro do seu dominio material,
mas a malha completa ja tem 46 faces invertidas contra a orientacao autorada.
Ao entrar diretamente no shell com suporte tambem em load completo, a segunda
construcao dos anchors aumenta sucessivamente esse numero:

| estagio | faces invertidas | delta |
|---|---:|---:|
| entrada material | 46 | - |
| projecao individual dos anchors | 65 | +19 |
| smoothing dos anchors | 78 | +13 |
| limpeza de faces da banda fixa | 79 | +1 |

Na primeira iteracao normal do shell, partindo dessas 79 faces, os operadores
produzem:

| operador | faces apos operador | delta |
|---|---:|---:|
| gravidade, stretch, strain e LRA | 79 | 0 |
| bending | 81 | +2 |
| solda de costura | 94 | +13 |
| contato de vertices | 119 | +25 |
| camadas e hull | 119 | 0 |
| plumb dentro do solve | 186 | +67 |
| contato de faces | 186 | 0 |
| auto-colisao | 184 | -2 |

Sem plumb, o passo termina em 163 faces em vez de 300. Sem plumb nem contato,
termina em 147. Removendo tambem self-edge/self-collision, termina em 106. Isso
confirma que plumb e o maior criador dentro do loop, mas tambem que apenas
desligar operadores nao resolve a entrada invalida nem a composicao sequencial.

Uma barreira relativa, que protegia apenas as faces validas no inicio da
iteracao, conteve a primeira proposta de 184 de volta a 79. Em quatro iteracoes
fez 21 backtracks, limitou tres propostas e recusou uma inteira. A recusa gera
movimento zero e falsa convergencia: a direcao conjunta dos operadores ja e
inviavel. Portanto uma barreira global posterior e diagnostico, nao solucao.

### Prova decisiva: preservar o suporte da FASE1

Quando o shell reutiliza exatamente os anchors recebidos da FASE1, sem
reconstrui-los, toda a cadeia inicial fica em 46 faces: projecao, smoothing e
face-clean nao adicionam nada. Mais importante, bending e solda tambem passam a
adicionar **zero**. Contato de vertices ainda adiciona 12 e plumb 66.

Logo os +2 de bending e +13 de costura nao eram defeitos primarios desses
operadores. Eram consequencias de duas definicoes incompativeis do mesmo campo
de suporte:

- FASE1 usa `support = baseVerts + raw` e o reimpoe exatamente;
- o shell recalcula `anchorTarget` a partir de `baseVerts + raw`, depois altera
  cada vertice por skin/body projection, smoothing e limpeza da banda fixa.

O teste de skin binding por triangulo mais proximo piorou a propria banda: 46
faces na entrada viraram 107 apos a projecao (101 apos smoothing e 107 apos
face-clean). Transportar cada anchor independentemente nao preserva a coerencia
da superficie e esta hipotese tambem foi rejeitada.

### Direcao arquitetural

A correcao universal deve eliminar a duplicacao, nao ajustar pesos do Telvanni:

1. construir um unico campo canonico de suporte dependente da carga, `A(t)`;
2. fazer FASE1 e shell consumirem exatamente o mesmo `A(t)`, sem uma segunda
   projecao ou limpeza escondida na transicao;
3. obter `A(t)` resolvendo a banda presa como superficie coerente
   (membrana/ARAP + nao-penetracao), nao projetando vertices isoladamente;
4. anchors que precisam escapar do corpo devem ser contatos deslizantes ou
   DOFs limitados, nao pontos simultaneamente corrigidos e imutaveis;
5. plumb, contato, costura e auto-colisao precisam de uma composicao acoplada ou
   passos localmente protegidos contra inversao. Uma correcao posterior nao
   torna uma direcao inviavel fisicamente valida.

Ha ainda 67 faces ruins tocando o suporte, das quais 26 sao inteiramente
rigidas. Isso mostra que o conjunto hard-fixed atual pode ser geometricamente
inviavel antes mesmo do pano livre ser resolvido. O proximo trabalho deve ser
prototipar a construcao unica de `A(t)` no harness e provar que a transicao nao
cria faces novas nos tres sentinelas antes de alterar o solver live.

## 2026-09-07: suporte canonico por reparo local - REFUTADO E REMOVIDO

Foi testado no harness o menor prototipo do contrato `A(t)` unico. Ele construia
uma unica copia de `base + raw`, corrigia sua orientacao contra a malha autorada,
preservava gemeos exatos de costura como um ponto e entregava a mesma familia
interpolada tanto a FASE1 quanto ao shell. Nesse modo nao rodavam a segunda
projecao, smoothing ou face-clean dos anchors. Nao havia nomes, coordenadas ou
regras de outfit.

A primeira versao exigia 10% de area **projetada** na normal autorada. Esse
criterio e fisicamente incorreto para pano: uma face pode girar perto de 90 graus
sem colapsar. Ele deslocou o suporte em ate 4.5229u e foi corrigido antes do
veredito. A versao valida usou apenas margem positiva de orientacao de 1%.

No Telvanni Heavy:

| medida | baseline | suporte reparado |
|---|---:|---:|
| faces ruins do suporte (criterio da FASE1) | 67 | 14 |
| faces ruins inteiramente rigidas | 26 | 12 |
| flips ao fim do passo 6 | 363 | 223 |
| flips apos unified finish/audit | 367 | 268 |
| drift maximo introduzido no suporte | 0 | 4.4555u |
| gate comparativo | rejeita | rejeita |

O experimento confirma causalidade: melhorar o embedding do suporte reduz muito
as inversoes posteriores. Mas tambem refuta reparo local de faces como forma de
construir `A(t)`: correcoes independentes acumulam drift grande, ainda deixam um
suporte inviavel e nao produzem um shell publicavel. Rodar os outros sentinelas
nao mudaria o veredito depois da falha do alvo primario.

Todo o codigo e a flag `DRAPE_MATERIAL_CANONICAL_SUPPORT` foram removidos; o
harness Release recompila limpo e nenhuma DLL foi deployada.

### Proximo passo reduzido pela evidencia

`A(t)` precisa ser a solucao de um unico problema variacional da banda, nao uma
pose do host seguida de consertos. O prototipo seguinte deve conter no mesmo
solve:

- fidelidade **soft** ao morph `base + raw` (o host e dado, nao Dirichlet);
- energia de membrana/ARAP entre vizinhos e gemeos de costura unificados;
- desigualdade unilateral de nao-penetracao no corpo interpolado;
- barreira de orientacao durante o passo, nao reparo posterior;
- continuacao de carga com estado anterior como inicializacao.

So a saida desse problema deve ser `A(t)`. FASE1 e shell passam a consumi-la sem
reconstruir anchors. Isso ataca simultaneamente as 26 faces rigidamente
inviaveis e o drift criado por projecoes vertice a vertice.

## 2026-09-07: suporte alternado soft/PBD - REFUTADO E REMOVIDO

Foi implementado no harness um segundo prototipo, agora com seis niveis de carga
e estado persistente. A banda fixa recebia fidelidade soft ao morph; arestas
fixed-fixed preservavam a metrica Heavy por XPBD; gemeos de costura eram unidos;
contato unilateral e a desigualdade analitica de orientacao eram aplicados no
mesmo ciclo. A saida substituia os anchors tanto na FASE1 quanto no shell, sem a
segunda cadeia de projecao/smoothing/face-clean.

O resultado mostrou que a composicao ainda nao era variacional: fidelidade,
contato e orientacao eram projecoes alternadas, e apenas as arestas carregavam
multiplicadores persistentes. No Telvanni Heavy:

| medida | resultado |
|---|---:|
| flips brutos do suporte | 67 |
| flips inteiramente rigidos apos solve | 41 |
| projecoes de contato | 691011 |
| projecoes de orientacao | 18733 |
| drift maximo dos anchors | 6.6762u |
| flips no passo 6 | 312 |
| flips no unified finish | 340 |
| gate comparativo | rejeitado |

O contato sem lambda persistente repetiu exatamente o churn ja conhecido: o
termo soft puxa para dentro, contato empurra para fora e orientacao tenta reparar
o dano depois. Alem disso, resolver somente vertices fixed deixa as faces mistas
fixed/free fora do dominio; elas reaparecem como uma fronteira rasgada. Diminuir
ganho, contato ou numero de iteracoes apenas esconderia o conflito.

O prototipo e a flag `DRAPE_MATERIAL_VARIATIONAL_SUPPORT` foram removidos. O
harness Release recompila limpo e nenhuma DLL foi deployada.

O proximo prototipo so vale o custo se for um sistema realmente acoplado: incluir
a faixa fixed+transition+primeiro anel livre no mesmo dominio, dar multiplicador
persistente a fidelidade e contato, e resolver as desigualdades pelo mesmo estado
dual. Nao repetir uma sequencia de projecoes posicionais com outro nome.

## 2026-09-07: suporte cloth como DOF acoplado - SINAL POSITIVO, AINDA PROBE

Foi implementada no harness a primeira versao que realmente coloca o suporte de
tecido dentro do mesmo estado XPBD do shell. A selecao e universal: usa apenas a
classe compartilhada e a conectividade. Vertices `RIGID` continuam Dirichlet;
vertices `FIXED` de tecido ganham massa por area, fidelidade soft persistente ao
morph e contato no mesmo loop. A orientacao e uma desigualdade unilateral de
area assinada com lambda persistente. Nao ha nomes, coordenadas, anatomia ou
regras de outfit.

Como a FASE1 ja entrega a metrica Heavy completa, esta sonda roda somente o
ultimo nivel de carga. Portanto os numeros abaixo medem viabilidade do suporte,
nao sao ainda um veredito visual de drape nem uma substituicao da continuacao.

No Telvanni Heavy canonico:

| variante | flips fim do loop | flips dominio | flips apos finish | drift suporte |
|---|---:|---:|---:|---:|
| referencia full-load com suporte reconstruido | ~300 | - | ~296 | 0 |
| suporte acoplado, orientacao praticamente ablacionada | 250 | - | 263 | 0.5108u |
| suporte acoplado, dominio suporte+transicao+anel, 1 sweep | 187 | 58 | 243 | 0.5875u |
| mesmo dominio, 4 sweeps | 134 | 25 | 207 | 0.6661u |
| orientacao global, 4 sweeps | **27** | 27 | 168 | **0.4882u** |

A ablação prova dois efeitos independentes: liberar o suporte reduz cerca de 50
flips; a desigualdade de orientacao reduz outros 63 no dominio estreito. Quando
aplicada globalmente, a mesma propriedade topologica leva o loop a 27 flips sem
o drift de 4.46--6.68u dos reparos anteriores.

O gargalo seguinte tambem ficou causalmente isolado. Depois de o loop global
chegar a 27 flips, a solda defensiva exata fora do solve cria **+105** de uma vez
(27 -> 132); o bridge leva a 140 e o unified finish a 168. Desligar apenas essa
cauda para diagnostico preserva 27, mas o gate ainda rejeita por fechamento de
costura/intersecoes/contato. Logo remover segurancas nao e solucao.

### Proximo passo exato

Substituir, somente no probe, `PathBSeamWeld` como atribuicao/averaging por
restricoes de coincidencia XPBD persistentes entre gemeos, antes da desigualdade
de orientacao. Costura, contato, membrana, fidelidade e orientacao precisam
convergir juntos. A solda defensiva e o unified finish so podem desaparecer se
o proprio solve satisfizer seus gates; ate la o modo continua experimental.

Flags do probe (harness, todas OFF por padrao):

- `DRAPE_SHELL_COUPLED_SUPPORT=1`
- `DRAPE_SHELL_COUPLED_ORIENT_GLOBAL=1`
- `DRAPE_SHELL_COUPLED_SKIP_LEGACY_POST=1` apenas para a ablação descrita acima

O probe permanece no fonte para a etapa de costura, compila em Release e nao foi
deployado como DLL.

## 2026-09-07: costura XPBD intercalada - VALIDADA NO LOOP, CAUDA AINDA REJEITA

`PathBSeamWeld` foi substituida, somente no probe acoplado, por igualdades XPBD
vetoriais persistentes entre gemeos exatos. Todas as onze reconciliacoes dentro
do loop passam pelo mesmo operador; fora do probe o caminho antigo continua
inalterado. Costura e orientacao sao intercaladas no mesmo microciclo, em vez de
uma sobrescrever o resultado completo da outra.

A primeira versao executava costura e depois quatro sweeps de orientacao. Ela
terminou com 24 flips, mas reabriu 258 costuras (maximo 5.110u). Intercalar os
dois termos mostrou convergencia monotonicamente melhor:

| Telvanni Heavy | flips loop | costuras >0.001u | abertura maxima | drift suporte |
|---|---:|---:|---:|---:|
| blocos separados, 4 sweeps | 24 | 258 | 5.1102u | 0.4826u |
| intercalado, 4 sweeps | 38 | 183 | 1.5813u | 0.4109u |
| intercalado, 16 sweeps | **12** | **58** | **0.1236u** | **0.4443u** |
| intercalado, 16, costura compliance=0 | 13 | 65 | 0.1263u | 0.8607u |

Forcar compliance zero nao melhorou fechamento e quase dobrou o drift. Isso
refuta a ideia de tornar a solda mais dura; o residual vem da convergencia
conjunta/representacao duplicada, nao de elasticidade excessiva. O default do
probe passou a 16 microciclos, ainda OFF no produto.

Dois sentinelas estruturalmente diferentes confirmam que nao e efeito Telvanni:

| sentinela | faces ruins FASE1 | flips loop | abertura costura | drift suporte | flips apos cauda |
|---|---:|---:|---:|---:|---:|
| BlackSmithTorso Heavy | 0 | **0** | 0.000394u | 0.1290u | 24 |
| vampire_robes_alt Heavy | 16 | **2** | 0.166619u | 0.9241u | 76 |

Nos tres casos a cauda legada piora um estado fisico melhor: Telvanni 12 -> 82,
Blacksmith 0 -> 24, Vampire 2 -> 76. A solda defensiva isolada agora causa
apenas +8 no Telvanni (antes +105), portanto a costura XPBD atacou corretamente
a causa, mas bridge e `unified finish` continuam aplicando projecoes posicionais
fora do sistema e recriando flips.

### Proximo passo

Nao endurecer nem desligar gates. O proximo prototipo deve substituir a cauda
por uma fase de convergencia que reutiliza os mesmos lambdas/DOFs de costura,
orientacao, contato e strain. Em particular, o `unified finish` nao pode mais
reparar flips puxando vertices 30% para uma pose salva e depois soldar por
averaging. Se a abertura residual de gemeos continuar limitando o gate, a
solucao correta e condensar cada grupo de gemeos em um unico DOF durante o
solve, nao aumentar a rigidez de duas copias concorrentes.

Nenhuma DLL foi deployada; todos os resultados acima sao replays Release com o
probe explicitamente habilitado.

## 2026-09-07: a causa dominante restante e tunnelling/self-collision desacoplada

O finish acoplado foi completado no probe: suporte, costura, orientacao e strain
agora reutilizam os mesmos lambdas depois do loop. No Telvanni ele troca a cauda
legada de 82 flips por **16**, mantem o pior edge em **2.02x** e termina com
costura maxima de **0.260425u**. Blacksmith termina com 0 flips e Vampire com 0;
portanto a formulacao de suporte/costura/orientacao generaliza. O gate ainda
rejeita, agora por cruzamentos novos, nao por esses termos.

Foi adicionada instrumentacao topologica generica ao gate. Ela separa pares
dentro do mesmo componente, entre componentes e numero de triangulos tocados;
o log de publish tambem informa `integrityMeasured=0` quando a closure recusa
antes da medicao (os antigos zeros eram structs default, nao resultados).

Telvanni canonico, cauda acoplada:

| estagio | novos | intra-componente | inter-componente | tris tocados |
|---|---:|---:|---:|---:|
| fim do loop | 1883 | 1354 | 529 | 1147 |
| solda defensiva | 1881 | 1352 | 529 | 1147 |
| bridges | 1682 | 1153 | 529 | 1073 |
| finish acoplado | **1751** | **1222** | **529** | **1089** |

Logo a cauda nao cria o problema dominante: o loop ja entrega a roupa
atravessada em grande escala. Nao e uma costura ou painel isolado; 62--70% dos
pares sao do mesmo componente e mais de mil faces participam.

### Ablacoes que fecharam o diagnostico

- Self-edge a cada iteracao em vez de a cada 8: 1751 -> 1303 pares finais.
- Relaxation 0.15 -> 0.50: 1303 -> 1128; 1.0 termina em 1137 e piora
  contato/costura. Frequencia/forca encontram outro equilibrio, nao fecham o
  conjunto.
- O untangle pos-convergencia faz trabalho real (`resolved=21039`) e melhora
  corpo `49@-1.69 -> 2@-0.59`, mas empurra atraves da roupa interna
  `16@-0.80 -> 35@-1.71`; o guard corretamente o reverte.
- Ligar `DRAPE_SHELL_LAYER_CONTACT=1` junto e muito pior: cruzamentos no loop
  1869 -> 6047. Rejeitado.
- Continuar corpo/suportes em seis passos mantendo a metrica Heavy inteira
  melhora pouco: final 1751 -> 1593. O salto full-load agrava, mas nao e a
  causa dominante.
- Um line search do update inteiro reduz o loop para 717, mas precisa rejeitar
  a atualizacao completa (`worstNew=1472`) e a cauda volta a 1522. E granular
  demais e congela fisica valida. Rejeitado como solucao.
- O gate dizia permitir relacoes autoradas mas comparava apenas com producao.
  A opcao diagnostica `DRAPE_SHELL_AUTHORED_CROSS_BASELINE=1` mede a uniao dos
  dois baselines; desconta apenas 16 pares no candidato normal. Essa
  inconsistencia existe, mas nao explica os ~1.7k pares e a opcao fica OFF.

### Causa no codigo

`PathBTriUntangle` e resposta **depois** da penetracao. Ele detecta edge-face ja
atravessado, move vertices sem multiplicador persistente e devolve o estado a
membrana, bending, contato e costura, que podem atravessar novamente no proximo
sweep. Rodar mais vezes/mais forte reduz parte do residuo mas nunca torna a
topologia uma invariante. O sintoma visual de pano torto e a manifestacao dessa
troca repetida entre constraints incompatíveis.

### Proximo passo exato

Implementar no probe uma barreira/CCD **local por par edge-face**, usando o
estado anterior e o proposto para impedir o instante de passagem antes que as
faces se atravessem. O contato precisa entrar no mesmo microciclo persistente
de membrana, costura, orientacao, corpo e strain. Nao repetir o line search do
mesh inteiro, nao aumentar relaxation e nao relaxar o gate. Validar primeiro
Telvanni e depois Blacksmith/Vampire antes de qualquer DLL.

Flags novas de diagnostico/probe, todas OFF por padrao:

- `DRAPE_SHELL_COUPLED_CROSS_AUDIT=1`
- `DRAPE_SHELL_ITERATION_TOPOLOGY_BARRIER=1` (probe global falsificado; manter
  apenas como evidencia/ablation)
- `DRAPE_SHELL_COUPLED_MORPH_CONTINUATION=1`
- `DRAPE_SHELL_AUTHORED_CROSS_BASELINE=1`

Build Release do harness verde. Nenhuma DLL foi deployada e nenhuma flag nova
esta ativa no produto.

## 2026-09-08: rollback topologico local - FALSIFICADO E REMOVIDO

Foi implementada a versao minima do proximo passo para testar a hipotese antes
de investir num CCD completo. A cada iteracao ela comparava a proposta com o
estado fisico anterior, encontrava apenas pares edge-face novos e reduzia pela
metade somente o deslocamento dos seis vertices envolvidos. O restante do pano
mantinha 100% do update. Era topologica, sem nome de shape, regiao corporal ou
eixo de mundo, e ficava OFF por padrao.

Resultado Telvanni Heavy:

| variante | cruzamentos fim loop | cruzamentos fim cauda | flips | seam max | tempo |
|---|---:|---:|---:|---:|---:|
| acoplado anterior | 1883 | 1751 | 16 | 0.2604u | 11.7s |
| rollback local no loop | **991** | 1591 | 15 | 0.2143u | 53.0s |
| rollback local + self-edge toda iteracao | 1100 | 1539 | 15 | 0.2240u | ~72s |
| rollback local tambem na cauda | 991 (901 apos bridges) | 1221 | **40** | **0.5331u** | **156.8s** |

A reducao parcial nao e convergencia. Em todas as 37 iteracoes do loop o
rollback atingiu o limite de 12 ondas; no ultimo update sobraram 94 pares. A
instrumentacao mostrou **zero pares imoveis**: todos continham DOF movel. O
residuo migra para faces vizinhas conforme os vertices recuam, porque parar no
limite de intersecao sem uma distancia positiva deixa o contato tangencial
numericamente instavel. Executar o self-edge toda iteracao piorou, confirmando
que a resposta pos-penetracao nao fornece essa barreira.

Na cauda o mecanismo repetiu 1212 ondas, tocou cumulativamente 126857 vertices,
recriou flips e abriu costuras. Isto e exatamente o tipo de patchwork caro que
nao deve permanecer. Todo o probe `DRAPE_SHELL_LOCAL_TOPOLOGY_BARRIER` e seu
codigo auxiliar foram removidos depois da medicao; nao ficou codigo morto.

### Consequencia para o proximo passo

Nao implementar outro rollback discreto, nem global nem local. O primitive
fisico faltante e uma constraint preventiva com espessura positiva:

1. broad phase por AABB varrida entre `xPrev` e a proposta;
2. narrow phase CCD vertex-face + edge-edge para achar o primeiro TOI;
3. contato unilateral XPBD/barreira antes do TOI, com lambda persistente;
4. esse contato no mesmo microciclo de body contact, membrana, costura,
   orientacao e strain;
5. cache incremental de pares ativos, pois reconstruir todos os cruzamentos
   tornou o replay 4.5--13.4x mais lento.

O teste tambem confirma que `PathBTriUntangle` pode continuar como safety net,
mas nao pode ser a formulacao primaria de self-collision. Nenhuma DLL foi
deployada.

## 2026-09-08: barreira preventiva edge-edge XPBD - PROMISSORA, AINDA PROBE

Foi implementado o primeiro primitive preventivo de espessura positiva dentro
do mesmo loop fisico: `PathBEdgeEdgeBarrier`. Ele opera sobre todas as arestas
topologicas, monta broad phase por AABB em grid uniforme, resolve a distancia
entre os dois segmentos como desigualdade XPBD unilateral e conserva lambda
por par durante o morph step. Nao ha nome de outfit, anatomia ou eixo de mundo.

As exclusoes tambem sao topologicas e derivadas do proprio asset: arestas com
vertice compartilhado, vizinhanca de dois aneis e gemeos exatos de costura nao
colidem entre si. Para nao inflar roupa multicamada nem apagar proximidade que
o autor quis, a distancia alvo de cada par e
`min(espessura, distancia_autorada)`; coincidencia autorada exata e ignorada.

Telvanni Heavy, sweep de compliance com espessura `0.10 * meanEdge` e relax
0.50:

| compliance | cruzamentos loop | cruzamentos finais | intra/inter final | flips | seam max |
|---:|---:|---:|---:|---:|---:|
| OFF (baseline) | 1883 | 1751 | 1222 / 529 | 16 | 0.2604u |
| 0.30 | 1509 | 1449 | 1045 / 404 | 17 | 0.1860u |
| 0.03 | 1372 | 1350 | 1007 / 343 | 16 | 0.2604u |
| **0.003** | **1304** | **1311** | **1017 / 294** | **16** | **0.2293u** |

A resposta e monotonicamente melhor ate retornos decrescentes, sem trocar a
reducao topologica por flips ou abertura de costura. O default do probe passou
a 0.003 com base nesse sweep; a feature inteira continua OFF por padrao.

### Validacao cruzada atual, com baseline medido pelo mesmo binario

| sentinela | variante | loop (intra/inter) | final (intra/inter) | flips final | seam loop/final | tempo |
|---|---|---:|---:|---:|---:|---:|
| BlackSmithTorso | OFF | 189 (95/94) | 118 (92/26) | 0 | 0.000394 / 0.000023u | 8.3s |
| BlackSmithTorso | EE 0.003 | **157 (72/85)** | **114 (82/32)** | 0 | 0.017117 / 0.000023u | 11.6s |
| vampire_robes_alt | OFF | 633 (588/45) | 727 (686/41) | 0 | 0.166619 / 0.080079u | 22.8s |
| vampire_robes_alt | EE 0.003 | **418 (371/47)** | **555 (511/44)** | 0 | **0.108093 / 0.080333u** | 26.2s |

No Vampire, alem de reduzir 34.0% no loop e 23.7% no final, o pior movimento
do loop caiu de 4.382 para 2.737 e a convergencia de 76 para 56 iteracoes. No
Blacksmith a melhora final e pequena, mas nao ha regressao de flips nem da
costura convergida. A distribuicao intra/inter pode migrar alguns pares entre
classes; o total melhora nos tres assets, portanto a evidencia e de um
primitive global, nao de ajuste para Telvanni.

Combinar esta barreira EE com o `PathBSelfCollide` vertex-face antigo nao soma
os ganhos: no Telvanni termina em 1315 pares (1089/226), 19 flips, seam 0.1894u
e ~35s, contra 1311, 16 flips e ~16s com EE sozinho. O VF atual melhora pares
entre componentes mas piora os intra-componente; o combo foi rejeitado nesta
formulacao e permanece OFF.

### Estado e proximo passo

A barreira EE reduz consistentemente tunnelling, mas ainda deixa 1311 pares no
Telvanni e o gate corretamente recusa publicar. Ela nao e pronta para DLL. O
primitive que falta e o complemento preventivo **vertex-face com CCD/TOI**,
formulado com a mesma espessura autorada e lambda persistente. Nao reativar o
VF antigo por simples soma: primeiro ele deve ser substituido por contato VF
por par (quatro DOFs e normal da face), em vez da projecao por vertice que
transforma contato entre paineis em deformacao intra-painel.

Flags do probe, todas OFF por padrao:

- `DRAPE_SHELL_EDGE_BARRIER=1`
- `DRAPE_SHELL_EDGE_BARRIER_COMPLIANCE=0.003`
- `DRAPE_SHELL_EDGE_BARRIER_RELAX=0.5`
- `DRAPE_SHELL_EDGE_BARRIER_THICK=0.10`

Nenhuma DLL foi deployada. Os quatro replays de validacao foram feitos em
Release, com `DRAPE_SHELL_COUPLED_CROSS_AUDIT=1`, e o gate permaneceu fechado.

### Ablacao VF por par estatico - FALSIFICADA E REMOVIDA

Foi prototipado o complemento vertex-face com lambda persistente por par,
reacao barycentrica nos tres vertices da face, exclusao topologica de dois
aneis e distancia limitada pela proximidade autorada. Era universal e OFF por
padrao, mas ainda consultava apenas a pose corrente (sem trajetória/TOI).

No Telvanni, VF sozinho terminou em 1656 pares contra 1751 do baseline
(-5.4%). Com EE junto terminou em 1296 contra 1311 do EE sozinho: apenas 15
pares adicionais, mesmos 16 flips e seam pior de 0.2293 para 0.2642u. Portanto
o operador duplica em grande parte o dominio do EE e nao captura o evento que
falta: um vertice pode saltar completamente atraves da face entre duas poses e
ja aparecer distante do outro lado, fora da banda de proximidade.

Todo o probe `DRAPE_SHELL_VF_BARRIER` e seu codigo foram removidos depois da
medicao. O proximo VF nao deve repetir proximidade estatica: precisa receber
`xPrev` e `xProposed`, construir AABB varrida e resolver o primeiro TOI do par
vertex-face antes de aceitar o update. Esta e agora uma exigencia medida, nao
uma preferencia arquitetural.

### CCD vertex-face com TOI e resposta Jacobi - FALSIFICADO E REMOVIDO

O passo seguinte implementou a parte continua que faltava: AABB varrida dos
quatro vertices, raiz do volume assinado do tetraedro ao longo de
`xPrev -> xProposed`, bissecao do primeiro zero e confirmacao baricentrica de
que o vertice estava dentro da face no instante do impacto. Assim o detector
enxerga inclusive um vertice que termina longe do outro lado da face.

A primeira resposta aplicava a reacao na normal da face ao fim do passo. Isso
era geometricamente errado quando a face girava e terminou o Telvanni com 87
flips no loop e 1609 crossings finais. Corrigida para a normal no proprio TOI,
o detector encontrou cumulativamente 8144 impactos (TOI mais cedo 0.00046),
mas ainda produziu 88 flips no loop e 1438 crossings finais, contra 16 flips e
1311 crossings com EE sozinho. A cauda reparou flips para 17, mas nao recuperou
a topologia nem a costura (`seamMax=0.2655u`).

O erro estrutural nao esta mais no detector: e a montagem da resposta. Muitos
vertices impactam a mesma face no mesmo sweep; somar todas as reacoes em Jacobi
concentra impulso nos tres DOFs dessa face e a vira. Selecionar apenas o
primeiro impacto por vertice nao impede dezenas de vertices de escolherem a
mesma face. Compliance/relaxation apenas modulam essa soma e seriam outra
tentativa de ajustar sintoma.

Todo o probe `DRAPE_SHELL_SWEPT_VF_CCD` foi removido. Um proximo CCD VF precisa
resolver **ilhas de contato**: construir o grafo bipartido vertex-face dos TOIs,
colorir/serializar constraints sem DOFs compartilhados ou resolver o pequeno
sistema da ilha, atualizando a geometria entre impactos. Nao voltar a acumular
impulsos VF independentes em Jacobi. A barreira EE validada permanece o unico
primitive novo no fonte, ainda OFF por padrao e sem DLL deployada.

### Auditoria do grafo CCD - HIPOTESE DE ILHAS CONFIRMADA

Antes de escolher entre coloracao e solver de bloco, foi executado um CCD
somente de leitura sobre exatamente `xPrev -> xProposed`, junto do EE validado.
Ele nao aplicava nenhuma resposta; o resultado permaneceu identico ao EE puro:
1304 crossings no loop, 1311 finais, 16 flips finais e seam 0.229305u.

No ultimo morph step do Telvanni, acumulado pelas iteracoes:

- 1,269,657 candidatos de broad phase;
- 4,791 impactos vertex-face confirmados por TOI + barycentria;
- **2,884 impactos (60.2%) compartilhavam face ou vertice** com outro contato;
- uma face recebeu ate 6 impactos no mesmo sweep;
- um vertice participou de ate 3 impactos no mesmo sweep;
- ate 73 faces estavam compartilhadas numa iteracao;
- a maior ilha conectada teve **65 constraints / 77 vertices**;
- primeiro TOI observado: 0.00045 do passo.

Isto confirma diretamente por que a soma Jacobi virou faces: a maioria dos
impactos nao e independente, e a maior ilha e grande demais para tratar cada
par como se possuisse quatro DOFs exclusivos. Tambem explica o TOI quase zero:
ha contato persistente/chattering que deve permanecer numa ilha ativa entre
iteracoes, nao ser redetectado como uma colisao nova a cada sweep.

O probe observacional `DRAPE_SHELL_SWEPT_VF_AUDIT` foi removido depois da
medicao. Proximo prototipo: cache persistente de ilhas ativas e coloracao
deterministica das constraints por compartilhamento de DOF; dentro de cada cor,
Jacobi e seguro, e entre cores a geometria deve ser atualizada em Gauss-Seidel.
Se uma ilha grande continuar mal condicionada, so ela deve cair para um solve
de bloco. Nenhuma regra depende de outfit, anatomia ou eixo.

### CCD VF colorido e conjunto ativo - FALSIFICADOS E REMOVIDOS

Foi implementada a resposta que a auditoria acima sugeria, sempre como probe
global e OFF por padrao. Os eventos vertex-face foram ordenados por TOI e
coloridos deterministicamente para que nenhuma constraint da mesma cor
compartilhasse qualquer dos quatro DOFs. As cores eram executadas em
Gauss-Seidel e o coupled orientation/seam vinha logo depois, dentro da mesma
iteracao.

Isso resolveu o defeito numerico do Jacobi: em vez de 88 flips, o Telvanni
chegou a 13 flips no loop com relaxacao 1.0 e a 12 com 0.5. Portanto a
coloracao de fato impediu varias respostas stale de virarem a mesma face.
Porem ela nao resolveu a topologia:

| variante | crossings loop | crossings finais | flips loop/final | seam final |
|---|---:|---:|---:|---:|
| EE validado, sem VF | **1304** | **1311** | 16 / 16 | 0.2293u |
| VF colorido, relax 1.0 | 1360 | 1388 | 13 / 16 | 0.1911u |
| VF colorido, relax 0.5 | 1320 | 1332 | 12 / 16 | 0.1820u |

A relaxacao menor aproximou o resultado do EE e melhorou a costura, mas ainda
comprou isso com mais crossings. Nao passou o gate.

Foi entao testado o complemento que faltava: cache ativo por par VF, lambda
persistente, barycentria atualizada a cada iteracao (contato sem friccao) e
liberacao ao deslizar para fora da face ou separar duas espessuras. Ele reduziu
o custo por convergir em 55 iteracoes, mas piorou para 1407 crossings no loop,
1461 finais, 13/18 flips e seam 0.2156u. Foram 16,263 impactos novos, 11,151
reaplicacoes persistentes e ate 535 contatos ativos simultaneos. O primeiro TOI
continuou praticamente zero (0.00003).

Conclusao: o conflito Jacobi era real, mas **nao era a causa suficiente** dos
crossings residuais. Uma desigualdade VF pontual, mesmo colorida e persistente,
redistribui o pano e cria mais intersecoes do que remove. Compliance e
relaxation nao mudam esse veredito. Todo o codigo e as flags
`DRAPE_SHELL_COLORED_VF_*` foram removidos; o fonte voltou a conter somente a
barreira EE preventiva validada, ainda OFF por padrao. Build Release do harness
passa depois da remocao. Nenhuma DLL foi deployada.

Proximo passo recomendado: nao implementar outro solver de contato ainda.
Primeiro fazer uma auditoria passiva de **nascimento de crossing por primitiva**
no intervalo `xPrev -> xProposed`, distinguindo evento VF de evento EE para o
mesmo par de triangulos. O detector final conta intersecao triangulo-triangulo,
mas isso nao prova que VF seja a primitiva que nasceu no tempo continuo. Se os
nascimentos residuais forem majoritariamente edge-edge, o proximo conserto e
CCD/line-search do EE ja promissor; se forem VF, somente entao vale montar um
solve de bloco por ilha. Essa classificacao e topologica, sem nomes, anatomia
ou eixos, e evita repetir mais uma resposta correta para a primitiva errada.

### Auditoria de nascimento por primitiva - EE CONFIRMADO, PROBE REMOVIDO

Foi executada a auditoria passiva proposta acima. Para cada intervalo aceito
`xPrev -> x`, ela coletava somente pares de triangulos que nao cruzavam antes e
cruzavam depois. Para cada par, resolvia continuamente os zeros de volume
assinado e validava barycentria interior para os seis eventos VF e distancia +
parametros interiores para os nove eventos EE. A classificacao usa a primitiva
com menor TOI; eventos a menos de 0.01 entre si contam como simultaneos.

No Telvanni, em 62 iteracoes, houve 15,919 ocorrencias de nascimento e 4,308
pares unicos: 1,063 VF-only, 2,023 EE-only, 1,221 ambos e somente 1 nao
classificado. Mais importante, entre os 1,342 pares nascidos que ainda estavam
cruzados ao fim do loop:

| sentinela | sobreviventes classificados | VF-only | EE-only | ambos | com VF | com EE |
|---|---:|---:|---:|---:|---:|---:|
| Telvanni | 1342 | 256 | 595 | 491 | 55.7% | **80.9%** |
| BlackSmithTorso | 172 | 58 | 88 | 26 | 48.8% | **66.3%** |
| vampire_robes_alt | 459 | 119 | 239 | 101 | 47.9% | **74.1%** |

Nos tres assets, portanto, a maioria dos defeitos sobreviventes envolve EE e
EE-only e de 1.5x a 2.3x VF-only. No Telvanni e no Vampire o primeiro TOI EE
tambem aparece quase no inicio do update (0.00007 e 0.00026); o Blacksmith tem
VF 0.00291 e EE 0.00704, mas a contagem sobrevivente continua dominada por EE.

A auditoria preservou byte-for-byte os resultados da barreira EE estatica:
Telvanni 1304/1311 crossings, Blacksmith 157/114 e Vampire 418/555. Todo o
codigo e a flag `DRAPE_SHELL_CROSS_BIRTH_AUDIT` foram removidos depois da
medicao. O harness Release compila depois da remocao; nenhuma DLL foi deployada.

Conclusao: a hipotese de que o primitive ausente era VF esta rejeitada. O
proximo prototipo deve estender a barreira EE validada para o tempo continuo:
AABB varrida, primeiro TOI edge-edge e desigualdade unilateral aplicada dentro
do solve acoplado, antes de existir penetracao. Nao usar untangle posterior nem
uma forca posicional. Como varios eventos compartilham vertices, a resposta
deve ter agendamento deterministico sem DOFs concorrentes. Tudo deriva da
geometria e da topologia do asset; nenhuma regra por outfit, anatomia ou eixo.

### CCD edge-edge varrido - FALSIFICADO E REMOVIDO

Foi implementado o proximo experimento indicado pela auditoria: AABB varrida,
primeiro TOI edge-edge e resposta antes de aceitar a penetracao. O detector
encontrou eventos reais, mas a resposta continua ficou grande demais e criou
novos crossings nas vizinhancas. Os piores updates de resposta chegaram a
7.8566u no Telvanni, 3.5225u no Blacksmith e 5.3793u no Vampire.

| sentinela | EE estatico loop/final | EE varrido loop/final | veredito |
|---|---:|---:|---|
| Telvanni | 1304 / 1311 | **985 / 1132** | melhora parcial, cauda piora |
| BlackSmithTorso | **157 / 114** | 184 / 120 | regressao |
| vampire_robes_alt | **418 / 555** | 553 / 660 | regressao forte |

O problema nao era falta de deteccao continua. O update no TOI, resolvido como
mais uma resposta local independente, deslocava o vertice compartilhado muito
alem da espessura do tecido e apenas movia o crossing para os pares vizinhos.
Todo o CCD EE varrido foi removido. Nao reintroduzir line search/TOI sem antes
resolver a acumulacao de varias constraints sobre o mesmo vertice.

### Causa numerica da torcao: acumulacao Jacobi sem trust region

A barreira EE estatica foi instrumentada sem alterar sua resposta. A medicao
achou o mecanismo universal por tras das assimetrias e protuberancias:

- no Telvanni um unico vertice recebeu ate **103 constraints EE simultaneas**;
- a espessura solicitada era apenas **0.3519u**;
- a soma Jacobi stale pediu ate **4.3587u** num unico update;
- Blacksmith e Vampire tambem chegaram aproximadamente a 0.9u e 3.35u.

Cada constraint isolada era geometricamente valida. O erro estava em somar
todas como se cada par possuisse DOFs exclusivos. A resultante pode ter uma
direcao lateral arbitraria e ser mais de dez vezes maior que o contato que ela
pretendia corrigir. Isso explica tecido torto mesmo sem um collider empurrando
naquela direcao: o impulso vinha da propria auto-colisao, nao do corpo.

Foi acrescentado um limite agregado por vertice e por chamada da barreira. O
limite e expresso como fracao da propria espessura EE, portanto independe de
outfit, anatomia, escala absoluta e eixo. Ele limita a norma do update somado;
nao escolhe direcao nem posicao alvo.

### Microciclo EE + limite agregado - RETIDO COMO PROBE

A mesma constraint EE persistente agora pode ser resolvida duas vezes por
iteracao: depois de material/seam e novamente depois do contato corporal. Esse
operator splitting evita que stretch/bend/contact acumulem uma iteracao inteira
antes de a auto-colisao participar. O microciclo sozinho melhorou Telvanni e
Vampire, mas regrediu Blacksmith; por isso ele so e candidato junto do limite
agregado.

Sweep com microciclo e cap, medido pelo mesmo binario:

| cap / espessura | Telvanni loop/final | Blacksmith loop/final | Vampire loop/final |
|---:|---:|---:|---:|
| 0.25 | 1245 / 1236 | 167 / 106 | 437 / 603 |
| 0.50 | 1169 / 1161 | 156 / 94 | 345 / 540 |
| **0.75** | **975 / 1022** | **158 / 93** | **491 / 512** |
| 1.00 | 1130 / 1140 | 166 / 94 | 473 / 587 |

O cap 0.75 e o unico ponto que vence o conjunto no resultado final dos tres
sentinelas. Cap sem microciclo falhou (Telvanni 1398/1294, Blacksmith 179/122,
Vampire 460/597), confirmando que as duas partes resolvem problemas diferentes:
frequencia de acoplamento e magnitude agregada.

A repeticao limpa depois de remover os probes mortos reproduziu:

| sentinela | loop (intra/inter) | final (intra/inter) | flips finish | seam max |
|---|---:|---:|---:|---:|
| Telvanni | 975 (821/154) | 1022 (843/179) | 18 | 0.233201u |
| BlackSmithTorso | 158 (75/83) | 93 (71/22) | 0 | 0.000014u |
| vampire_robes_alt | 491 (473/18) | 512 (493/19) | 0 | 0.079722u |
| Cloak | 153 (153/0) | 99 (99/0) | 0 | 0.000361u |

O cap foi acionado milhares de vezes e manteve o maior update aplicado em
0.2639u / 0.1976u / 0.1806u / 0.2546u respectivamente. Ou seja, o resultado
nao veio de um parametro inativo: ele bloqueou exatamente as somas explosivas.

### Gate visual direto do candidato

O gate normal mede `final.obj`. Em strict, a transacao rejeita este candidato
por ainda haver crossings novos e `final.obj` volta ao resultado de producao;
portanto comparar esses arquivos daria um falso "sem mudanca". A avaliacao
correta desta fase foi feita diretamente sobre `candidate.obj`, contra o
`candidate.obj` da barreira EE estatica, em 42 direcoes.

Variacao de curva de auto-intersecao visivel (negativo e melhora):

| sentinela | garment solid/p95/worst | com corpo solid/p95/worst |
|---|---:|---:|
| Telvanni | -18.6% / -17.1% / -24.2% | -16.6% / -13.3% / -22.6% |
| BlackSmithTorso | -5.4% / +2.4% / +4.2% | -5.2% / -2.1% / -3.0% |
| vampire_robes_alt | -7.2% / -5.7% / -6.9% | -7.4% / -5.7% / -6.8% |
| Cloak | -11.9% / -9.7% / -11.0% | -9.9% / -9.3% / -9.4% |

Todos os eixos com o corpo visivel melhoram nos quatro assets. A unica troca e
pequena e garment-only no Blacksmith, cuja pior/p95 curva fica oclusa pelo
corpo na avaliacao real. Isto e evidencia de ganho global, mas ainda nao e
permissao para publicar: o gate topologico estrito permanece a autoridade.

JSONs da medicao:

- `build/algo/edge_microcap075_telvanni/visible_vs_static.json`
- `build/algo/edge_microcap075_blacksmith/visible_vs_static.json`
- `build/algo/edge_microcap075_vampire/visible_vs_static.json`
- `build/algo/edge_microcap075_cloak/visible_vs_static.json`

### Agendamento colorido e contato no finish - FALSIFICADOS E REMOVIDOS

Tambem foi testado ordenar os pares EE, colori-los por compartilhamento dos
quatro DOFs e executar as cores em GS. No Telvanni isso reduziu o loop para 934,
mas a cauda subiu para 1080, com seam 0.260218u e 18 flips. A instrumentacao por
estagio localizou a subida: 919 depois dos bridges para 1080 depois dos 80
rounds de finish; trapped eviction nao era a causa.

Intercalar EE a cada 8 rounds do finish produziu contagens muito boas
(Telvanni 894, Blacksmith 100, Vampire 567, Cloak 104 com coloracao; na versao
Jacobi 915/91/461/100). Porem a versao Jacobi regrediu o Blacksmith visivel com
corpo em +6.9% no solid angle e +0.3% no p95. Cadencia 16 sacrificou Vampire e
nao venceu o Blacksmith sem finish. Nao existe uma cadencia fixa universal
validada, logo tanto a coloracao quanto `FINISH_CYCLE` foram removidos.

### Estado atual e proximo passo (2026-09-08)

O fonte contem somente a extensao preventiva retida, toda OFF por padrao:

- `DRAPE_SHELL_EDGE_BARRIER=1`
- `DRAPE_SHELL_EDGE_BARRIER_COMPLIANCE=0.003`
- `DRAPE_SHELL_EDGE_BARRIER_MICROCYCLE=1`
- `DRAPE_SHELL_EDGE_BARRIER_STEP_CAP=1`
- `DRAPE_SHELL_EDGE_BARRIER_STEP_FRAC=0.75`

Build Release e self-test passam. Nenhuma DLL foi deployada. O comparative
closure gate continua rejeitando o candidate porque exige zero novos crossings
contra a entrada de producao; nao afrouxar esse gate e nao confundir rollback
em `final.obj` com validacao do candidato.

O proximo trabalho deve atacar os nascimentos residuais sem outro impulso
independente: transformar o cap agregado em um trust region realmente acoplado
ao conjunto ativo EE, com aceitacao baseada na reducao topologica do passo (ou
resolver somente a pequena ilha responsavel). Nao adicionar banda corporal,
eixo, nome de mesh ou perfil de outfit. A meta imediata e fazer a barreira
preventiva se aproximar de zero crossings novos; so depois reabrir deploy/DLL.

### Cap comum por componente de contato - FALSIFICADO E REMOVIDO

Foi testada uma versao mais conservativa do trust region: em vez de truncar o
delta de cada vertice separadamente, todas as constraints EE ativas eram unidas
por DOFs compartilhados e cada componente recebia um unico fator de escala.
Isso preservava a resposta relativa e a reciprocidade dentro da componente,
sem qualquer regra de outfit.

O grafo, porem, percola longe demais: as maiores componentes chegaram a 911
vertices no Telvanni, 282 no Blacksmith, 654 no Vampire e 406 no Cloak. Um
contato severo passou a amortecer centenas de contatos independentes.

| sentinela | cap por vertice loop/final | cap por componente loop/final |
|---|---:|---:|
| Telvanni | **975 / 1022** | 1366 / 1316 |
| BlackSmithTorso | **158 / 93** | 188 / 117 |
| vampire_robes_alt | 491 / **512** | **484** / 607 |
| Cloak | **153 / 99** | 187 / 126 |

A pequena melhora no loop do Vampire nao sobrevive ao finish e todos os quatro
resultados finais pioram. O probe e a flag `DRAPE_SHELL_EDGE_BARRIER_ISLAND_CAP`
foram removidos. Consequencia para o proximo desenho: "ilha" nao pode significar
fecho transitivo de todos os contatos ativos. O agrupamento deve permanecer
local ao conflito simultaneo (por exemplo, estrela de um vertice ou pequeno
bloco com raio limitado) e nao propagar o menor passo por uma componente inteira.

### Cap pela folga autorada local - FALSIFICADO E REMOVIDO

Outra hipotese local foi testada: o maximo update de cada vertice passou a ser
0.75 vezes a menor folga autorada entre seus contatos ativos, em vez de 0.75
vezes a espessura media. Isso impediria uma camada fina de receber um passo
maior que sua propria separacao.

Sem piso, Blacksmith melhorou 93->90 e Vampire 512->504, mas Telvanni piorou
1022->1138 e Cloak 99->100. Com piso adimensional de 50% da espessura, a troca
se inverteu:

| sentinela | cap global loop/final | cap local com piso 0.5 loop/final |
|---|---:|---:|
| Telvanni | 975 / 1022 | **960 / 971** |
| BlackSmithTorso | 158 / 93 | **155 / 91** |
| vampire_robes_alt | **491 / 512** | 466 / 564 |
| Cloak | **153 / 99** | 201 / 120 |

A folga local redistribui o erro entre assets; nao o resolve. Nao foi feito um
sweep oportunista de pisos ate achar um numero favoravel, pois isso seria
tuning de corpus sem uma lei validada. Todo o probe e as flags
`DRAPE_SHELL_EDGE_BARRIER_LOCAL_CAP*` foram removidos. O cap por vertice baseado
na espessura media continua sendo o candidato retido.
