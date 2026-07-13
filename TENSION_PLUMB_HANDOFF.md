# Tension Plumb — handoff de implementação

Data: 2026-07-12

## Superseded by the corrected live OBJ diagnosis

The tension-plumb experiment remains preserved but is no longer the leading
fix.  Critical correction: the later tangential diagnosis reversed the Y axis
and repaired the front, not the glute cleft.  The true `-Y` rear still has the
original low-frequency tuck.  See `TRUE_REAR_BLACKSMITH_HANDOFF.md` and the
opt-in `DRAPE_POSTERIOR_CHORD` implementation.  Do not re-enable tension-plumb
or tangential repair as substitutes for that bridge.

## Estado

Implementação C++ preservada, mas **desligada por padrão** após o teste live do
usuário não confirmar a correção visual. Métricas offline não substituem esse gate.

Código:

- solver: `algo/conform_hook.cpp`, `ApplyTensionSeededGraphPlumb`;
- integração/flags: bloco `DRAPE_TENSION_PLUMB`, executado depois do true-final
  body floor;
- laboratório offline e matriz de falsificação: `tools/shell_spike.py`;
- histórico completo: `SHELL_SPIKE_LOG.md`.

## Formulação final

1. Vértices próximos do corpo formam o active set de contato.
2. O suporte é propagado de cima para baixo pelo grafo de arestas da roupa.
3. Aresta comprimida ou folgada não transmite suporte.
4. Aresta esticada transmite no máximo sua extensão fracionária.
5. O peso do caminho é limitado pelo elo mais fraco.
6. A correção é somente radial/outward; nunca magnetiza tecido livre para o corpo.
7. Outer colliders e seam twins são protegidos.
8. Cada vértice tem trust region de 1.25u.
9. O body floor estabelece o baseline antes do passe outward.
10. `outSolvedVerts` e `outColliderVerts` são saídas distintas: a primeira contém
    o visual corrigido; a segunda preserva o baseline para layers downstream.

O passe só aceita shapes cujo preset **original** era `Fitted Cloth`. Um garment
Loose que foi posteriormente mesclado no caminho Fitted não entra.

## Enable experimental e A/B

Harness/terminal:

```powershell
$env:DRAPE_TENSION_PLUMB = '1'
```

OutfitStudio live: para ativar experimentalmente, criar este arquivo vazio:

```text
E:\SkyrimDev\Drape\tension_plumb_on.flag
```

Apagar o arquivo restaura o baseline. `tension_plumb_off.flag` também força OFF.
Não é necessário recompilar para rollback/A-B.

Knobs de A/B e defaults:

```text
DRAPE_TENSION_PLUMB_SEED_GAP=0.50
DRAPE_TENSION_PLUMB_STRENGTH=0.99
DRAPE_TENSION_PLUMB_RETURN=0.0
DRAPE_TENSION_PLUMB_ROUNDS=10
DRAPE_TENSION_PLUMB_MAX_MOVE=1.25
DRAPE_TENSION_PLUMB_COVER=10.0
```

## Resultado Blacksmith RM_Heavy

Comparação C++ real contra o hand-sculpt:

| métrica | baseline | tension-plumb |
|---|---:|---:|
| index mean | 1.5441 | 1.5420 |
| result→sculpt mean | 0.7538 | 0.7306 |
| sculpt→result mean | 0.7868 | 0.7529 |
| worst edge stretch | 4.0651 | 4.0651 |
| garment residual count | 2 | 2 |
| garment face residual count | 10 | 10 |
| worst garment penetration | 0.2861 | 0.2861 |
| worst face penetration | 0.3008 | 0.3008 |

Render principal:

```text
tmp/cpp_tension_plumb/profiles_cpp_postfloor.png
```

Determinismo: duas execuções produziram o mesmo SHA-256:

```text
010ECC9F32A990B80DA256E348F26618932C52CB191801F49A296ACD4D846114
```

## Corpus e gate de release

- Default OFF: todos os 18 shapes batem com o golden baseline.
- A collision proxy separada elimina drift downstream quando o experimento é ligado.
- Apesar das métricas Blacksmith melhorarem offline, o usuário informou que o
  defeito visual permanece. O passe não deve ser promovido sem novo A/B live.

## Build verificada

```text
build/algo/RelWithDebInfo/drape-harness.exe
build/algo/RelWithDebInfo/drape-algo.dll
```

`drape-algo.dll` foi compilada e o post-build a implantou nos dois diretórios de
mod configurados pelo projeto. A feature está OFF; `tension_plumb_on.flag`
habilita somente para novos testes.
