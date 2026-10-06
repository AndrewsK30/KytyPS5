# Plano de desempenho — KytyPS5 / Wolverine

Data: 2026-10-05. Repositório local: `G:\kity_work\KytyPS5`.

## Objetivo e escopo

Reduzir o tempo necessário para produzir um frame correto de Wolverine e aproveitar as correções nos demais jogos. Começar por diagnóstico confiável e alterações no código do emulador. FPS maior com draws ausentes, imagem incorreta, labels publicados cedo ou reprodução acelerada não é melhoria válida.

Referência atual: PR 937 em `41c7b27b738ce882c93108e8b882405e33a78d8d`. A branch local `wolverine-performance` também contém `1e2185db`, adaptação experimental da PR 900. Esse commit não demonstrou ganho no Wolverine e não foi enviado ao fork. Não o incorporar à referência de desempenho sem uma comparação isolada. Preservar as duas revisões para comparação em builds/worktrees separados.

Metas de engenharia: obter uma primeira redução repetível de pelo menos 10% no tempo de frame; depois reavaliar a distância para 15 FPS (66,7 ms), 30 FPS (33,3 ms) e 60 FPS (16,7 ms). Esses valores são marcos, não previsões. A mediana medida de 166,7 ms exige cerca de cinco vezes menos tempo para chegar a 30 FPS.

## Evidências e limites do diagnóstico atual

| Evidência | Interpretação permitida |
| --- | --- |
| Captura `live-gameplay-presentmon.csv`: 117 frames, média de 5,92 FPS, mediana de 166,72 ms | Confirma a lentidão do trecho capturado. Repetir com cenário e duração documentados. |
| Medianas de GPU time ~166 ms e GPU busy ~67 ms nessa captura | Motiva investigar bolhas, waits e alimentação da fila. Não prova sozinho onde os ~100 ms restantes são gastos; medianas de métricas distintas não se somam por frame. |
| CSV do PresentMon identifica runtime DXGI, embora o renderer seja Vulkan | Validar correlação com flips do guest e timestamps Vulkan antes de usá-lo como medida exclusiva. |
| Tracy: ~536 mil chamadas de `CpOpReleaseMem`, mas só três frames reportados e carregamento de módulos na mesma janela | Frequência útil como pista. Percentuais e taxas não representam um perfil confiável de gameplay. |
| Configuração `Performance` tem getter, mas nenhum consumidor no caminho atual de shaders | A flag não aplica otimização SPIR-V nesta revisão. Corrigir a implementação é um candidato concreto, com validação por shader. |
| Queries de oclusão retornam resultados sintéticos sempre visíveis | Pode causar renderização extra. Medir uso de queries e predicação antes de estimar o benefício. |
| Compilação gráfica já possui worker threads, com espera inicial de 20 ms em `TryGetGraphicsPipeline` | Medir miss, variantes e espera. Não duplicar um sistema assíncrono que já existe. |
| Testes de flags e batching amplo não melhoraram ou perderam progressão | Não tomar essas tentativas como correções prontas. |

Uma amostra de frequência/temperatura saudável da RTX 4090 não exclui todos os limites de hardware. Registrar também VRAM, RAM disponível, paginação e clocks durante toda a medição.

## Etapa 1 — referência reproduzível e instrumentação leve

Entregáveis: harness de benchmark, manifesto da execução, CSV por frame e relatório de comparação. Esta é a primeira implementação autorizável pelo plano; as otimizações seguintes dependem do resultado.

1. Confirmar flags de compilação efetivas do executável (o Ninja atual contém `/O2`), SHA, submódulos, driver, configuração e hash do binário. Usar builds separadas para referência e candidato. Resolver o install prefix local; atualmente aponta para `C:/Program Files (x86)/Kyty` e prejudica a instalação reproduzível.
2. Fixar um trecho de Wolverine. Se não houver save utilizável, selecionar um intervalo identificável da cinematic inicial por evento/marcador e estado visual. Não comparar períodos arbitrários de inicialização.
3. Separar carga fria, execução após aquecimento de cache e gameplay estável. Medir pelo menos três execuções de 60 segundos por candidato no mesmo trecho; se não houver intervalo estável, declarar a compilação como parte do custo.
4. Correlacionar IDs de flip do guest, submissões, ticks de completion e apresentação. Contar frames produzidos e apresentações/repetições separadamente.
5. Adicionar contadores locais por thread e agregação fora do caminho crítico. Exportar uma linha por frame ou lote, sem printf por pacote e sem atomics globais por draw. Registrar o overhead da própria instrumentação; meta inferior a 2%, ou reduzir a coleta.
6. Acrescentar timestamps Vulkan amostrados, recuperar resultados somente após completion e sem esperar a GPU para coletá-los. Usar os períodos suportados pelo dispositivo e controlar o reaproveitamento dos query pools.
7. Capturar timeline curta de CPU/GPU apenas no trecho selecionado. Usar Tracy com zonas largas ou amostragem, complementado por profiler de CPU quando necessário. Evitar repetir a captura de milhões de zonas por pacote.

| Ponto do código | Dados a adicionar |
| --- | --- |
| `src/graphics/guest_gpu/command_processor/pm4Handlers.cpp` — `CpOpReleaseMem`, WAIT_REG_MEM, predicação | Contagens por combinação de evento/data_sel/GCR/interrupt; origem dos flushes; waits satisfeitos e suspensos. |
| `src/graphics/guest_gpu/graphicsRun.cpp` — processamento, labels, waits, flips | IDs do frame/submission; tempo executando vs esperando; tick que produz o label e momento de publicação. |
| `src/graphics/host_gpu/renderer/commandScheduler.cpp` — Submit, Wait, Finish e callbacks | Submits/frame, comandos por submit, motivo, tempo de queue mutex/API/wait, fila pendente e atraso de callback. |
| `src/graphics/host_gpu/renderer/cache/bufferCache.cpp`, `textureCache.cpp`, `streamBuffer.cpp` | Bytes e intervalos de upload/readback; causa da transferência; waits de fault/ring wrap; reutilização e pressão de memória. |
| `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp` | Hits/misses, variantes por hash, tempo de tradução/SPIR-V/driver, fila e espera de compilação. |
| `src/graphics/host_gpu/renderer/pipeline/descriptors.cpp`, `shaderResourceBarrier.cpp`, `renderDraw.cpp` | Rebinds, lookups e barreiras por draw/dispatch; tempo CPU e GPU amostrado por hash/estágio. |
| `src/graphics/presentation/window/swapchain.cpp`, `window.cpp` | Espera de acquire/present, renderer mutex e atualização SDL do título; correlação com flips. |

Saída mínima: média de FPS calculada por frames/duração, mediana/p95/p99 de frame time, variação entre execuções, submits/frame, waits/frame, bytes transferidos/frame, tempo de compilação e tempo GPU por grupo de trabalho. Não somar tempos de threads que trabalham em paralelo.

## Etapa 2 — escolher a primeira otimização pelo caminho crítico

A ordem abaixo é uma fila de investigação. A prioridade de implementação deve acompanhar a timeline da etapa 1.

### A. Submissões, completion e sincronização

Hipótese: muitas submissões pequenas ou waits interrompem a alimentação da GPU. Arquivos: `pm4Handlers.cpp`, `graphicsRun.cpp`, `commandScheduler.cpp`, `sync.cpp` e implementação de semáforos do renderer.

- Rastrear os produtores e consumidores dos labels, incluindo espera do guest na CPU, WAIT_REG_MEM na GPU, filas de compute e interrupções. A ausência de um consumidor no command buffer atual não prova que o label pode ser adiado.
- Identificar submits duplicados: `WriteAtEndOfPipe` já pode fazer Flush com `KYTY_LABELS_AFTER_GPU`, e alguns caminhos de `CpOpReleaseMem` também fazem Flush em seguida. Primeiro medir se o segundo buffer está realmente vazio; considerar eliminar apenas trabalho redundante com observabilidade de ticks/callbacks preservada.
- Preservar publicação depois da execução GPU, ordem de interrupções e progresso independente. Agrupar somente com garantia de envio no próximo limite ou mecanismo que assegure progresso; nunca exigir um próximo comando que depende do próprio label adiado.
- Se houver benefício demonstrado, implementar regras de envio no scheduler com motivos explícitos e limites de lote. O interrupt-only batching da PR 900 é um experimento a avaliar, não uma política geral comprovada.
- Substituir barreiras globais por dependências mais específicas apenas quando os recursos/acessos forem conhecidos; manter fallback conservador para BDA/bindless ou aliases desconhecidos.

Validação: testes de label seguido de espera CPU/GPU, interrupção sem comandos subsequentes, compute intercalado com graphics, visibilidade de memória, flush repetido e shutdown. Aprovação depende de menor tempo de frame, não apenas menos submits.

### B. Coerência de memória, readbacks e caches

Hipótese: fault handlers, readbacks de imagens lineares e transferências de intervalos maiores que os realmente usados drenam a fila ou consomem CPU.

- Medir bytes úteis vs bytes copiados, frequência dos faults, produtores dos recursos e ticks necessários por leitura.
- Reduzir intervalos e transferir somente regiões sujas; reutilizar conversões quando o conteúdo não mudou.
- Adiar readback até um consumidor real somente quando a coerência entre memória guest, buffers, imagens e aliases estiver garantida. Evitar waits de fila inteira quando um tick específico resolve a dependência.
- Medir coleta de lixo, cache churn, RAM/VRAM e buffers temporários. Manter recursos referenciados por comandos em voo vivos até completion.

Validação: testes existentes de memory tracker, image page table, resource tracking/materialization; adicionar casos para escrita GPU seguida de leitura CPU, aliases imagem/buffer, reupload CPU e aposentadoria de recursos. Correlacionar redução de bytes/waits com frame time.

### C. Código de shader e trabalho efetivo da GPU

Hipótese: a tradução produz shaders caros ou o renderer envia trabalho que poderia ser descartado corretamente.

- Ordenar hashes e estágios por tempo GPU acumulado e por chamada; estudar os maiores consumidores, registrando tamanho do dispatch, wave size e variantes.
- Implementar aplicação real da configuração SPIR-V no caminho entre emissão e criação do módulo. Avaliar conjunto de passes e custo de compilação; validar SPIR-V e saída GPU antes de habilitar por padrão. Incluir tesselação no mapeamento dos caminhos de compilação.
- Trabalhar em simplificações de IR apenas após identificar instruções/conversões/acessos redundantes nos shaders dominantes. Preservar máscaras de execução, precisão exigida, atomics e side effects.
- Investigar queries de oclusão e predicação: mapear semântica guest, resultado por DB, readiness e ordem de publicação. Implementar queries Vulkan e consumo correto apenas se o workload provar benefício. Nunca devolver zero artificialmente para ganhar FPS.

Validação: `shader_recompiler_compute_tests`, `resource_tracking_tests`, `shader_vertex_metadata_tests`, `scalar_provenance_tests` e casos novos de equivalência para a transformação escolhida. Comparar imagens, iluminação, partículas, geometria, compute e progresso do jogo.

### D. Compilação e persistência de cache

Hipótese: recompilações e criação de pipelines explicam engasgos ou parte do custo sustentado.

- Separar tradução, especialização, SPIR-V, módulo Vulkan e pipeline do driver. Medir variantes que diferem sem necessidade e compilação repetida por hash.
- Adicionar cache persistente de planos/SPIR-V com chave baseada em conteúdo, especialização, opções, capacidades do dispositivo e versão do formato/recompiler. Não reaproveitar módulos Vulkan entre processos.
- Preservar a arquitetura de workers já existente e investigar por que a thread de GPU espera inicialmente até 20 ms. Preaquecer variantes conhecidas; não omitir draws críticos enquanto aguarda compilação para aparentar FPS maior.
- Versionar o cache do driver pelo formato e compatibilidade apropriados, registrando invalidações. Medir execução fria e quente separadamente.

Validação: equivalência de cold/warm, invalidação por mudança de código/opções/capacidades, arquivo corrompido e cache incompleto. Benefício em startup deve ser reportado separadamente do FPS estável.

### E. CPU e apresentação

- Se CPU dominar, priorizar rebind de descritores, leituras SRT/BDA, lookups e alocações que apareçam na timeline. Cachear estado apenas com invalidação por geração/conteúdo correta.
- `WindowContext::UpdateTitle` chama `SDL_RunOnMainThread(..., true)` a cada apresentação. Medir o bloqueio; se significativo, reduzir frequência e despachar atualização com ownership/lifetime seguros.
- Medir contenção entre presenter e renderer. Reduzir duração do lock somente preservando acesso a frames e recursos. Avaliar starvation da thread GPU antes de alterar prioridades de threads.

Validação: apresentação contínua, UI responsiva, shutdown e lifetime dos dados enviados à main thread. Ganho deve aparecer na timeline, e não somente na leitura do título da janela.

## Etapa 3 — regressão em outros jogos e aceitação

- Wolverine: cinematic inicial, trecho com geometria/compute pesado e cena noturna, conforme alcançáveis sem crashes. Gravar referência visual e marcadores do trecho.
- Pelo menos dois outros jogos que o usuário tenha disponíveis: um com muitas submissões/compute e outro com renderização convencional. Disponibilidade ainda precisa ser levantada; não afirmar ganho geral antes de testá-los.
- Para cada alteração: um commit com problema, mecanismo, teste funcional e tabela antes/depois. Usar a mesma configuração e trecho entre A/B; variar uma mudança de código por comparação.
- Aceitar ganhos acima do ruído entre execuções. Meta inicial de 10% no frame time; melhorias menores podem valer se o efeito for repetível e sem piora de cauda ou correção.
- Investigar regressões acima de 5% e qualquer alteração visual, travamento, áudio fora de sincronia ou resultado de memória divergente. Retirar o candidato da branch de integração se não passar.
- Consolidar no fork depois de medir. Conservar os artefatos de benchmark localmente e publicar apenas código/relatórios sem conteúdo do jogo.

## Sequência de entregas

1. `perf: add frame-correlated renderer diagnostics` — instrumentação, harness e referência limpa.
2. Relatório de caminho crítico — decidir entre submissões, memória, shader, compilação ou CPU/apresentação, com evidências por frame.
3. Primeira correção limitada ao maior consumidor — incluir testes de dependência/coerência necessários.
4. Comparação Wolverine antes/depois em três execuções — resultado visual e percentis.
5. Segunda correção, escolhida a partir do novo perfil — não assumir que o gargalo anterior continuará dominante.
6. Matriz de regressão dos jogos disponíveis e integração no fork.

Atualização de 2026-10-05: a instrumentação, harness e capturas exploratórias estão implementados e validados, ainda sem commit. A classificação inicial de consumidores está em [WOLVERINE_READBACK_REPORT.md](WOLVERINE_READBACK_REPORT.md): pedidos CPU dominam os readbacks de buffers; GC/metadados não explicam esse volume; waits de completion precisam ser separados dos waits da thread GPU. A tentativa de reduzir a janela pré-escrita para 4 KiB foi testada, rejeitada por regressão e revertida.

Não há melhoria de FPS comprovada. O próximo passo é aprofundar a entrega 2 com intervalos/ticks e recursos recorrentes, além de cenário repetível e avaliação do overhead. A próxima correção deve reduzir sincronizações preservando o agrupamento de acessos, com três comparações A/B antes de integração.

Continuação: [WOLVERINE_SYNC_REPORT.md](WOLVERINE_SYNC_REPORT.md) registra fases e amostras implementadas, 59,9% de amostras CPU pre-write sem cópia e tentativa de segunda checagem pré-envio rejeitada (quatro envios evitados no processo inteiro). O alvo agora é a relação entre pedidos já pendentes e downloads que os tornam redundantes, preservando ordem dos comandos e vida útil dos callbacks. A taxa exploratória variou sem benefício mensurável do mecanismo; não tratar como ganho de FPS.

Última continuação: [WOLVERINE_FIFO_REPORT.md](WOLVERINE_FIFO_REPORT.md). Agrupamento FIFO reduziu entradas de readback em 23,15% na candidata e passou nos testes, mas manteve as cópias/waits/submits e não comprovou benefício de frame time. Fonte preservado como candidata; executável instalado restaurado. Investigar o custo dominante por frame e submits sem trabalho antes de promover outra otimização de estruturas auxiliares.
