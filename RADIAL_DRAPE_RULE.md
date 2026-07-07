# Regra Radial de Drape

Data: 2026-07-05
Status: especificação de comportamento / alvo de implementação

Este documento define a regra que o solver do Drape deve seguir para três
comportamentos de roupa: **draped**, **fitted** e **skin-tight**.

Versão curta:

> O corpo pode empurrar o tecido para fora, afastando-o do eixo do corpo. O
> corpo não pode puxar o tecido para dentro de um buraco. Quando o corpo retrai,
> o tecido solto cai na direção da gravidade ou faz ponte até a próxima região
> convexa de suporte.

---

## 1. Modelo radial

O corpo deve ser tratado como um cilindro vertical aproximado.

- O eixo do cilindro é vertical.
- `Z` é altura.
- `R` é a distância radial no plano horizontal, medida a partir da linha central
  do corpo.
- Sair para fora significa aumentar `R`.
- Entrar para dentro significa diminuir `R`.

Em termos de código:

```text
R = distance_xy(vertex, body_center_xy)
```

O centro pode vir do centro de referência do corpo já usado pelo solver, como
`g_refCenterX/Y`.

Esta regra é sobre comportamento radial. Ela não diz que o tecido nunca pode se
mover para dentro por nenhum motivo. Tensão do tecido, forma autorada, costuras e
o tipo de preset ainda podem mover o tecido para dentro.

A restrição importante é mais específica:

> A superfície do corpo não pode funcionar como um ímã que puxa o tecido para
> dentro só porque o ponto mais próximo do corpo está mais para dentro.

Essa é a regra anti-magnetismo.

---

## 2. Contato é uma via de mão única

Contato corpo-tecido é unilateral.

Quando o corpo expande para fora e encosta no tecido:

```text
body R aumenta -> cloth R pode aumentar
```

Quando o corpo retrai para dentro:

```text
body R diminui -> cloth R não deve diminuir automaticamente
```

O corpo pode empurrar o tecido para fora. Ele não deve atrair o tecido de volta
para a superfície corporal.

Essa distinção é o núcleo da regra radial.

---

## 3. Draped

`draped` significa tecido guiado principalmente pela gravidade.

Quando o corpo empurra para fora, o tecido acompanha esse empurrão. Quando o
corpo retrai para dentro, o tecido não acompanha a retração. Ele mantém o raio
atual e continua caindo verticalmente até encontrar outra parte do corpo que o
empurre para fora.

Em uma seção vertical:

```text
corpo expande para fora -> tecido expande para fora
corpo retrai para dentro -> tecido mantém o raio e cai verticalmente
corpo expande de novo -> tecido é empurrado para fora de novo
```

Uma forma simples de pensar no algoritmo:

```text
clothR = raio_autorado_ou_inicial_do_tecido

para cada altura na direção da gravidade:
    supportR = bodyR(altura) + folga_desejada

    se supportR > clothR:
        clothR = supportR       # corpo empurra o tecido para fora
    senão:
        clothR = clothR         # corpo não puxa o tecido para dentro
```

Isso cria uma queda vertical sobre regiões onde o corpo retrai.

### Critério de aceitação

Um `draped` falha a regra se seguir uma concavidade do corpo para dentro quando
não há contato, costura ou restrição autorada exigindo esse movimento.

Exemplos:

- Um robe solto não deve afundar no cofrinho.
- Uma saia não deve desenhar o vale entre as coxas, a menos que isso tenha sido
  autorado ou preso por alguma restrição.
- Um painel solto perto da coxa não deve curvar para dentro só porque a silhueta
  da coxa se afasta do tecido.

---

## 4. Fitted

`fitted` fica entre `draped` e `skin-tight`.

Ele não cai como uma cortina totalmente solta, mas também não copia cada vale do
corpo. Quando o corpo retrai para dentro, o tecido deve fazer uma ponte direta
entre uma convexidade e a próxima.

Em uma seção:

```text
convexidade de suporte -> vale/concavidade -> próxima convexidade de suporte
```

O resultado `fitted` deve ser uma ponte sobre o vale, não uma queda vertical pura
e não uma cópia skin-tight da concavidade.

Isso significa que o `fitted` pode inclinar para dentro ou para fora como parte
da ponte. A diferença é que a ponte vem das convexidades vizinhas, não do ponto
mais fundo do corpo.

### Critério de aceitação

Um `fitted` falha a regra se preservar uma folga quase uniforme em relação ao
corpo dentro de uma concavidade. Folga uniforme em vale é comportamento
`skin-tight`, não `fitted`.

Exemplos:

- No cofrinho, o tecido `fitted` deve passar por cima da fenda.
- Na coxa, o tecido pode acompanhar a silhueta geral da perna, mas não deve criar
  um amassado local onde a malha nem está tocando.
- Em peito, quadril e bunda, o tecido deve simplificar o corpo em regiões
  convexas maiores, em vez de copiar cada reentrância.

---

## 5. Skin-tight

`skin-tight` segue a silhueta exata do corpo.

Nesse preset, movimento para dentro é permitido, porque esse é o comportamento
esperado. A roupa deve preservar a folga local e acompanhar tanto expansões
quanto retrações do corpo.

Em fórmula:

```text
clothR = bodyR + folga_autorada
```

`skin-tight` é o único dos três comportamentos em que "superfície corporal mais
folga" é um alvo válido em todos os lugares.

---

## 6. Concavidades horizontais

A regra radial também vale horizontalmente.

A ideia de `gravity-shadow` cobre parte dos buracos verticais: regiões onde o
corpo retrai ao longo da direção da queda. Mas alguns buracos importantes são
horizontais:

- cofrinho
- vale entre as coxas
- vale interno do peito
- espaços laterais entre massas separadas do corpo

Esses casos não são resolvidos olhando apenas colunas verticais.

Para concavidades na mesma altura, o solver precisa fazer ponte no sentido
angular/radial:

```text
convexidade esquerda -> concavidade horizontal -> convexidade direita
```

`draped` e `fitted` não devem ser puxados para dentro desse buraco, a menos que
exista uma costura, restrição ou contato real ali.

O cofrinho é o caso visual canônico dessa regra.

---

## 7. O que a regra proíbe

Para `draped` e `fitted`, a regra radial proíbe:

- Puxar tecido para o ponto mais próximo do corpo dentro de um vale.
- Preservar folga uniforme em regiões côncavas.
- Aplicar smoothing que recria o vale depois de uma ponte correta.
- Interpretar "perto do corpo" como prova de que o tecido deve seguir o corpo.
- Usar o corpo como alvo de atração depois que o tecido já está livre dele.

A regra permite:

- Colisão do corpo empurrando o tecido para fora.
- Tensão do tecido criando pontes amplas.
- Forma autorada mantendo a roupa mais larga que o corpo.
- Costuras, constraints ou regiões apertadas sobrescrevendo o comportamento
  solto.
- `skin-tight` seguindo retrações do corpo.

---

## 8. Alvo de implementação

Essa regra deve virar uma fonte central de verdade do solver, não apenas um patch
cosmético no final.

Ordem ideal:

1. Construir um perfil radial do corpo ao redor da linha central.
2. Construir um alvo/envelope radial específico para cada preset.
3. Mover o tecido contra esse alvo usando semântica de contato unilateral.
4. Rodar smoothing, weld de costura, controle de stretch e limpeza de colisão.
5. Aplicar um clamp final para impedir que a limpeza recrie vales proibidos.

### Alvo draped

Para cada fatia angular, escanear para baixo e manter o maior raio de suporte
encontrado até ali.

```text
drapedEnvelopeR(z) = maior raio de corpo/suporte encontrado acima de z
```

Se a forma autorada da roupa já estiver mais para fora que esse suporte, ela deve
ser preservada.

### Alvo fitted

Para cada seção relevante, calcular uma ponte entre convexidades de suporte.

Uma implementação possível é criar um envelope externo ou hull sobre o perfil
radial do corpo. O alvo `fitted` deve ignorar vales internos, exceto quando a
roupa estiver explicitamente presa a eles.

```text
fittedEnvelopeR = ponte(convexidades_de_suporte)
```

O solver pode aplicar essa ponte com blend em vez de snap total, mas o alvo ainda
precisa ser uma ponte. Ele não pode ser a superfície côncava do corpo.

### Alvo skin-tight

`skin-tight` continua sendo a superfície do corpo mais a folga autorada.

```text
skinTightR = bodyR + folga_autorada
```

---

## 9. Estado atual do código

Em 2026-07-05, essa regra só existe parcialmente no código.

Partes que já apontam nessa direção:

- `LOOSE_DRAPED` tem lógica de `gravity-shadow` para alguns buracos verticais.
- Existem experimentos de ponte horizontal para roupas loose.
- `panel-flatten` e correções radiais tentam reduzir magnetismo em painéis
  `fitted`.
- `SKIN_TIGHT` já fica próximo do comportamento correto: seguir o corpo.

Lacunas principais:

- Ainda não há um envelope radial formal compartilhado pelos presets.
- `fitted` ainda não tem uma regra central de "ponte até a próxima convexidade".
- Buracos verticais e horizontais são tratados por patches diferentes.
- Alguns passes de smoothing/correção ainda podem preservar ou recriar magnetismo
  em vales do corpo.

Resposta honesta para "essa regra está no código?":

> Ainda não. Existem pedaços dela, mas a regra radial completa precisa ser
> promovida para a arquitetura principal do solver.

---

## 10. Regressões que precisam existir

Essa regra precisa de testes que meçam violações radiais diretamente, não apenas
distorção geral da malha.

Métricas sugeridas:

- **Violação inward em draped:** o raio do tecido diminui em uma fatia de queda
  sem contato, costura ou constraint justificando isso.
- **Valley-follow em fitted:** o tecido fica mais próximo do vale côncavo do
  corpo do que da ponte entre convexidades.
- **Falha de folga uniforme:** `fitted` ou `draped` preserva a mesma clearance no
  vale que preserva nas convexidades.
- **Falha de concavidade horizontal:** o tecido afunda em uma fenda na mesma
  altura, como o cofrinho.
- **Regressão pós-smoothing:** a ponte está correta antes do smoothing, mas o
  vale volta depois da limpeza.

Casos visuais canônicos:

- robe solto sobre o cofrinho
- torso blacksmith `fitted` perto da coxa/bunda
- saia ou robe entre as coxas
- vales de peito/quadril em roupas `fitted`

Esses testes devem passar tanto no corpus padrão quanto nos runs experimentais
com live flags antes de considerarmos a regra implementada.
