# Plano — waits por call site, churn de buffers e readbacks "GPU atrasada" (Kyty / Wolverine)

## Contexto

O trabalho anterior com o Codex (ver `G:\kity_work\WOLVERINE_SESSION.md` e os relatórios `WOLVERINE_*_REPORT.md`) construiu instrumentação nativa (`performanceDiagnostics.{h,cpp}`, `KYTY_PERF_CSV`), harness (`tools/performance/capture.ps1`, `analyze.py`, `readbacks.py`) e um benchmark 3D. Também testou quatro candidatos: readback de 4 KiB, rechecagem antes do envio, agrupamento FIFO e CSE. **Nenhum teve ganho de FPS comprovado.** Os dois problemas de base continuam:
- não existe cena repetível; as janelas por tempo de boot passam por cenas diferentes;
- os waits aparecem agregados por thread (`Gpu`/`Completion`/`Other`) e não por ponto de chamada.

Um dev que trabalha em UFC 5/6 sugeriu três coisas: (1) rotular cada wait pelo call site; (2) tratar a GPU como estando sempre alguns ms atrás, porque ler dados frescos dela custa essa latência e a solução é não precisar do dado na CPU, ou usá-lo um frame atrasado; (3) cortar churn de buffers arredondando para 1 MiB. Com isso ele chegou a ~30 FPS na cutscene do helicóptero.

A exploração do código confirma que as três se aplicam ao Kyty:
- **Waits:** `MasterSemaphore::Wait` (`masterSemaphore.cpp:41`) só registra o papel da thread. Os caminhos rápidos (tick já atingido) não são contados, então não dá para calcular a razão instantâneo/bloqueante. Nenhuma função de wait tem zona Tracy. A espera de 100 ms da thread GPU em labels (`graphicsRun.cpp:614`, `m_work_available.WaitFor`) **não é medida**. Também não têm timer: wrap dos anéis (`streamBuffer.cpp:325`), GC de buffers sujos (`bufferCache.cpp:878`), fault buffer (`faultManager.cpp:111`) e `Finish()` via GDS (`graphicsRun.cpp:1670`).
- **GPU atrasada:** `ReadMemory` → `SendReadbackSync` → `DownloadBufferMemory` → `CommandScheduler::Wait(CurrentTick)` (`bufferCache.cpp:417`, `commandScheduler.cpp:196-209`) faz um *self-submit drain*: submete o command buffer parcial e espera a GPU inteira. Essa é a hipótese principal para os ~1.400 submits por apresentação e para os 8,4 GB de leitura CPU mais 7,7 GB de pré-escrita CPU.
- **Churn:** `CreateBuffer` (`bufferCache.cpp:517-548`) alinha só a 16 KiB (`CACHING_PAGESIZE`, `bufferCache.h:31`). Todo buffer guest tem BDA, e o construtor de `Buffer` (`streamBuffer.cpp:58-105`) usa `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`, então **cada buffer é um `vkAllocateMemory`**. Cada join copia o buffer antigo inteiro com duas barreiras globais (`streamBuffer.cpp:158-195`) e reescreve a page table BDA. O tiler aloca e destrói um VMA buffer por operação (`tiler.cpp:91-113`). Hoje não existe contador de criação, join ou alocação.

Decisões do usuário: o cenário fixo é a **cutscene do helicóptero**; implementar **os três pontos em ordem**, com A/B antes de passar ao seguinte.

Regras que vêm dos documentos e continuam valendo: uma hipótese por A/B; 3 execuções de ≥60 s por lado na mesma cena; não publicar labels antes da GPU; não pular draws; preservar as alterações sem commit e trabalhar só no fork `AndrewsK30/KytyPS5`. A candidata FIFO+CSE está no fonte; o `install/` estável é pré-FIFO (SHA `BA74C6D3…`).

---

## Fase 0 — Cena repetível: cutscene do helicóptero

1. Limpar o fonte para a referência. Gerar um binário de referência **sem** FIFO e sem CSE, usando o `pre-fifo-readbacks.exe` estável ou um build com as duas desligadas, para que os A/B futuros meçam uma mudança por vez. Guardar FIFO e CSE num stash ou patch separado (os patches já estão nas pastas de captura).
2. Fazer uma execução para localizar a cutscene. Adicionar a `capture.ps1` um `-ScreenshotEverySeconds` (screenshot da janela a cada N s, só nessa execução de calibração) e ao CSV o índice cumulativo de flip. Assim se define o início e o fim da cutscene **pelo número do flip guest**, não pelo tempo de boot.
3. Adicionar a `analyze.py` a opção `--start-flip/--end-flip` (seleção por flip em vez de segundos) e reportar frame time por flip. Nas execuções A/B, salvar um screenshot no flip inicial e outro no final para confirmar que a cena é a mesma.
4. Estado de cache igual em todas as execuções. Hoje o cache do driver é desativado por "dirty build". Escolher entre cache frio sempre (cutscene na primeira passada) ou aquecida (duas passadas, medir a segunda) e registrar a escolha no manifesto.
5. Medir o overhead da coleta: 3 execuções com `-DisableDiagnostics` e 3 sem, na cutscene. Esta é a primeira pendência antiga que fica resolvida.

**Saída:** baseline da cutscene (mediana, p95 e p99 do frame time, FPS e variação entre execuções) e overhead da instrumentação.

## Fase 1 — Rotular cada wait por call site e separar sync de trabalho

Reutilizar o padrão que já existe: `ReadbackScope` / `ThreadRole` é uma tag thread-local lida dentro de `MasterSemaphore::Wait` (`performanceDiagnostics.cpp:195-201`).

1. **Novo `enum class WaitSite` + `WaitSiteScope` RAII** em `performanceDiagnostics.h`. Sites iniciais:
   - `ReadbackCpuRead`, `ReadbackCpuWrite`, `ReadbackGpu`, `Metadata` (derivados de `ReadbackSource`)
   - `SelfSubmitDrain` (`CommandScheduler::Wait` com `tick == CurrentTick`)
   - `FinishGdsSync` (`graphicsRun.cpp:1670`), `FinishUnmap` (`renderContext.cpp:114`), `FinishComputeRecovery` (`renderCompute.cpp:101`), `FinishCollectFaults` (`faultManager.cpp:104`)
   - `GcDirtyDownload` (`bufferCache.cpp:878`), `FaultBuffer` (`faultManager.cpp:111`)
   - `RingWrapDownload`, `RingWrapStream`, `RingWrapStaging`, `RingWrapDevice` (`streamBuffer.cpp:325`, tag pelo tipo do anel)
   - `TextureDownload` (`textureCache.cpp:1837`), `SwapchainFramePool` (`swapchain.cpp:118`), `PriorityOps` (thread de completion, `commandScheduler.cpp:287`), `Unknown`
2. **`MasterSemaphore::Wait`:** contar **todas** as chamadas por site, inclusive as que saem pelo caminho rápido (`wait_calls_<site>`, `wait_blocking_<site>`, `wait_ns_<site>`). Na espera real, registrar também o atraso da GPU: `CurrentTick - gpu_tick` no início (soma e máximo por site). Isso mostra diretamente "quantos submits atrás" a GPU estava.
3. **Waits fora do semáforo**, com o mesmo esquema de site:
   - `WaitPriorityOperations` (cv)
   - `SendReadbackSync` / `SendCommandSync` (`done.acquire()`, `graphicsRun.cpp:210/243`)
   - **thread GPU bloqueada em label/WAIT_REG_MEM** (`graphicsRun.cpp:614`), separada do idle normal (`:592`)
   - `videoOut` (`videoOut.cpp:1185/1198/1781`)
   - `PipelineWait`, que já existe
4. **Submits por motivo:** contador `submit_reason_<x>` em `CommandScheduler::Flush/Submit`, com motivos self-drain, label flush, RELEASE_MEM, present, GC, Finish e ring wrap. Isso testa se o self-submit drain explica os ~1.400 submits por frame.
5. **Tracy:** um `KYTY_PROFILER_BLOCK("Wait:<site>")` (macros em `common/profiler.h:45-58`) dentro de cada scope de site, só nos pontos de wait (baixa frequência), para ver a timeline na cutscene sem os milhões de zonas por pacote.
6. **Orçamento da thread GPU por frame** em `analyze.py`: executando PM4/draw versus bloqueada (semáforo por site, label, readback inline) versus idle. Relatório novo `waits.py` (ou seção no `analyze.py`) com uma tabela por site: chamadas, % instantâneas, ms/frame bloqueado, atraso médio em ticks. Ordenar por ms/frame.
7. **Testes:** estender `tests/PerformanceDiagnosticsTests.cpp` (tag aninhada e restaurada, contagem instantânea vs bloqueante, concorrência). Manter a ordem `Metric` ↔ `Names` (o array não detecta nome faltando; adicionar `static_assert` ou checagem no teste).

**Gate:** capturar 3× a cutscene com a referência e produzir a tabela "top sites por ms/frame". O que a Fase 3 vai atacar depende dela. Hipótese a confirmar ou refutar: poucos sites de readback/self-drain concentram quase todo o tempo bloqueado.

## Fase 2 — Churn de buffers (granularidade de 1 MiB)

1. **Contadores primeiro**, porque hoje não existem:
   - `buffer_create` + bytes (`bufferCache.cpp:535`); `buffer_join` + bytes copiados (`:542-545`); `buffer_delete` (`:106`)
   - `vma_buffer_create` + ns e `dedicated_alloc` no construtor de `Buffer` (`streamBuffer.cpp:82`)
   - `bda_pagetable_upload` (`ChangeRegister`, `:59-98`)
   - `image_create` (`textureCache.cpp:227`), `tiler_scratch_alloc` (`tiler.cpp:91`)

   Medir na cutscene de referência.
2. **Candidata A, granularidade:** em `CreateBuffer`, `AlignDown/AlignUp(vaddr, KYTY_BUFFER_GRANULARITY)` com padrão de 1 MiB, mantendo `CACHING_PAGESIZE` como granularidade da page table. **Risco a verificar antes:** o arredondamento não pode estender o buffer para fora do mapeamento guest. Clamp à região mapeada que contém o endereço e ao espaço de endereços, como `ResolveOverlaps` já faz em `:487-494`. Também verificar que upload e tracking cobrem só páginas mapeadas (o `StreamLeap` de 2 MiB já padeia, então é um bom ponto de referência). Gate por env para o A/B; depois virar padrão se ganhar.
3. **Candidata B, separada:** o scratch do tiler vira anel ou pool reutilizável em vez de `vmaCreateBuffer` por operação (`tiler.cpp:91-113`), respeitando `DeferDestroy` por tick.
4. **Testes:** `buffer_cache_ranges` com novos casos (buffer no meio de um mapeamento; perto da borda de um mapeamento menor que 1 MiB; join de buffers arredondados; GC e coerência inalterados). Rodar `renderer_benchmark_smoke` e confirmar que o checksum não muda.
5. **A/B:** 3×3 na cutscene, primeiro A, depois B, cada um contra a referência. Reportar criações/joins/allocs por frame, frame time mediano/p95 e o tempo de sync vs trabalho da Fase 1. Promover só se o ganho estiver acima do ruído e sem diferença visual.

## Fase 3 — Readbacks: não precisar do dado fresco na CPU

Ordem conforme a tabela da Fase 1. Cada item exige evidência de site antes de mexer.

1. **Diagnóstico de frescor:** em cada readback amostrado (`KYTY_PERF_READBACK_CSV`, que já existe), adicionar se causou `SelfSubmitDrain`, o atraso em ticks e o tick do último *submit que escreveu no buffer*. Isso exige registrar o tick de escrita GPU por buffer (`last_gpu_write_tick`), o que também fecha a pendência "produtor real" dos relatórios.
2. **Esperar só o tick necessário:** se o último escritor do intervalo já foi submetido num tick anterior, esperar esse tick em vez de `CurrentTick` (sem self-drain). Isso elimina o submit parcial e a espera da GPU inteira quando o dado já estava a caminho. Testes de coerência: escrita GPU seguida de leitura CPU, intervalo escrito no command buffer atual (precisa continuar drenando) e aliases imagem/buffer.
3. **Rastreio de escrita GPU mais preciso**, para a pré-escrita CPU (7,7 GB, 59,9% sem cópia): verificar se o buffer vira GPU-dirty inteiro quando só está bound como gravável. Se sim, marcar dirty só no intervalo realmente escrito quando isso for conhecido (cópias, fills, RELEASE_MEM), mantendo o fallback conservador para UAV/BDA.
4. **Consumidores tolerantes a um frame de atraso:** só para dados que o hardware real já entrega de forma assíncrona (timestamps, contadores e estatísticas; queries hoje são sintéticas). **Não** aplicar a memória guest genérica nem a labels (a regra de `KYTY_LABELS_AFTER_GPU` continua). Remover `Finish()` total no caminho GDS (`SynchronizeGpu`) se a tabela mostrar que ele pesa, esperando só o tick da escrita GDS.
5. Cada item vira uma candidata separada, com A/B 3×3 na cutscene.

## Fase 4 — Comparação "sync vs trabalho" e outros jogos

Seguindo a sugestão do dev: para cada mudança promovida, mesma cena com a mudança ligada e desligada, comparando **ms/frame em zonas de sync** e **ms/frame de trabalho**, e não só o FPS. Repetir num segundo jogo disponível (perguntar quais o usuário tem). Atualizar `WOLVERINE_SESSION.md` e um novo `WOLVERINE_WAITS_REPORT.md` ao fim de cada fase, com tabelas, hashes, hipóteses refutadas e próximos passos. Commits locais por fase na branch `wolverine-performance` (push só para o fork, e só se o usuário pedir).

---

## Arquivos principais

- `src/graphics/host_gpu/renderer/performanceDiagnostics.{h,cpp}`: `WaitSite`, contadores novos
- `src/graphics/host_gpu/renderer/masterSemaphore.cpp`, `commandScheduler.cpp`: contagem instantânea/bloqueante, atraso, motivo de submit
- `src/graphics/guest_gpu/graphicsRun.cpp`: tags em Finish/GDS/label wait/SendReadbackSync
- `src/graphics/host_gpu/renderer/cache/bufferCache.cpp`, `streamBuffer.cpp`, `faultManager.cpp`, `textureCache.cpp`, `tiler.cpp`, `renderContext.cpp`, `renderCompute.cpp`
- `src/graphics/presentation/window/swapchain.cpp`, `videoOut.cpp`
- `tools/performance/capture.ps1`, `analyze.py` (+ `waits.py`), testes Python
- `tests/PerformanceDiagnosticsTests.cpp`, testes de `buffer_cache_ranges`

## Verificação

- Build Release (`cmake --build _Build/windows --target kyty_emulator performance_diagnostics_tests shader_recompiler_compute_tests`), CTest completo (diagnostics on/off, memory_tracker, gpu_command_lane, buffer_cache_ranges, shader_cse, renderer_benchmark_smoke e a suíte GPU), testes Python e `git diff --check`.
- Overhead da coleta medido na Fase 0.
- Cada candidata: 3 execuções de referência e 3 de candidata na cutscene do helicóptero, mesma janela por flip, screenshots de início e fim iguais, cache em estado igual e sem build concorrente. Anunciar ganho só se a diferença de mediana e p95 passar a variação entre execuções.
- Checagem visual da cutscene e do trecho de gameplay seguinte (sem artefatos, sem travar, áudio sincronizado).
