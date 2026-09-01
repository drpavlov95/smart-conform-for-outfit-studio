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
