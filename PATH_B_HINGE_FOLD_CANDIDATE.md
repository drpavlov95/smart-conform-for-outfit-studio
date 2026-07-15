# Path B — Candidato: hinge-fold gravity re-pose ("dobra de placas")

Data: 2026-07-15 · Autor da ideia: usuário (brainstorm) · Anotado por: Opus
Status: **PROPOSTO — bloqueado por 2 dependências. NÃO implementar antes do
preflight kinemático passar** (regra do plano: nenhuma família fora do preflight).

Este documento é um adendo ao `PATH_B_SHELL_SOLVE_PLAN.md` (v2.2) e reusa toda
a infraestrutura dele (sentinelas, baselines, `pathb_gate.py`, gates de
integridade, regras de parada). Não é um plano novo.

## 1. A ideia (formulação do usuário)

Um passe de gravidade no FIM do processamento que trata as faces do mesh como
**placas sólidas interligadas por dobradiças maleáveis** (edges/verts). A partir
da região suportada, as placas penduradas **giram para baixo em torno das
dobradiças** — ancoradas nelas — até (a) tocarem um mesh abaixo (corpo, outra
camada, o próprio tecido), ou (b) atingirem a vertical (plumb).

## 2. Por que é um candidato forte (ligação com a evidência)

1. **Ataca exatamente o defeito medido.** S0.3e/S0.3d: a seção transversal do
   tecido tem forma ~certa no lugar errado — o que falta é controlar a POSIÇÃO
   da seção. Rotação de placa rígida move material em arco preservando forma
   local e detalhe autoral. É a operação "mover a seção sem deformá-la".
2. **Corrige o modo de falha da gravidade crua** (rejeitada no log): translação
   para baixo é desfeita pelo stretch quase-inextensível; **rotação em
   dobradiça não estica nada** — é o único movimento kinematicamente admissível
   de queda para tecido inextensível (parentesco: follow-the-leader 1D de
   Müller, rigid origami).
3. **Placas rígidas preservam pregas/detalhe autoral por construção** — a dor
   crônica dos passes de smoothing.

## 3. Posição no pipeline (decidida no brainstorm 2026-07-15)

**Tardio, mas NÃO o último.** Ordem:

```
solve de correção → passes de forma (panel-flatten/hem-smooth/etc)
  → HINGE-FOLD (weld de seams + body floor INTERNOS por passo, como o
    cleft-bridge faz) → rede de segurança final (floor/anti-clip/weld) → writeback
```

Razões: o fold pressupõe forma pronta (só re-posa); precisa do ambiente de
contato FINAL para as condições de parada; e qualquer passe body-following
posterior re-imprimiria o corpo. Precedente de slot: tension-plumb (marco 4/5)
e o plumb-guard do cofrinho (o fix de posição aprovado visualmente) vivem aí.

**Decisão de design em aberto (registrar antes de implementar):** o colisor
para garments downstream deve ser o resultado DOBRADO (senão camadas clipam
contra a posição antiga) — mas o tension-plumb escolheu o oposto para isolar
goldens. Para um fold grande, provavelmente downstream-vê-dobrado; decidir com
medição de layer-clipping nos multi-garment (yarl, lemoria).

## 4. Preflight kinemático (S0.4a — OBRIGATÓRIO antes de qualquer solver)

Measurement-only, sem código de solver, reusando as seções do S0.3e:

1. Para cada banda material abaixo da fronteira de suporte: aplicar a rotação
   IDEAL de placa (em torno da fronteira, até plumb ou contato) às seções do
   candidato e medir se o **dC axial/perp fecha** em direção ao sculpt.
2. Gates de necessidade (o trio padrão):
   - o defeito viola a lei? (as bandas do blacksmith under-butt estão fora da
     posição de queda? — S0.3e sugere que sim)
   - o sculpt satisfaz? (as bandas do sculpt estão em/próximas da posição
     dobrada? — se o sculpt NÃO for alcançável por rotação pura, a família morre)
   - margem mensurável?
3. **Sentinela de falsificação: yarl Cloak.** O sculpt dele está ACIMA da
   produção (dDown<0 em todas as bandas, S0.3e; verificado em world-Z puro:
   média +0.95). Um fold "sempre pra baixo" o afastaria do sculpt. O preflight
   deve mostrar que as CONDIÇÕES DE PARADA (o cloak já está em/acima do plumb
   do seu suporte?) o deixam parado. Se o fold ideal move o cloak para longe do
   sculpt, a família precisa de um gate "já está em repouso" — ou morre.
4. Só se (1)-(3) passarem: escada de protótipo.

## 5. Escada de protótipo (após preflight)

- **P1 — tira 1D (meridiano):** FK fold numa cadeia de placas por meridiano no
  spike, partindo da última dobradiça suportada PARA BAIXO (a rotação de cima
  carrega o subtree; nunca de baixo pra cima — dupla contagem). Julgar nas
  bandas z38-56 do blacksmith (onde o sculpt É plumb) + cloak como anti-regressão.
- **P2 — 2D com coerência lateral:** o problema DURO. Colunas vizinhas param em
  ângulos diferentes (contato) ⇒ cisalha/banding na fronteira (cicatrizes:
  plumb-guard por x-bin criou banding; gravity-shadow por coluna criou
  guarda-chuva; costuras-gêmeas abrem invisível). Opções a comparar: relaxação
  das dobradiças pós-fold (XPBD só nos hinges, placas rígidas) vs média lateral
  com weld por passo. Se P2 exigir tuning por-garment ⇒ PARAR (mod universal).

## 6. Armadilhas conhecidas (carregar do plano + brainstorm)

ordem do sweep (do suporte pra baixo); paradas layer-aware + self-contact
(clump de tiras = lição do verlet); weld de exactSeamGroups por passo;
mobilidade contínua (cutoff duro 0.20 = stretch 6-8x); determinismo (ordem
fixa); gates de integridade (FLIPS! — rotações grandes podem virar faces);
marco 6: métrica melhor ≠ visual aprovado — gate live do usuário é obrigatório.

## 7. Dependências (bloqueiam a implementação, não o preflight §4)

1. **Fronteira de suporte do Codex** (em andamento, commit `43d3081`): define a
   linha de dobradiças ancoradas de onde o fold parte. Sem ela, o fold não tem
   origem confiável (o classifier atual liga populações largas e distantes —
   ver PATH_B_HANDOFF, tabela de bindings).
2. **Ground truth de gravidade limpo**: cloak não mede gravidade (sculpt acima
   da produção); falmer benchado (OSD contaminado). Precisa de re-sculpt do
   falmer (ou outro garment de queda) confirmado pelo usuário. Sem isso, os
   únicos juízes são as bandas plumb do blacksmith + anti-regressão do resto.

## 8. Gates e regras de parada

Os do plano v2.2 na íntegra (baselines de `fixtures/pathb/BASELINES.md`,
integridade sem flips novos, determinismo SHA×2, RM_Thin fixturado, corpus OFF
verde). Alvo primário novo: **redução do dC axial/perp por banda (S0.3e)** nos
sentinelas, sem regressão das 3 métricas nem da cobertura setorial. Parada:
preflight §4 falhou ⇒ família morta, registrar no SHELL_SPIKE_LOG e voltar ao
usuário; P2 só com P1 verde; port C++ só com TODOS os gates + visual live.
