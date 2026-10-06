# Wolverine — flush de label limitado à fatia PM4 (`KYTY_LABEL_FLUSH_EVERY`)

> **CORREÇÃO (posterior): a retirada abaixo estava errada.** O "fundo preto" não é regressão do flush de labels. Uma referência **quente, sem nenhuma flag e sem X** (`benchmarks/warm-ref-forest`, frame 3962, 240 s) também mostra a caixa SCREEN READER sobre fundo preto. A floresta aparece só em execuções com cache de shaders do driver **frio** (a primeira de cada nome de executável), porque o stall de ~58 s em compilação de pipelines dá tempo ao jogo de carregá-la. Os casos de "fundo preto" (`KEEP`, `DEFER_MODE`, async) eram todos execuções quentes. Conclusão: a regressão visual **não está provada**, e o ganho medido quente contra quente (12,7 → 15,4 FPS) volta a ser candidato; falta confirmá-lo com A/B limpo (`warm-*`) e checagem de progresso do jogo além do menu.

> **RESULTADO RETIRADO (regressão visual) — ver correção acima.** O usuário apontou, e os screenshots confirmam: com `KYTY_LABEL_FLUSH_EVERY=0` a caixa "SCREEN READER" aparece sobre **fundo preto**. Na referência ela aparece sobre a cena 3D da floresta (`waitsites-heli-ref-01/screenshots/t0141s-p001663.png` e as screenshots da calibração). O jogo deixa de renderizar o fundo, portanto a carga **não é a mesma** e os +21,5% abaixo **não** são ganho do emulador. Não usar `=0`. A tabela é mantida apenas como registro do que foi medido.

Data: 2026-10-05. Branch `wolverine-performance`, HEAD `1e2185db`, sem commit/push.

## Origem

A instrumentação de waits/submits por call site ([WOLVERINE_WAITS_PLAN.md](WOLVERINE_WAITS_PLAN.md)) mostrou que, com `KYTY_LABELS_AFTER_GPU=1`, **83% dos submits do renderer** (~1.074 de 1.287 por frame no menu) vinham do `Flush()` feito após cada RELEASE_MEM com label. Isso custa ~16 ms/frame de `vkQueueSubmit` na thread GPU, que é o gargalo (idle 0).

## Mudança

- `CommandProcessor::FlushAfterLabel()` em `graphicsRun.cpp`, com contador em `commandProcessor.h`.
- `KYTY_LABEL_FLUSH_EVERY=N`:
  - N > 1: submete a cada N labels;
  - 0: deixa o submit para o fim da fatia PM4;
  - ausente ou 1: comportamento anterior, que continua sendo o **padrão**.
- Progresso preservado: toda fatia de `GuestGpu::Process` que progrediu termina em `BufferFlush()`, inclusive quando suspende em WAIT_REG_MEM. O label adiado é submetido sem depender de um comando futuro (o risco anotado sobre a tentativa anterior de "agrupar todos os labels").
- Labels continuam publicados só após a conclusão do tick na GPU.

## A/B — tela de acessibilidade "SCREEN READER"

A tela é alcançada sem input, estável e sem compilação de shader na janela. Mesmo binário `benchmarks/labelflush-ab.exe` (SHA256 `5DDCC3E4584D21B6111B93B64C90B4928C63DA7202848D6801E5D617A8086592`); só a env muda. Janela = últimos 40 s de cada captura (`compare_ab.py tail 40`). A seleção por presentation id não serve entre execuções com velocidades diferentes, porque o carregamento apresenta frames conforme o tempo real.

| Execução | FPS | Mediana | p95 | p99 | Submits/frame | queue.submit ms/frame | Compilações |
|---|---:|---:|---:|---:|---:|---:|---:|
| A1 (padrão) | 12,65 | 82,50 | 84,43 | 101,06 | 1.286 | 15,73 | 3 |
| A2 (padrão) | 12,77 | 82,37 | 84,26 | 101,04 | 1.287 | 15,51 | 0 |
| **B2 (`=0`)** | **15,49** | **66,58** | **67,92** | **83,69** | 215 | 3,47 | 0 |
| **B3 (`=0`)** | **15,38** | **66,60** | **72,88** | **84,61** | 213 | 3,45 | 0 |

Resultado: **+21,5% FPS (12,71 → 15,44) e −19,2% na mediana (82,4 → 66,6 ms)** no mesmo cenário, com variação entre repetições de ~1% de cada lado. A thread GPU caiu de ~54,6 para ~38,3 ms por frame de trabalho (sem contar waits). O B3 tem screenshots confirmando o mesmo menu e imagem correta. B2/B3 também chegaram ao menu muito antes: frame 2293 aos 61 s, contra ~1800 aos 140 s na referência.

O B1 foi excluído: o usuário clicou na janela e o jogo saiu do menu para a cutscene de abertura. Mesmo assim ele mostra que a candidata roda na cutscene sem travar. Artefatos: `benchmarks/labelflush-menu-{A1,A2,B1,B2,B3}/`, `ab-tail40.json` em cada pasta.

## Limites e próximos passos

- Validado só nesta tela. Falta medir a cutscene do helicóptero, onde o flush por label é ~3% do frame e há ~450 ms/frame de trabalho sem atribuição, e regressão visual/progresso no gameplay e em outros jogos.
- Timestamps EOP (`clock_value`) são lidos na conclusão do tick. Com submit mais tardio, os valores ficam mais tarde. Observar se afeta resolução dinâmica ou pacing.
- Ainda não é padrão. Promover a padrão após (1) a cutscene do helicóptero sem regressão, (2) uma checagem de gameplay e (3) os testes. O valor intermediário N (ex. 16) fica disponível se `=0` aumentar a latência de algum consumidor de labels na CPU.

## Segunda tentativa: classificar labels por consumidor (opção 2) — também falhou

Implementado (opt-in, padrão inalterado): cada escrita de label é classificada como interrupção, endereço já esperado pela GPU (WAIT_REG_MEM / COND_INDIRECT, via `ReadLabel`), timestamp ou "outro"; `KYTY_LABEL_FLUSH_KEEP=<máscara>` mantém o submit imediato só para as classes escolhidas. Binário `benchmarks/labelclass.exe`, SHA256 `E720701BC06FE30BE9FD20DFA197DA63BE80CDE58A1F0BFFC118C73DCDE99FF2`.

**Contagem no menu, por frame:** interrupção 0, GPU-waited ~70, timestamp ~35, **outro ~970** (de ~1.075 labels). A CPU do jogo pode fazer polling de memória e isso não é observável, portanto "outro" significa "sem consumidor visível ao emulador".

| Variante | Resultado visual |
|---|---|
| Referência (padrão) | cena 3D do Wolverine atrás da caixa SCREEN READER |
| `KEEP=7` (adia só "outro") | **tela totalmente preta** e travada por >30 s |
| `KEEP=1` (mantém só interrupção) | caixa sobre fundo preto |
| `KEEP=0`, `DEFER_MODE=1` (barreira global + fim do render pass nos labels adiados) | fundo preto |
| `KEEP=0`, `DEFER_MODE=2` (só fim do render pass) | fundo preto |

Conclusões comprovadas: adiar **qualquer** classe, mesmo só os ~970 labels sem consumidor visível, quebra o render. E a causa **não** é o submit funcionar como barreira ou fim de render pass: substituí-lo por ambos não restaurou a imagem. Ficam duas hipóteses não testadas: (a) a CPU do jogo consome esses labels por polling (fences de recursos por draw/dispatch) e o atraso muda o fluxo; (b) o início tardio da GPU muda alguma dependência CPU↔GPU. Os FPS dessas execuções (15,0–15,4) **não valem**, porque o trabalho renderizado não é o mesmo.

Direções: o custo de ~16 ms/frame é de `vkQueueSubmit` (15 µs por submit), não do número de labels em si. Uma alternativa que **preserva todos os pontos de submit e a ordem** é mover o `vkQueueSubmit` para uma thread própria (FIFO por tick). A thread GPU, que é o gargalo (idle 0), deixaria de pagar esse custo sem mudar quando cada command buffer chega à GPU. É uma mudança de arquitetura com risco (tratamento de erro/device lost, ordem com o presenter, lifetime), e o teto de ganho é ~16 ms de 82 no menu e só ~3% na cutscene do helicóptero.

## Resultado final controlado (substitui a tabela e a retirada acima)

Todas as execuções com o mesmo `async.exe` quente, mesmo menu **INITIAL SETUP**, mesma imagem, últimos 40 s, 2 repetições por variante (`benchmarks/warm-*`):

| Variante | FPS | Mediana | Submits/frame |
|---|---|---:|---:|
| Referência (padrão) | 12,36 / 12,29 → **12,33** | 83,0 ms | 1.290 |
| `KYTY_LABEL_FLUSH_EVERY=0` | 15,01 / 15,18 → **15,09 (+22,4%)** | 66,6 ms | ~214 |
| `KYTY_ASYNC_SUBMIT=1` | 13,85 / 13,81 → **13,83 (+12,2%)** | 66,8 ms | 1.290 |

Verificação funcional com `KYTY_LABEL_FLUSH_EVERY=0` (`warm-flush-story`, 330 s, dois X automáticos): o jogo passa da caixa de acessibilidade para a **tela de título com a floresta 3D renderizada** (idêntica à da referência), sem travar nem perder frames até o fim.

A tentativa anterior de "classificar labels por consumidor" (`KEEP`, `DEFER_MODE`) e a conclusão de regressão visual foram baseadas em execuções com cache de shaders quente, nas quais **a própria referência** também mostra fundo preto. Essas execuções não provam regressão; também não provam que a classificação era necessária.

**Não provado ainda:** cutscene do helicóptero (lá o submit por label pesa só ~3% do frame), gameplay, outros jogos, e se algum consumidor de timestamps/labels na CPU muda de comportamento mais adiante. Por isso as duas flags continuam desligadas por padrão. Para testar no jogo: definir `KYTY_LABEL_FLUSH_EVERY=0` (e opcionalmente `KYTY_ASYNC_SUBMIT=1`) no ambiente antes de abrir o emulador.
