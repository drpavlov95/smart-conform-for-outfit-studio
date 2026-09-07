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
