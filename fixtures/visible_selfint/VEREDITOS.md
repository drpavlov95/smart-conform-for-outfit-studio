# Vereditos humanos congelados — auto-interseção visível

Poses do `TelvanniBody_female / RM_Heavy` com julgamento do usuário registrado no
momento em que ele olhou no Outfit Studio. Servem para validar qualquer métrica
que se proponha a julgar clipping: **a métrica tem de ordenar estas poses como o
olho dele ordenou**, senão ela não serve de portão.

| arquivo | o que é | veredito dele |
|---|---|---|
| `telvanni_autorada.obj` | malha do artista, corpo magro | referência |
| `telvanni_conform_do_host.obj` | Conform All do OutfitStudio | referência |
| `telvanni_antes_do_conserto.obj` | nosso pipeline sem o limite de painéis | o clipping que ele reclamou |
| `telvanni_aprovada.obj` | com o limite + "só o que o passe causou" | **"melhorou 90% o clipping"** |
| `telvanni_reprovada.obj` | + separação paga pelo painel interno | **"não só não corrigiu como fez o clipping retornar"** |
| `telvanni_corpo.obj` | corpo morfado, para o canal de oclusão | — |

## Ordenação exigida

```
antes_do_conserto  >  reprovada  >  aprovada        (pior para melhor)
```

## Por que isto existe

A contagem total de pares de triângulos que se cruzam **falha** aqui: a pose
reprovada tem `new=138 / lost=101` contra a aprovada, ou seja quase empate em
número, e mesmo assim o usuário viu o clipping voltar. Medindo só o que tem
linha de visão até o exterior, ela é +25% pior no pior ângulo. Foi essa
diferença que me fez entregar uma regressão.

## Como rodar

```
python scripts/check_visible_fixtures.py
```
