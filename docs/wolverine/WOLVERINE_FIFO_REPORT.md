> **ARQUIVADO (2026-10-06): candidata descartada, sem ganho comprovado. O código foi removido do fonte (backup em `backup-pre-cleanup/`); este relatório fica só como registro.**

# Wolverine — agrupamento FIFO de pedidos de readback

Data: 2026-10-05. Branch local `wolverine-performance`, HEAD `1e2185dba7c6a13662e2fb208131950fa33020de`. Nenhum commit ou push nesta continuação.

## Mudança concreta

Foi implementado `GuestGpu::SendReadbackSync(cache, window, operation)`, usado pelo `BufferCache::ReadMemory`. Pedidos CPU podem compartilhar uma entrada da fila somente quando o último item ainda pendente pertence ao mesmo cache e janela de 512 KiB. Um comando comum ou uma janela diferente interrompe o agrupamento. Limite: 16 membros por entrada.

O mutex que protege a fila também protege a inclusão de membros. Quando a GPU retira uma entrada, o grupo deixa de aceitar pedidos e sua lista fica sob propriedade exclusiva da GPU. Cada operação é executada em ordem e libera seu próprio semáforo imediatamente após retornar. Isso preserva a vida útil dos registros do caller e permite que a primeira CPU prossiga mesmo que o pedido seguinte precise esperar. Não foi criado cancelamento, retorno antecipado, reutilização de versão ou publicação antes da conclusão da GPU.

O mecanismo reduz entradas retiradas da fila, locks de retirada, atualizações do contador de pendências e sinalizações de trabalho. Elimina os wrappers individuais de `SendCommandSync` nesses pedidos, mas acrescenta vetores que podem crescer; a quantidade exata de alocações e bytes alocados ainda não foi medida. Todas as operações de readback, cópias e esperas são mantidas. Isto é agrupamento do gerenciamento da fila, não deduplicação dos callbacks ou dos submits Vulkan.

## Evidência antes de ativar

`readback-fifo-baseline-01` preserva o caminho original de semáforo/wrapper por pedido, apenas com tags e contadores de elegibilidade. Na janela 90,067–149,867 s:

- 94.111 pedidos CPU de readback.
- 35.467 encontraram outro pedido compatível no final ainda pendente da fila (37,7%).
- Nenhum pedido agrupado nesta referência.

O contador `readback_queue_depth` soma entradas pendentes vistas no enqueue; não soma membros e não é máximo de fila. Após o agrupamento, uma entrada pode representar vários pedidos, portanto a diferença de profundidade não deve ser confundida com a mesma redução de solicitações.

## Captura da candidata

Artefatos em `KytyPS5/_Build/windows/benchmarks/readback-fifo-candidate-01/`; relatório `steady.json` entre 90,047 e 149,931 s (59,885 s).

| Medida | Referência sem agrupamento | Candidata |
| --- | ---: | ---: |
| Pedidos CPU de readback | 94.111 | 86.698 |
| Pedidos agrupados | 0 | 20.070 |
| Entradas de readback criadas, derivadas | 94.111 | 66.628 |
| Entradas evitadas na própria candidata | 0 | 23,15% |
| Presents / flips | 485 / 485 | 418 / 418 |
| FPS exploratório | 8,110 | 6,980 |
| Mediana de intervalo | 132,353 ms | 133,707 ms |
| p95 / p99 | 152,035 / 249,374 ms | 166,808 / 268,105 ms |
| Compilações de shader | 96 | 18 |
| Submits do renderer | 664.282 | 596.376 |
| Download de buffers | 22,881 GB | 24,316 GB |
| Despacho agregado dos pedidos | 22,599 s | 20,714 s |

As 20.070 entradas evitadas são uma medida do mecanismo na candidata: `readback_queued - readback_joined` dá a quantidade de entradas de readback criadas, sem incluir comandos comuns. Não usar a diferença de pedidos entre execuções como medida de benefício. O agrupamento foi acionado de fato, ao contrário da segunda checagem pré-envio anterior que evitou somente quatro pedidos no processo inteiro.

**Não houve melhora de FPS demonstrada.** A candidata teve taxa exploratória menor. As execuções passaram por cenas diferentes e não constituem A/B controlado; não é possível concluir a causa da diferença. Não atribuir ganho ao tempo agregado de despacho menor, porque houve menos pedidos e o tempo por pedido é muito próximo (aproximadamente 240 vs 239 microsegundos). Não promover a candidata com base na redução de entradas.

A janela final da referência coincidiu com o início da compilação da candidata. Por isso a comparação acima exclui o trecho após 150 s e usa `steady.json` de 90–150 s nas duas capturas. A captura candidata não teve build concorrente. Ambas duraram 180 s e seus processos foram encerrados pelo harness.

## Validação funcional e estado de uso

Build Release aprovado. CTest 5/5: `performance_diagnostics`, `performance_diagnostics_disabled`, `memory_tracker`, `gpu_command_lane` e `buffer_cache_ranges`. Python 2/2 e `git diff --check` aprovados. O teste da GPU agora cobre:

- Dois pedidos compatíveis compartilham entrada; comando comum e janela diferente mantêm a ordem 1, 2, 3, 4, 5.
- A primeira CPU retorna antes de o segundo pedido esperar por uma ação dela; liberar callers somente no final do grupo falharia nesse teste.
- Dispatch aninhado na própria thread GPU executa inline.
- 22 callers são atendidos em dois grupos, respeitando o limite de 16, sem perder pedidos.
- Os testes existentes continuam cobrindo dirty data, publicação, progresso e shutdown.

A candidata foi preservada no fonte local para revisão e em `_Build/windows/benchmarks/readback-fifo-candidate.exe` (SHA256 `6E4AAE85E8EE28427E5238C5364A87DD8C1204026E74E88FB82624ACB39911DA`). A referência com tags está em `readback-fifo-baseline.exe`. Patches, configuração e fontes exatos estão nas pastas das capturas.

**O executável instalado foi restaurado para a versão anterior sem essa tentativa**, preservada em `pre-fifo-readbacks.exe`, SHA256 `BA74C6D3EB379B2EBE981AEA36502A3DDEC99D8F6018241EC780D638832DFCCC`. O executável na raiz `_Build/windows/kyty_emulator.exe` e o fonte local continuam sendo a candidata; um novo install/copy a recolocaria em uso. Nenhum emulador ficou aberto.

## Continuação

Esta candidata tem efeito estrutural comprovado, mas benefício de frame time pendente e sinal exploratório de taxa menor. Antes de integrar, fixar uma cena/save e repetir A/B incluindo overhead da coleta. Retirar ou ajustar o mecanismo se a regressão se repetir no mesmo cenário. Não aumentar o limite nem pular callbacks para tentar fabricar FPS.

Os submits Vulkan continuam na ordem de mil por apresentação e não foram reduzidos por esse mecanismo. Investigar o custo dominante de CPU/GPU e a quantidade de command buffers sem trabalho real, preservando dependências, ticks, publicação de labels e interrupções. Tags de fila não estabelecem qual produtor tornou cada pedido redundante; correlação com ticks de escrita e callbacks ainda é necessária antes de reutilizar resultados ou cancelar pedidos.
