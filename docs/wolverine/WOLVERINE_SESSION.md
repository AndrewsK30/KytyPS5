# Continuidade das sessões — Wolverine / KytyPS5

Última atualização: 2026-10-05. Este é o ponto de entrada para retomar o trabalho.

## Objetivo e orientação do usuário

Melhorar o FPS de Marvel's Wolverine através de mudanças justificadas no código do KytyPS5 e aproveitar as melhorias nos demais jogos. O usuário pediu um plano de engenharia e autorizou começar sua implementação. Tentativas repetidas de flags não resolveram e não devem substituir diagnóstico, implementação e validação.

Trabalhar na cópia local e no fork do usuário. Não modificar/publicar na branch do autor da PR. O usuário perguntou explicitamente sobre isso e foi informado de que nenhum código foi enviado ao autor ou ao upstream.

## Estado para retomar

- Workspace: `G:\kity_work`.
- Repositório: `G:\kity_work\KytyPS5`.
- Branch: `wolverine-performance`, seguindo `fork/wolverine-performance`.
- HEAD local: `1e2185dba7c6a13662e2fb208131950fa33020de`.
- Base da PR 937: `41c7b27b738ce882c93108e8b882405e33a78d8d`.
- O HEAD é um cherry-pick adaptado da PR 900, originalmente `20c76cfeca1c92dc4957c0804cd5cf29e37f8daa`; contém resolução de conflito em `pm4Handlers.cpp`.
- Esse commit está apenas local: a branch está um commit à frente do fork. A instrumentação e suas ferramentas estão modificadas/não rastreadas, ainda sem commit.
- Nenhum ganho de FPS foi comprovado. A candidata de agrupamento FIFO está implementada/testada no fonte local, mas teve FPS exploratório menor e não foi promovida. O executável instalado foi restaurado. O relatório mais recente é `WOLVERINE_FIFO_REPORT.md`.
- A captura de três minutos terminou e seu processo foi encerrado. Não há processo do emulador identificado como ativo na última verificação.
- Verificar novamente Git/processos antes de retomar: este arquivo não substitui o estado real do disco.

Remotes e referências:

- `origin`: https://github.com/KytyPS5/KytyPS5.git
- `fork`: https://github.com/AndrewsK30/KytyPS5.git
- Fork do usuário: https://github.com/AndrewsK30/KytyPS5
- PR do Wolverine: https://github.com/KytyPS5/KytyPS5/pull/937 (`itsmemac:wolverine`).
- Issue de compatibilidade: https://github.com/KytyPS5/KytyPS5/issues/932.
- PR experimental de interrupt-only batching: https://github.com/KytyPS5/KytyPS5/pull/900.

## Jogo e máquina

Jogo: `D:\wolverine\[SuperPSX]-Marvels.Wolverine-PPSA03671-USA-Game (v01.001.005)-PS5`.
Title ID: `PPSA03671`. Versão: `01.001.005`.

Hardware identificado nas sessões anteriores: Ryzen 7 7800X3D, RTX 4090 (índice Vulkan 0) e GPU AMD integrada (índice 1). Driver NVIDIA registrado: 617.14. Uma amostra durante execução mostrou P0, 2655 MHz, 49 °C e 38% de utilização. Isso não equivale a um monitoramento completo; ainda é preciso registrar RAM/VRAM, paginação e clocks ao longo da captura. Não usar o AdapterRAM de WMI para inferir a VRAM da 4090.

## Histórico de compatibilidade e tentativas

1. Clone, submódulos, dependências e build inicial preparados. A versão inicial falhou no tracking de recurso de compute, hash `0xade1bef389644fa5`, PC `0x114c`, coerente com o relato de Wolverine na issue 932.
2. PR 937 obtida e usada como base. Ela permite avançar pela abertura, mas o usuário observou aproximadamente 7 FPS.
3. Build da PR exigiu `/EHsc` para Xbyak. Foi configurado no CMake local. As flags efetivas do Ninja incluem `/O2`; não atribuir lentidão a uma suposta build Debug sem verificar.
4. PresentMon mediu 51,73 FPS em uma execução inicial com Immediate. **Essa conclusão foi retirada:** a amostra incluía apresentações rápidas da inicialização e não representava o trecho lento. Não reutilizar o número como ganho.
5. Captura posterior no trecho aberto confirmou 5,92 FPS. Isso validou a reclamação do usuário.
6. Retirar `KYTY_LABELS_AFTER_GPU` não resolveu. Essa opção protege o momento de publicação dos labels e não deve ser removida como otimização geral sem validar coerência.
7. Agrupar todos os labels em um experimento local perdeu progressão. O experimento foi retirado do código. A interrupção dos contadores, isoladamente, não prova a causa do stall; o teste apenas não forneceu uma execução validada.
8. `--shader-optimization-type Performance` foi tentado. A leitura do código depois mostrou que `GetShaderOptimizationType` não tem consumidor no caminho de compilação desta revisão. **A flag não aplica otimização SPIR-V aqui.**
9. PR 900 foi integrada em um commit local. O usuário continuou relatando lentidão. A captura nativa posterior mostrou zero uso de seu caminho de flush interrupt-only no intervalo analisado; portanto ela não é a prioridade para este cenário.

O perfil Tracy anterior capturou aproximadamente 536 mil chamadas de RELEASE_MEM em 30 segundos, milhões de zonas, carregamento de módulos e apenas três frames registrados. É uma pista de frequência, não prova de que RELEASE_MEM domina o gameplay nem benchmark de FPS. Seus percentuais não devem ser tratados como um perfil estável do jogo.

## Implementação disponível

Arquivo central: `src/graphics/host_gpu/renderer/performanceDiagnostics.{h,cpp}`.

Coleta opcional por `KYTY_PERF_CSV=<caminho>`, desligada quando a variável está ausente. Usa contadores com um escritor por thread e snapshots seguros; gera linhas por apresentação e uma amostra por segundo. Não adiciona espera GPU e não muda as regras de sincronização/renderização.

Pontos instrumentados:

- `pm4Handlers.cpp`: RELEASE_MEM e flush interrupt-only.
- `graphicsRun.cpp`: flush do processor/label, barreira global e label publicado.
- `commandScheduler.cpp`: submits do renderer/presenter, espera no queue mutex, duração host de queue.submit e Finish.
- `masterSemaphore.cpp`: waits efetivos na API Vulkan e sua duração host.
- `bufferCache.cpp`: bytes programados de upload/readback de buffers.
- `textureCache.cpp`: bytes do caminho DownloadImageMemory.
- `pipelineCache.cpp`: CompilePermutation e espera inicial de pipeline gráfico.
- `videoOut.cpp`: chamadas concluídas de CompleteFlip.
- `swapchain.cpp`: apresentações bem-sucedidas, acquire, present e atualização do título.

Ferramentas e testes:

- `tools/performance/capture.ps1`: execução controlada, manifesto, logs, amostras do processo, patch e fontes não rastreadas; encerra o processo que criou e restaura o ambiente do chamador.
- `tools/performance/analyze.py`: analisa uma janela temporal explicitamente selecionada, excluindo deltas parciais nas bordas.
- `tools/performance/test_analyze.py`: teste do analisador com dados sintéticos.
- `tools/performance/README.md`: formato, comandos e limites da medição.
- `tests/PerformanceDiagnosticsTests.cpp` e registros CMake/CTest.

O script novo registra o título em process.csv para ajudar comparações com coleta desligada. A primeira captura foi lançada antes dessa pequena adição, então não tem esse campo. O primeiro lançamento também encontrou erro no argumento Git --output; o patch da captura foi recuperado manualmente. O script foi corrigido para abortar se essa preservação falhar.

## Achados da primeira captura nativa

Diretório: `G:\kity_work\KytyPS5\_Build\windows\benchmarks\diagnostics-opening-01`.
Referência detalhada: `WOLVERINE_FIRST_CAPTURE.md` neste workspace.
Dados principais: `renderer.csv`, `manifest.json`, `stdout.txt`, `stderr.txt`, `process.csv`, `source.patch`, `untracked-sources/`, `CMakeCache.txt` e `report-120-180.json`.

Binário SHA256: `435C42C25952F6DCBE7BF4B2B357EFE830C9305A3A4A893241941707673A04B6`.

Execução exploratória desde a abertura, sem input. Não foi definido um save/câmera de gameplay repetível. Intervalo analisado: 120,055–179,082 segundos desde o início da coleta, duração efetiva de 59,027 segundos.

| Métrica | Resultado |
| --- | --- |
| Apresentações / flips GPU concluídos | 371 / 371 |
| Taxa de apresentação / conclusão de flips | 6,285 por segundo |
| Intervalo entre apresentações: mediana / p95 / p99 | 133,457 / 249,954 / 565,315 ms |
| Submits do renderer | 531.619, cerca de 1.433 por apresentação |
| RELEASE_MEM / flushes por label | 423.408 / 423.408 |
| Flush interrupt-only | 0 |
| Barreiras globais | 355.375 |
| Readback de buffers | 20.737.386.368 bytes (~20,737 GB decimal) |
| Readback de imagens | 372.805.632 bytes |
| Upload de buffers | 11.531.665.408 bytes |
| Tempo host agregado de queue.submit | 6,946 s |
| Tempo host agregado de waits de semáforo | 37,426 s |
| Compilações de shader no escopo medido | 63, com 0,606 s acumulados |
| Espera inicial de pipeline gráfico | 0,132 s |
| Atualização do título | 0,026 s |

Esses dados priorizam investigação de transferências e submissões. A compilação e o título medidos têm peso menor nesta janela, mas o escopo de CompilePermutation não inclui toda a tradução/preparação anterior.

Limites que devem acompanhar qualquer conclusão:

- Os tempos host podem se sobrepor entre threads: não somá-los como custo exclusivo de um frame.
- Scopes são contabilizados quando terminam; uma espera em andamento aparece só na conclusão.
- Counters são correlação por intervalo de apresentação, não ownership exato por frame guest.
- Bytes são transferências programadas nos caminhos instrumentados, não medição física de PCIe nem todas as cópias internas GPU.
- A instrumentação ainda não teve overhead percentual medido. Não declarar que está abaixo de 2%.
- O log confirmou cache Vulkan do driver desativado por dirty build. Manter estado de cache comparável nos próximos testes.
- O manifesto registra apenas uma amostra inicial de NVIDIA-SMI.
- Ainda não há timestamps GPU nem tempo GPU por hash de shader.

## Outras evidências e arquivos históricos

Em `KytyPS5/_Build/windows/benchmarks`:

- `baseline-presentmon.csv`: aproximadamente 10,63 FPS numa amostra anterior, com cenário insuficientemente fixado.
- `immediate-presentmon.csv`: 51,73 FPS, amostra de inicialização inválida para afirmar ganho de gameplay.
- `live-gameplay-presentmon.csv`: 5,92 FPS, média de 168,94 ms, mediana de 166,72 ms, GPU time mediano ~166 ms e GPU busy ~67 ms. O CSV identifica DXGI apesar do renderer Vulkan; correlacionar métricas antes de inferir execução GPU.
- `no-labels.*`, `performance-opt.*`, `batched-labels.*`, `restored.*`, `interrupt-batching.*`: logs dos experimentos anteriores.

Em `KytyPS5/_Build/windows/traces`:

- `wolverine-pr937-warm.tracy` e `zone-self-time.csv`: perfil distorcido por overhead/carregamento, conforme explicado acima.

O backend atual retorna queries de oclusão sintéticas sempre visíveis em `graphicsRun.cpp`. Isso pode criar trabalho extra, mas ainda não foi quantificado. A compilação gráfica já tem workers e uma espera inicial de 20 ms em `pipelineCache.cpp`; medir antes de criar outro mecanismo assíncrono.

## Próxima tarefa técnica

1. Classificar waits por thread e consumidor: GPU, completion callbacks, CPU guest/fault handlers e stream-buffer wraps.
2. Classificar readbacks por origem, endereço/intervalo e recurso: labels, argumentos indiretos, SRT/BDA, dados de recursos e aliases imagem/buffer. Usar amostras limitadas, sem dump do conteúdo do jogo.
3. Separar tipos de RELEASE_MEM, incluindo timestamps, e correlacionar tick produtor com a leitura consumidora.
4. Medir submits consecutivos realmente redundantes, preservando callbacks, ticks e semáforos.
5. Escolher e implementar uma correção limitada ao custo comprovado, com testes de coerência/progresso. Exemplos condicionais: reduzir janela de readback ou retirar envio redundante.
6. Medir overhead da coleta; fixar o trecho visual e realizar três execuções A/B de pelo menos 60 segundos antes de anunciar ganho.
7. Completar timestamps GPU e testar outros jogos disponíveis conforme o plano.

Não assumir que um label pode ser adiado só porque não há consumidor no command buffer atual. Pode haver espera na CPU guest ou em outra fila. Não acelerar o jogo omitindo draws, desligando atomics ou publicando labels antes da GPU.

## Build e comandos para retomada

PowerShell; executar a partir de `G:\kity_work\KytyPS5`. Usar caminhos absolutos porque processos desta sessão podem não herdar Git/CMake no PATH.

```powershell
$env:Path = 'C:\Program Files\Git\cmd;C:\Program Files\CMake\bin;' + $env:Path
git status --short --branch
git log -2 --oneline
cmake --build _Build/windows --target kyty_emulator performance_diagnostics_tests --parallel 16
ctest --test-dir _Build/windows -R '^performance_diagnostics' --output-on-failure
& G:/kity_work/tools/python313/python.exe tools/performance/test_analyze.py
git diff --check
```

Dependências preparadas nas sessões anteriores:

- Visual Studio 2022 Community e clang-cl: `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe`.
- CMake: `C:\Program Files\CMake\bin\cmake.exe`.
- Git: `C:\Program Files\Git\cmd\git.exe`.
- GitHub CLI: `C:\Program Files\GitHub CLI\gh.exe`, autenticado como `AndrewsK30` na preparação anterior; verificar se necessário.
- Qt: `G:\kity_work\Qt\6.8.3\msvc2022_64`.
- Vulkan SDK: `C:\VulkanSDK\1.4.363.0`.
- glslang portátil: `G:\kity_work\tools\glslang\bin\glslangValidator.exe`.
- Python portátil: `G:\kity_work\tools\python313\python.exe`.

Executável instalado: `KytyPS5/_Build/windows/install/kyty_emulator.exe`. A versão anterior foi preservada em `_Build/windows/benchmarks/pre-diagnostics-1e2185db.exe`. Rebuild não copia automaticamente o executável para install: instalar ou copiar explicitamente após sucesso, com o jogo fechado.

Nova captura, mantendo a configuração compatível e descrevendo o cenário:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/performance/capture.ps1 -Game 'D:\wolverine\[SuperPSX]-Marvels.Wolverine-PPSA03671-USA-Game (v01.001.005)-PS5' -Scenario 'DESCREVER O TRECHO' -DurationSeconds 180
```

O Bypass vale apenas para esse processo; não alterar a ExecutionPolicy global. O harness usa `--gpu 0 --present-mode Immediate --bindless --redzone --tessellation --readback-linear-images true`, `KYTY_IGNORE_WRITTEN_OVERLAP=1`, `KYTY_LABELS_AFTER_GPU=1` e desativa `KYTY_DEFER_RELEASE_MEM_INTERRUPTS`. O objetivo é manter a configuração fixa enquanto o código é medido.

Análise do relatório existente:

```powershell
& G:/kity_work/tools/python313/python.exe tools/performance/analyze.py _Build/windows/benchmarks/diagnostics-opening-01/renderer.csv --start-seconds 120 --end-seconds 180
```

PresentMon na preparação anterior: `C:\Users\andre\AppData\Local\Microsoft\WinGet\Packages\Intel.PresentMon.Console_Microsoft.Winget.Source_8wekyb3d8bbwe\presentmon.exe`. Uma tentativa posterior recebeu access denied para ETW; a coleta nativa funciona sem essa dependência. Não ficar repetindo PresentMon sem resolver sua disponibilidade. Tracy instalado pelo WinGet, com capture/csvexport no pacote `wolfpld.tracy_Microsoft.Winget.Source_8wekyb3d8bbwe`.

Documentos devem ser lidos como UTF-8. No Windows PowerShell antigo: `Get-Content -Encoding UTF8` evita aparência de caracteres corrompidos.

## Validação concluída e pendências

Concluída: build Release, dois testes CTest de diagnóstico, teste Python, sintaxe PowerShell, captura real, relatório JSON e `git diff --check` com a configuração Git normal. Não usar `-c core.autocrlf=false` neste checkout para checar whitespace: isso interpreta CRLF como alterações integrais e gera falso ruído.

Pendentes: overhead A/B da coleta, cenário repetível, classificação dos consumidores, correção funcional, timestamps GPU, comparação de FPS e regressão em outros jogos. Atualizar este arquivo ao finalizar cada sessão com os novos números, hipóteses refutadas, commits, arquivos e próximos passos.

Documentos complementares: [plano completo](WOLVERINE_FPS_PLAN.md) e [relatório da primeira captura](WOLVERINE_FIRST_CAPTURE.md).

## Continuação em 2026-10-05: consumidores e granularidade de readback

Relatório detalhado: [WOLVERINE_READBACK_REPORT.md](WOLVERINE_READBACK_REPORT.md). Ler este relatório antes de repetir experiências com o tamanho da janela.

Implementado e mantido, ainda sem commit:

- Contadores de bytes/pedidos por origem: CPU read/write, GPU-thread read/write, metadados e GC. TLS de origem propagado explicitamente através de `SendCommandSync`, para que um pedido CPU não seja atribuído à thread GPU executora.
- Waits separados por thread GPU, completion worker e demais threads, mantendo o agregado; o mesmo timer fornece ambas as métricas.
- RELEASE_MEM separado por data_sel: dword, qword, timestamp, GDS ou nenhum dado.
- Testes concorrentes verificam origens e restauração de TLS, além dos timers espelhados. README/analisador documentam limites de atribuição e padding de bytes.
- Teste GPU de coerência antes de escrita CPU em `tests/ShaderRecompilerComputeTests.cpp`: preservação de dados na mesma página, dirty bytes externos à janela mantidos sob propriedade GPU, reupload CPU sem clobber de valores GPU.

Captura de referência `KytyPS5/_Build/windows/benchmarks/readback-origin-01/late.json`, janela efetiva 120,059–179,081 s: 346 presents/flips, 5,862 FPS, 18,137 GB de download de buffers. Origens: CPU read 8,458 GB, CPU pre-write 7,700 GB, GPU read 1,971 GB, GPU pre-write 0,009 GB, demais zero. GPU-thread wait 9,411 s, completion wait 25,543 s. Não chamar os 34,954 s agregados de bloqueio exclusivo da thread GPU. GC/metadados não explicam o volume de buffers nessa captura.

Experimento rejeitado `KytyPS5/_Build/windows/benchmarks/write-page-candidate-01/`: janela pré-escrita limitada às páginas de tracker afetadas (4 KiB), leituras mantidas em 512 KiB. Passou no teste GPU, mas no trecho pesado chegou a 1,745 FPS, 530.286 pedidos CPU de escrita para 103 apresentações, 6.326 submits por apresentação e 31,953 s de espera na thread GPU. A referência tinha 40.823 pedidos CPU de escrita para 346 apresentações. Volume por apresentação não caiu (52,419 MB referência; 56,085 MB tentativa). A evidência sugere perda de coalescing e multiplicação de faults/sincronizações. Não reduzir novamente a janela apenas para diminuir bytes sem medir número de pedidos e waits.

Os FPS altos no início eram do logo Marvel, confirmado por captura de tela, e não constituem ganho de gameplay. Janelas por tempo de lançamento não garantem cena equivalente. Não anunciar uma porcentagem de regressão ou ganho controlado a partir desta comparação; a tentativa foi rejeitada por não mostrar benefício e apresentar forte sinal de regressão. Foram salvos `window-mid.png` e `window-heavy.png`, com sobreposição parcial do terminal.

Estado final: política de janela 512 KiB restaurada no fonte e no executável instalado. Binário instalado e `_Build/windows/kyty_emulator.exe` SHA256 `1992C862E48FA0EC8130E1F5F45222C3765C5A6CA6AD0805850C796505B16000`. Referência preservada em `benchmarks/readback-origin-baseline.exe`; tentativa rejeitada em `benchmarks/write-page-rejected.exe`, com patch/fontes exatos na pasta de captura. Não executar acidentalmente o binário rejeitado como versão corrente.

Build Release final aprovado; CTest 3/3 (`performance_diagnostics`, `performance_diagnostics_disabled`, `buffer_cache_ranges`); teste Python do analisador aprovado; `git diff --check` aprovado. Logs em `_Build/windows/readback-candidate-build.log`, `readback-candidate-test.log` e `readback-restored-build.log`. As duas capturas de 180 segundos terminaram e não há processo Kyty ativo na verificação final. HEAD/branch/remotes permanecem os mesmos; nenhum commit/push nesta continuação e nada publicado na branch do autor.

Próximo passo: identificar intervalos/ticks e recursos que originam readbacks recorrentes com amostras limitadas. Separar download efetivo, despacho `SendCommandSync`, espera da GPU e publicação das cópias. Procurar redução de sincronizações mantendo coalescing e coerência antes de outra mudança de política. Definir cenário repetível (foi pedida descrição do trecho/save ao usuário), medir overhead e fazer três pares A/B de pelo menos 60 segundos antes de declarar ganho. Timestamps GPU e outros jogos seguem pendentes.

## Continuação autorizada: amostras e custo de despacho

Relatório atual: [WOLVERINE_SYNC_REPORT.md](WOLVERINE_SYNC_REPORT.md). A instrumentação de intervalos/ticks e fases foi implementada, testada e usada em duas novas capturas de 180 s; permanece sem commit junto das alterações anteriores.

- Novos contadores: `read_dispatch_ns`, `read_prepare_ns`, `read_gpu_wait_ns`, `read_publish_wait_ns`, `read_download_requests`, `read_no_download_requests`.
- Novo `KYTY_PERF_READBACK_CSV` e CSV de 20 colunas. 1 pedido em 64 por thread, até 20.000 linhas por processo. TLS propagado no callback síncrono, sem conteúdo de memória. Bytes efetivos e padded são separados; ticks não identificam o último produtor.
- Harness configura ambos os CSV e restaura ambos os envs; `-DisableDiagnostics` desliga ambos.
- Novo analisador `tools/performance/readbacks.py`; teste Python verifica ordem de conclusão vs início e agrupamento sem extrapolar somas da amostra. Teste C++ verifica concorrência, TLS, frequência e limite de 20.000 linhas. README explica limites.

Referência `KytyPS5/_Build/windows/benchmarks/readback-samples-01/late.json`, 120,016–179,091 s: 403 presents/flips, 6,822 FPS exploratórios; 54.896 pedidos com e 35.191 sem download; despacho 20,864 s; preparo 0,620 s; scheduler wait de readback 10,853 s; publication wait 2,742 s. Tempos podem se sobrepor. Em `samples-late.json`, 444/741 amostras CPU pre-write não fizeram cópia (59,9%). O despacho ainda custa mediana de 0,1874 ms nesses pedidos CPU pre-write. A amostra parcial inicial tinha 75,2%, não usar como número da janela final.

Tentativa rejeitada: segunda checagem CPU pre-write via `MemoryTracker::InvalidateRegion` antes de `SendCommandSync`, combinando teste GPU e marcação CPU sob o mesmo lock. Preservou 512 KiB e passou nos testes, mas evitou só **4 envios em 155.712 pedidos CPU de escrita no processo inteiro**; na janela final, 1 em 57.780. Quando o dado torna-se atual, em geral o pedido já está na fila. A tentativa foi retirada, inclusive seu contador específico. Código/patch exatos estão preservados em `benchmarks/clean-write-candidate-01/`, binário em `benchmarks/clean-write-rejected.exe`. Referência instrumentada preservada em `benchmarks/readback-samples-baseline.exe`.

Tentativa apresentou 7,958 FPS exploratórios na janela final, mas avançou por cenas diferentes e teve 94 compilações contra 19 na referência. Não atribuir esse aumento à checagem que evitou um único envio na janela. Continua sem ganho de FPS comprovado.

Estado final instalado: política original de readback, sem a rechecagem rejeitada, com novas métricas/amostras. SHA256 `BA74C6D3EB379B2EBE981AEA36502A3DDEC99D8F6018241EC780D638832DFCCC`. Build Release final aprovado, CTest 4/4 (`performance_diagnostics`, `performance_diagnostics_disabled`, `memory_tracker`, `buffer_cache_ranges`), Python 2/2, `git diff --check` aprovado. Logs `readback-samples-build.log`, `clean-write-build.log`, `clean-write-test-build.log`, `readback-samples-final-build.log` em `_Build/windows/`. As capturas terminaram; nenhum emulador ativo na verificação final. HEAD/branch/remotes inalterados, nenhum commit/push e nada enviado ao autor.

Próximo alvo: sequência/tamanho da fila e relação entre pedidos **já pendentes** e o download anterior que resolveu a página. Coalescing/cancelamento precisa preservar ordem de comandos e vida útil dos callbacks; o callback atual referencia stack do caller, portanto não retornar cedo sem um estado compartilhado que impeça execução posterior. O relatório descreve esses requisitos. Ainda faltam produtor real/tick de escrita, overhead da coleta, cenário visual repetível e três A/B antes de anunciar ganho. Não repetir rechecagem pré-envio nem redução para 4 KiB como solução presumida.

## Continuação: candidata de agrupamento FIFO

Relatório atual: [WOLVERINE_FIFO_REPORT.md](WOLVERINE_FIFO_REPORT.md).

Implementado, ainda sem commit: `GuestGpu::SendReadbackSync` e entradas tipadas da fila. Somente pedidos consecutivos no tail ainda pendente, do mesmo cache/janela 512 KiB, podem compartilhar entrada; limite 16. Comandos comuns e janelas diferentes interrompem grupos. Cada operação mantém seu readback/waits e libera seu semáforo imediatamente ao retornar. Não houve cancelamento, reutilização de versões nem salto de callbacks/submits GPU. `ReadMemory` usa esse método. O fonte e `_Build/windows/kyty_emulator.exe` continuam como candidata para revisão.

Métricas novas: `readback_queued`, `readback_queue_depth` (soma de entradas pendentes, não membros), `readback_tail_eligible`, `readback_joined`. Referência sem agrupar, com tags, em `benchmarks/readback-fifo-baseline-01/`; candidata em `benchmarks/readback-fifo-candidate-01/`. Ambas duraram 180 s. Usar `steady.json` 90–150 s: o fim da referência coincidiu com início de build e foi excluído. Candidata não teve build concorrente.

Referência: 94.111 pedidos, 35.467 elegíveis, zero agrupados; 485 presents/flips, 8,110 FPS exploratórios, mediana 132,353 ms, p95 152,035 ms. Candidata: 86.698 pedidos, 20.070 agrupados; 66.628 entradas criadas, **23,15% de entradas evitadas na própria candidata**; 418 presents/flips, 6,980 FPS exploratórios, mediana 133,707 ms, p95 166,808 ms. Compilações 96 vs 18 e progressões visuais diferentes. Não atribuir ganho de FPS à redução de entradas nem descartar o sinal de taxa menor. Tempo de despacho por pedido quase igual, aproximadamente 240 vs 239 microsegundos. Esta candidata reduz gerenciamento da fila, não o volume de GPU submits.

Build Release aprovado. CTest 5/5: diagnósticos on/off, memory_tracker, gpu_command_lane e buffer_cache_ranges. Testes novos verificam FIFO entre grupos/comando comum/janela diferente, liberação individual antes de uma espera do membro seguinte, dispatch inline aninhado e atendimento de 22 callers em dois grupos limitados a 16. Python 2/2 e `git diff --check` aprovados. Logs `readback-fifo-build.log` e `readback-fifo-candidate-build.log` em `_Build/windows/`. Não há emulador ativo na verificação final. Nenhum commit/push.

**Instalação restaurada** para a versão anterior sem esta candidata: `install/kyty_emulator.exe` SHA256 `BA74C6D3EB379B2EBE981AEA36502A3DDEC99D8F6018241EC780D638832DFCCC`, preservada em `benchmarks/pre-fifo-readbacks.exe`. Candidata preservada em `benchmarks/readback-fifo-candidate.exe`, SHA256 `6E4AAE85E8EE28427E5238C5364A87DD8C1204026E74E88FB82624ACB39911DA`. Referência tagged/unbatched em `benchmarks/readback-fifo-baseline.exe`. Fonte/raiz do build são candidatos: instalar/copiar o build recolocaria a candidata em uso. Não fazê-lo como melhoria aprovada de FPS.

Próximo passo: fixar cena e comparar candidata/ref em A/B controlado antes de integrar. Investigar custo dominante real por frame/CPU/GPU e quantidade de submits sem trabalho nativo; os submits continuam na ordem de mil por apresentação. Preservar ticks/labels/interrupções e ordem dos comandos ao tentar retirar submits. Relação com último produtor e publicação continua necessária antes de cancelar ou reutilizar pedidos. Melhorias locais de estruturas que não reduzem o caminho crítico não devem ser tratadas como avanço de FPS.

## Continuação: adaptação de CSE do RPCS3

Pedido do usuário: implementar uma técnica concreta do RPCS3 e verificar se funciona aqui. Relatório: [WOLVERINE_CSE_REPORT.md](WOLVERINE_CSE_REPORT.md). Fonte oficial RPCS3 `SPULLVMRecompiler.cpp`, revisão `3e7477107a356c5d1fae92a7dc3c8d5953984782`, usa LLVM `EarlyCSEPass(true)`.

Implementado, ainda sem commit, CSE local de expressões inteiras na IR dos shaders Kyty antes de emitir SPIR-V. Chave completa opcode/flags/operandos; apenas lista explícita de operações puras, dentro do mesmo bloco, excluindo definições retidas em metadados. Não é transplante de LLVM/SPU nem implementação do EarlyCSE completo. Novas métricas estáticas `shader_cse_candidates`, `shader_cse_eliminated` e tempo host `shader_cse_ns`. Não atribuir instruções dinâmicas/FPS a esses contadores.

Build Release e CTest selecionado 7/7 passaram, incluindo o novo teste de 10.000 entradas e limites semânticos; suíte GPU completa `shader_recompiler_compute` passou em 19,11 s. Capturas em andamento ao escrever esta entrada; resultados e estado final serão registrados abaixo. Candidata FIFO mantida igual na candidata CSE e na referência sem CSE deste experimento. Executáveis de teste recebem nomes separados; a instalação estável `install/kyty_emulator.exe` não foi substituída. Nenhum commit/push.

Capturas concluídas, 180 s cada, sem build concorrente: `benchmarks/shader-cse-candidate-01/` e `shader-cse-control-01/`, com `late.json` e `full.json`. Candidata removeu **10.445 / 531.604 expressões inteiras elegíveis (1,965%)**, em 71,775 ms acumulados host da pass. Não é porcentagem de todas as instruções nem de trabalho dinâmico GPU.

Janela exploratória final 120–180 s: referência 260 presents/flips, 4,405 FPS, mediana 117,502 ms, p95 251,986 ms e 119 compilações; candidata 289 presents/flips, 4,896 FPS, mediana 116,958 ms, p95 243,638 ms e 110 compilações. Continua **sem ganho de FPS comprovado**: cenas/progressões e carregamento não equivalentes; a mediana praticamente igual não resolve lentidão. Submits continuam altos (358.431 vs 391.021 na janela). Detalhes e hashes no relatório CSE.

Chamada CSE restaurada após referência, build final aprovado; fonte e binário raiz são candidatos CSE + FIFO. `install/kyty_emulator.exe` permanece estável pré-FIFO, SHA256 `BA74C6D3EB379B2EBE981AEA36502A3DDEC99D8F6018241EC780D638832DFCCC`. Binários preservados `benchmarks/shader-cse-candidate.exe` e `shader-cse-control.exe`. Nenhum commit/push, branch/HEAD inalterados. Próximo alvo: timestamps GPU e custo crítico CPU por frame em cenário repetível, antes de ampliar otimizações de shaders. Validar em outros jogos e três A/B controlados segue pendente.

Verificação final: Python 2/2, `git diff --check` aprovado, nenhum processo do emulador ativo. Fonte restaurado idêntico ao snapshot da candidata capturada. Hash do binário raiz após relink `F81BC625BA4A47F99FAE6C96630D585C80FF0992011AEE0632F6B938B68664BF`; o executável exato da captura foi preservado separadamente (hash no relatório). Não confundir relink final/candidata com instalação estável.

## Continuação: benchmark 3D de integração sem abertura do jogo

Pedido do usuário: criar teste 3D de carga/integração para não rodar Wolverine e seus filmes a cada tentativa. Implementado e executado. Relatório completo: [WOLVERINE_BENCHMARK_REPORT.md](WOLVERINE_BENCHMARK_REPORT.md).

Cena offscreen determinística: 64 cubos com perspectiva/depth, 512×512. Usa pipeline cache, bindings/targets, scheduler e anel de download reais. Pixel shader passa pelo compiler RDNA2; vertex shader é fixture SPIR-V, geometria pré-projetada. Não é demo/executável PS5 completo: CPU guest, decode PM4 completo, faults CPU/`SendReadbackSync`, labels e complexidade de shaders do jogo não estão cobertos. Não atribuir FPS de jogo ao throughput deste teste.

Novos arquivos: `tests/RendererBenchmark.inc`, `tests/RunRendererBenchmark.cmake`, `tools/performance/renderer.ps1`, `renderer.py`, `test_renderer.py`; integração em `ShaderRecompilerComputeTests.cpp` e CMake. CLI `--renderer-benchmark OUTPUT_DIR FRAMES DRAWS`; wrapper faz três execuções e arquiva binário/DLL, patch/fontes/config, CSV por frame, metadados, JSONs e PNG. Aquece por >=16 frames e >=2 s por perfil; um frame em voo. Query timestamps GPU com period/valid bits; intervalos entre vários submits incluem gaps, não são GPU busy time. Diagnósticos CSV antigos ficam desligados no benchmark e envs são restaurados.

Referência canônica **cube-grid-v2**, `benchmarks/renderer-production-01/`: 256 draws/frame, 180 frames/perfil em três processos, total 1.620 frames. Mediana das medianas host: raster 0,4845 ms (faixa 0,4833–0,4952), submits 1,3417 ms (64 submits/frame; 1,3290–1,3562), readback 2,2551 ms (4 MiB/frame; 2,2249–2,3377). Render GPU ~0,0428 ms raster; readback GPU ~0,6302 ms. Checksum de todos os perfis/repetições `11224440966537881514`, 65.090 pixels cobertos.

Carga pesada `benchmarks/renderer-stress-01/`: 4.096 draws/frame, 1.024 submits/frame no perfil submits, três execuções de 60 frames/perfil (540 frames). Host raster 7,073 ms, submits 22,214 ms, readback 9,562 ms; mesma imagem/binário. É resposta a uma mudança de carga, **não ganho de FPS/otimização do emulador**. Artefatos, faixas e limites no relatório.

Executável benchmark Release SHA256 `72B962D12667CE83F831E1970FD3B0AD511676B970E361D45159A2C230212187`, preservado em ambas as pastas como `benchmark.exe` com DLL. O primeiro protótipo v1 em `renderer-fixed-01/02/` usava staging auxiliar e teve readback ~19,7 ms; não usar como baseline do Kyty. A versão v2 usa o anel de download de produção. Falha inicial de compilação (Program vs CompiledShaderInfo) corrigida; wrapper ExitCode nulo no Windows PowerShell corrigido preservando o handle. Capturas antigas foram mantidas, sem ocultar falhas.

Build Release aprovado, CTest 8/8 (inclui suíte GPU inteira e benchmark smoke), Python 5/5, sintaxe PowerShell e `git diff --check` aprovados. Nenhum emulador/teste GPU ativo ao finalizar. Instalação estável intacta (`install/kyty_emulator.exe`, SHA256 `BA74C6D3EB379B2EBE981AEA36502A3DDEC99D8F6018241EC780D638832DFCCC`). Nenhum commit/push; branch `wolverine-performance`, HEAD `1e2185dba7c6a13662e2fb208131950fa33020de` inalterados.

Próxima sessão: usar `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/performance/renderer.ps1` na raiz do repo; para pressão em scheduler, `-Frames 60 -Draws 4096 -Runs 3`. Fazer A/B somente com cargas/versões equivalentes, uma mudança por hipótese, checksums e distribuição dos tempos. Para testar otimizações de faults/fila de readback, acrescentar uma carga que exercite explicitamente produção GPU, coerência CPU e despacho guest; o fixture atual não valida essas otimizações. Manter validação final em cena repetível de Wolverine.

## Continuação: plano "waits por call site / GPU atrasada / churn" (sugestão de um dev de UFC 5/6)

Plano completo: [WOLVERINE_WAITS_PLAN.md](WOLVERINE_WAITS_PLAN.md). As decisões do usuário foram: cena fixa = **cutscene do helicóptero**, e os três pontos em ordem, com A/B entre eles. Feito nesta sessão, **sem commit e sem captura do jogo**:

- **Waits por call site.** `WaitSite`/`WaitSiteScope`/`WaitTimer` em `performanceDiagnostics`. Gera colunas `wait_<site>_{calls,blocking,ns,lag_ticks,drains}`.
  - `MasterSemaphore::Wait` agora conta também as esperas instantâneas, registra o atraso da GPU em ticks e abre uma zona Tracy `Wait` com o nome do site.
  - Sites tagueados: Finish (GDS, processor, unmap, compute recovery, collect faults), shutdown, GC sujo, fault buffer, os quatro anéis, download de textura, frame pool do swapchain, thread de completion.
  - Esperas por cv: publish (`WaitPriorityOperations`), despacho de readback/command sync, **thread GPU bloqueada em label (WaitFor 100 ms)** e videoOut slot/flip/vblank. A espera em label antes não era medida.
- **Submits por motivo** (`submit_label/processor/flip/drain/finish/shutdown/other`) e orçamento da thread GPU (`gpu_process_ns`, `gpu_command_ns`, `gpu_idle_ns`).
  - Hipótese da leitura do código, ainda não medida por motivo: com `KYTY_LABELS_AFTER_GPU`, todo RELEASE_MEM com label faz `Flush()`. Na primeira captura foram 423.408 label flushes para 531.619 submits (~80%).
- **Contadores de churn:** `buffer_create/_bytes`, `buffer_join/_bytes`, `buffer_delete`, `vma_buffer_create/_ns`, `dedicated_alloc`, `bda_table_upload`, `image_create`, `tiler_scratch_alloc`.
  - Pela leitura do código, todo buffer guest tem BDA, portanto `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`, portanto um `vkAllocateMemory` por buffer.
  - `CreateBuffer` alinha só a 16 KiB.
  - O tiler cria um VMA buffer por operação.
- **Ferramentas.**
  - `analyze.py --start-flip/--end-flip`: janela por presentation id em vez de tempo de boot.
  - Novo `waits.py --table`: ranking por site, % de esperas instantâneas, ms por apresentação, lag médio, submits por motivo e orçamento da thread GPU.
  - `capture.ps1 -ScreenshotEverySeconds N`: screenshots `t<seg>s-p<presentation>.png` para calibrar a cutscene.
  - `test_waits.py` novo. `test_renderer.py` corrigido: o Python embutido não põe o diretório do script no `sys.path`.
- **Validação.** Build Release completo aprovado. CTest: 29/29 dos testes compilados passaram, e o teste `performance_diagnostics` foi estendido para sites aninhados, fallback, instantâneo vs bloqueante, concorrência, `WaitTimer` e motivos de submit. Os 26 "Not Run" são alvos fora do build padrão, como antes. Python 8/8 e `git diff --check` aprovados. Log em `_Build/windows/waitsites-tests.log`.
- **Estado.** `install/kyty_emulator.exe` intacto (`BA74C6D3…`). O binário raiz `_Build/windows/kyty_emulator.exe` (`6CBE8A1C…`) contém **FIFO + CSE + a nova instrumentação**. Branch e HEAD inalterados, nada publicado.

Próximo passo (Fase 0 do plano):
1. Rodar a calibração com screenshots para achar os presentation ids de início e fim da cutscene do helicóptero.
2. Gerar uma referência sem FIFO/CSE (patches preservados nas pastas de captura) com a nova instrumentação.
3. Fazer 3 capturas da cutscene, mais 3 com `-DisableDiagnostics` para medir o overhead.
4. Rodar `waits.py --table` e decidir a Fase 2 (granularidade de 1 MiB) e a Fase 3 (readbacks) pelos números.

### Calibração executada (2026-10-05) — `benchmarks/waitsites-calibration-01/`

**Sem input, o jogo para numa tela de acessibilidade "SCREEN READER" a partir de ~136 s** (presentation ~1800) e fica nela. A cutscene do helicóptero exige passar pelo menu e iniciar o jogo, portanto precisa de input humano ou de um save. Hipótese: as janelas antigas de 120–180 s do Codex eram essa tela, não gameplay. Comparar com cautela.

Os screenshots desta execução saíram deslocados (problema de DPI). `capture.ps1` foi corrigido com `SetProcessDPIAware`, e a correção foi verificada com a janela inteira. Também ficou claro que o título do emulador mostra `frame: N`.

Tela de menu estável, flips 1866–3136 (99,5 s, sem compilação de shader, binário `6CBE8A1C…` com FIFO+CSE ativos). Resultado: 12,77 FPS, mediana 82,4 ms, p95 84,3, p99 100,7. Por apresentação:

- **Submits: 1.287**, dos quais **label 1.074 (83%)**, processor 137, drain 75 e flip 1. Confirmado: a quase totalidade dos submits vem do `Flush()` por label com `KYTY_LABELS_AFTER_GPU`.
- **Thread GPU: ~82 ms** = `queue.submit` 16,2 + lock 1,1 + bloqueada em label (WAIT_REG_MEM) 13,5 + waits de semáforo de readback 9,6 (todos são *drains*: cada readback submete o buffer aberto) + o resto do processamento PM4/gravação (~40). Idle 0, ou seja, a thread GPU é o gargalo.
- Threads guest esperando readbacks (`readback_dispatch`): 44,6 ms/apresentação, ~15 pedidos de 299 µs. Isso se sobrepõe à thread GPU.
- `priority_ops` (thread de completion) 45,6 ms: é a espera natural pela GPU numa thread auxiliar; lag médio de 4,8 ticks. Não está no caminho crítico por si só.
- Readback 31,1 MB/apresentação; upload 15,9 MB. Pedidos CPU: 42 de leitura e 107 de escrita; 84 sem download e 75 com download.
- **Churn:** buffers quase nada (90 criações em 99 s; o arredondamento para 1 MiB tem pouco alvo *nesta tela*). Porém há **11,5 imagens criadas e 24,6 scratch do tiler por apresentação** numa tela estática. O churn aqui é de **texturas/tiler**, não de buffers.
- Esperas instantâneas: anéis 100% instantâneos, swapchain 100%, publish 63,5%, priority 69%.

Interpretação, ainda hipótese: os maiores alvos são **(1) o submit por label**, ~16 ms de `vkQueueSubmit` mais bolhas por frame, e **(2) os drains de readback**. O candidato natural para (1) é fazer o flush do label no fim de cada fatia de `Process()` ou antes de suspender em WAIT_REG_MEM, em vez de a cada RELEASE_MEM. Isso dá latência limitada e progresso garantido. É diferente do "agrupar todos os labels" que perdeu progressão; investigar o que aquela tentativa fez antes de implementar.

### Referência para os A/B

FIFO (`KYTY_READBACK_BATCH=1`) e CSE (`KYTY_SHADER_CSE=1`) agora são **opt-in por env, desligados por padrão**. O CTest `gpu_command_lane` roda com `KYTY_READBACK_BATCH=1`. Binário de referência `benchmarks/waitsites-reference.exe` (também `install/waitsites-reference.exe`), SHA256 `E6A9E552542985840EC933A53F5CF80FAD04E04981373F3C44FC9EEBC55BE95F`. Validação: build ok, CTest 29/29 dos compilados, Python 3/3 arquivos (8 testes), `git diff --check` ok. `analyze.py` agora descarta uma última linha truncada pelo encerramento forçado.

### Referência na cutscene do helicóptero — `benchmarks/waitsites-heli-ref-01/`

Binário `waitsites-reference.exe` (`E6A9E552…`, FIFO/CSE desligados). O usuário navegou pelo menu durante a captura de 420 s, com screenshots a cada 5 s. **Mapa por presentation id:**
- ~1800–2290: cutscene inicial (close no Logan), ~7 FPS;
- **2299: helicóptero ("PRESENT DAY")**;
- ~2410: salto;
- ~2429: pouso;
- ~2590: combate na floresta.

Janela principal `--start-flip 2299 --end-flip 2429` (`heli.json`, `waits-heli.json`); janela longa 2299–2589 (`long.json`). O cache Vulkan de pipelines está desativado ("dirty build"), portanto a execução é fria. Isso vale igualmente para A e B.

Janela do helicóptero, 105,8 s, 130 apresentações: **1,23 FPS**, mediana 233 ms, **p95 3.799 ms** (stalls longos). Por apresentação:

- **Thread GPU ocupada 661 ms** (idle 96, bloqueada em label 43). Dentro do tempo ocupado, o que já está identificado: espera de pipeline 80,5, compilação de shader 45 (3,4 compilações), waits de semáforo 50, `queue.submit` 24,7 e comandos síncronos 13,5. **Cerca de 450 ms do tempo ocupado ainda não têm atribuição**; este é o próximo diagnóstico.
- Submits 1.838: label 1.496, processor 181, finish 88, drain 73. O flush por label custa ~25 ms de 813 ms médios (~3%). Ganho limitado nesta cena; na tela de menu é ~20%.
- **Unmap: 87 por apresentação**, cada um faz um `Finish()` completo. `finish_unmap` soma 26 ms na thread GPU, e a thread guest espera 40,7 ms em `command_sync`. É um candidato concreto: esperar só o necessário para o intervalo desmapeado.
- Threads guest esperando readback: 64,5 ms; readback 24,8 MB e upload 27,1 MB.
- **Churn: imagens 48,4 e scratch do tiler 71,4 por apresentação**; buffers só 1,9 criações e 0,3 joins. O arredondamento de buffers para 1 MiB **não tem alvo nesta cena** (`vma_buffer_create` 0,19 ms/apresentação). O equivalente aqui é churn de imagens e do tiler.

Conclusão do gate da Fase 1: nesta cena a espera **não** domina. O maior bloco é trabalho de CPU na thread GPU sem atribuição (~450 ms/frame), mais compilação/pipeline (~125 ms) e stalls. Próximo passo: perfilar a thread GPU nessa janela (Tracy, com zonas largas que já existem nos handlers PM4/descriptors/pipeline, ou timers por categoria: texture cache/detile, descriptors, draw, upload). Depois, A/B do flush de label (`KYTY_LABEL_FLUSH_EVERY`, já implementado no fonte, ainda não compilado) e do unmap.

### ~~Primeiro ganho comprovado~~ RETIRADO: flush de label por fatia — [WOLVERINE_LABELFLUSH_REPORT.md](WOLVERINE_LABELFLUSH_REPORT.md)

Por sugestão do usuário, a tela de menu (~12,7 FPS, já ruim, automática) virou cena de A/B. `KYTY_LABEL_FLUSH_EVERY=0` (opt-in; padrão inalterado): **12,71 → 15,44 FPS (+21,5%), mediana 82,4 → 66,6 ms**, em 2 referências contra 2 candidatas no mesmo binário (`labelflush-ab.exe`, `5DDCC3E4…`), com variação de ~1%. Submits 1.287 → ~214 por frame. Sem travamento; screenshots confirmam a cena.

Harness: `capture.ps1 -Environment 'KYTY_X=Y'` registra a env no manifesto. Comparação: `compare_ab.py` (scratchpad da sessão, modo `tail N`). O `install/kyty_emulator.exe` estável continua intacto. Nada commitado.

Próximo: validar a candidata na cutscene do helicóptero e em gameplay (o usuário navega), depois decidir se vira padrão. Em seguida atacar o unmap com `Finish()` completo (87/frame na cutscene) e o trabalho sem atribuição na thread GPU (~450 ms/frame na cutscene; churn de imagens/tiler 48/71 por frame).

**Correção (mesma sessão): o resultado acima foi retirado.** Com `KYTY_LABEL_FLUSH_EVERY=0` o menu aparece sobre **fundo preto**, enquanto a referência renderiza a floresta atrás da caixa "SCREEN READER". É regressão visual; a carga caiu porque o jogo deixou de desenhar a cena. **Não há ganho de FPS comprovado.** Hipótese: o jogo usa o momento em que o label chega (fence CPU) para decidir renderizar ou carregar o fundo, e o atraso até o fim da fatia muda essa decisão. Lição para o harness: todo A/B precisa de screenshot dos dois lados, comparado antes de aceitar números. Próximo passo possível: medir N limitado (ex. `KYTY_LABEL_FLUSH_EVERY=4/16`) com screenshots dos dois lados, ou identificar quais labels o CPU guest consome e adiar só os demais.

### Opção 2 (labels por consumidor) — refutada

Detalhes em [WOLVERINE_LABELFLUSH_REPORT.md](WOLVERINE_LABELFLUSH_REPORT.md). Em resumo: ~970 de ~1.075 labels/frame no menu não têm consumidor visível ao emulador, e adiar qualquer classe (inclusive só esses) quebra o render (fundo preto ou tela preta). Substituir o submit por barreira global + fim de render pass **não** restaura a imagem, então não é barreira acidental. **Sem ganho de FPS comprovado.** Referência visual correta do menu: `benchmarks/labelmode-ref/screenshots/t0176s-p002210.png`. Flags novas, todas opt-in e sem efeito por padrão: `KYTY_LABEL_FLUSH_EVERY`, `KYTY_LABEL_FLUSH_KEEP`, `KYTY_LABEL_DEFER_MODE`, `KYTY_READBACK_BATCH`, `KYTY_SHADER_CSE`.

Regra para o harness daqui em diante: todo A/B tem screenshot dos dois lados, comparado antes de aceitar qualquer número.

### Atribuição da thread GPU por categoria (exclusiva) — menu, `benchmarks/cat-menu-01/`

Binário `benchmarks/cat.exe` (`BB286241…`), comportamento padrão (sem flags experimentais), imagem verificada (Wolverine na floresta, `screenshots/t0181s-p002276.png`). Janela: últimos 40 s. Tempo exclusivo por frame (`waits.py --table`; um escopo aninhado desconta do externo). Soma 80,4 ms contra ~82 ms/frame medidos.

| Categoria (thread GPU) | ms/frame | chamadas/frame | µs/chamada |
|---|---:|---:|---:|
| wait (semáforo/cv/label bloqueados) | 27,0 | 220 | 123 |
| submit (`vkQueueSubmit` + fim do command buffer) | 17,5 | 1.285 | 13,6 |
| **dispatch (compute)** | **12,5** | **1.328** | 9,4 |
| draw | 8,3 | 201 | 41 |
| pm4 (decode/restante) | 5,8 | 235 | 25 |
| buffer cache | 5,0 | 13.847 | 0,4 |
| bindings/descriptors | 2,1 | 9.394 | 0,2 |
| image cache | 1,5 | 5.971 | 0,3 |
| tiler / gc / pipeline | 0,5 | | |

Leitura (fato medido, interpretação marcada):
- **Esperas somam 27 ms:** a thread GPU bloqueada em label 13,9 (todas as filas suspensas esperando publicação), drains de readback ~9,8, publish ~3,3. Interpretação: o GPU executa ~1.070 command buffers minúsculos por frame em série e a cadeia de dependência dá latência, não throughput. A 4090 ficava a ~38% de uso na amostra do Codex.
- **O jogo faz 1.328 dispatches e ~1.073 labels por frame no menu.** Quase um label por dispatch. É o padrão de um engine com muitos passes de compute pequenos.
- CPU de emulação em si (decode, draw, bindings, caches) soma só ~23 ms; o resto é submit + espera.
- Cutscene do helicóptero (referência sem categorias): ~450 ms/frame de trabalho não atribuído; ainda não medido com categorias.

Próximo: (1) medir a mesma atribuição na cutscene (requer navegação do usuário); (2) abrir `dispatch`: 1.328/frame × 9,4 µs; verificar quantos são meta-clears/clears de HTILE ou dispatches triviais que poderiam ser fundidos; (3) a espera de ~24 ms só cede com menos viagens CPU↔GPU, e a tentativa de adiar labels quebrou o render.

### Automação do controle e achado: o piso por frame é fixo — `benchmarks/cat-heli-01/`

**Controle automático:** no emulador `J` = X (cruz) e `L` = O. `scratchpad/heli-auto.ps1` (cópia em `disp-auto.ps1`) lança a captura, espera `presentation_id ≥ 1900`, traz a janela para frente (Alt + SetForegroundWindow) e envia J uma vez via `keybd_event`. Funcionou: a caixa SCREEN READER passou para o menu **INITIAL SETUP** ("New Game" selecionado), onde o jogo fica parado. O X precisa de ~1 s de foco da janela.

**Achado:** o menu preto de texto (INITIAL SETUP) e o menu com cena 3D (SCREEN READER) têm carga **idêntica** por frame: ~1.329 dispatches, ~201 draws, ~1.075 labels, ~1.288 submits, 12,4 FPS (mediana 82,8 ms). A atribuição por categoria é igual nos dois (wait 27,0; submit 17,6; dispatch 12,5; draw 8,3; pm4 5,8; buffer cache 5,0). **O custo é um piso fixo por frame, independente da cena**, que serve de teste: INITIAL SETUP é estático, preto e fácil de comparar por imagem. Em jogo a cena soma por cima.

Novo: `KYTY_PERF_DISPATCH_CSV=<arquivo>` agrega dispatches de `DispatchDirect` por (shader, grupos, resultado: executado, meta-clear, image-clear, sem programa) com contagem e tempo; reescrito a cada segundo. Binário `benchmarks/disp.exe`.

Nota de ferramenta: os escapes `\` das minhas chamadas são colapsados antes de chegar ao shell/Python; para texto com barra invertida (strings C, caminhos) usar a ferramenta Edit, não scripts Python/sed. Isso produziu três vezes NUL/quebras de linha indevidas em arquivos C++; todas foram detectadas e corrigidas.

### Thread de submit assíncrona (`KYTY_ASYNC_SUBMIT=1`) — resultado parcial

Implementação: `CommandScheduler::SubmitThread`, fila FIFO, ticks atribuídos em ordem sob `m_submit_mutex`, submits com semáforos externos permanecem síncronos e antes esperam a fila esvaziar (`DrainSubmitQueue`); thread criada sob demanda e unida no `Shutdown`. Desligada por padrão. Binário `benchmarks/async.exe` (`64328FFB…`). Testes: CTest 29/29 com a flag desligada e 7/7 nos de scheduler/GPU com a flag ligada.

A/B no menu INITIAL SETUP (X automático no frame ~1900), últimos 40 s, alternando (`benchmarks/async2-{ref1,async1,ref2,async2}`):

| Execução | FPS | Mediana | p95 | p99 | `submit` na thread GPU | `wait` na thread GPU |
|---|---:|---:|---:|---:|---:|---:|
| ref1 | 12,40 | 82,8 | 84,6 | 100,6 | 17,6 ms | 27,1 ms |
| async1 | 13,81 | 66,9 | 85,0 | 85,6 | 0,7 ms | 35,3 ms |
| ref2 | 12,43 | 82,9 | 84,4 | 100,3 | 17,6 ms | 27,0 ms |
| async2 | 12,77 | 82,4 | 101,3 | 116,1 | 0,8 ms | 37,0 ms |

Média ref 12,41 contra async 13,29 FPS (**+7%**), mas com variação grande entre as duas execuções async (13,81 e 12,77), enquanto as referências variam 0,2%. O custo de `submit` some da thread GPU (17,6 → 0,7 ms), mas o `wait` sobe ~8–10 ms: parte do ganho vira espera. **Ganho modesto e instável; ainda não comprovado.** O queue_submit continua ~16–17 ms por frame, agora na thread nova.

**Limite do método descoberto:** a referência também mostra a caixa SCREEN READER sobre **fundo preto** no instante do X (frame ~1906); a floresta 3D só aparece mais tarde (na referência entre ~1900 e ~2210). Portanto o X automático em 1900 **não exercita a fase com a cena 3D**, onde a tentativa de adiar labels falhava (preto até o frame 4144). A validação visual do async nessa fase está em `benchmarks/async3-forest` (sem X).

### ALERTA metodológico: cache de shaders do driver confunde os A/B

Em `cat-menu-01` (primeira execução do binário `cat.exe`) a thread GPU gastou **76 s dentro de `PipelineCache`** (51,9 s só no travamento de 58 s em torno do frame 1601, sem nenhuma apresentação). Em execuções seguintes do mesmo binário (`async2-ref1`, `async3-forest`) a categoria pipeline soma ~0,5 s e o travamento **não existe**. O cache de pipelines do Kyty está desativado ("dirty build"), então a explicação mais provável é o cache de shaders **do driver NVIDIA**, que parece ser por nome de executável: todos os meus binários de teste tinham nomes novos (`labelflush-ab.exe`, `labelclass.exe`, `labelmode.exe`, `cat.exe`, `disp.exe`, `async.exe`). A primeira execução de cada nome é fria (stall de ~50 s); as demais são quentes. (Hipótese ainda não confirmada.)

Consequência: execuções frias mostram a floresta atrás da caixa SCREEN READER (o stall dá tempo de carregar); as quentes mostram fundo preto. Os casos "fundo preto" que atribuí a `KEEP`, `DEFER_MODE`, label flush e async **podem ser só cache quente**. Pendente: referência quente sem flags e sem X (`benchmarks/warm-ref-forest`). Se ela também ficar preta, a regressão visual atribuída às minhas mudanças não é comprovada, e o ganho de 12,7 → 15,4 FPS do `KYTY_LABEL_FLUSH_EVERY=0` (A2 quente contra B2/B3 quentes) volta a ser candidato.

### A/B limpo (quente contra quente, mesmo menu, mesma imagem) — `benchmarks/warm-*`

Controle que desfez a conclusão visual anterior: `warm-ref-forest` (referência quente, **sem flags**, sem X) também mostra a caixa sobre fundo preto aos 240 s. O fundo preto vem do cache quente, não das flags. A floresta aparece só com cache frio (stall de ~58 s em compilação de pipelines).

Mesmo executável `async.exe` (já quente) em todas as execuções; só a env muda; X automático após ≥85 s de relógio e frame ≥1900; janela = últimos 40 s no menu **INITIAL SETUP**, imagem confirmada por screenshot nos dois lados. As duas primeiras `flush` (`flush1/2`) foram excluídas: o X foi cedo demais e elas mediram a caixa, não o INITIAL SETUP.

| Variante | FPS (2 execuções) | Média | Mediana de frame | Submits/frame |
|---|---|---:|---:|---:|
| Referência (padrão) | 12,36 / 12,29 | **12,33** | 83,0 ms | 1.290 |
| `KYTY_LABEL_FLUSH_EVERY=0` | 15,01 / 15,18 | **15,09 (+22,4%)** | 66,6 ms | ~214 |
| `KYTY_ASYNC_SUBMIT=1` | 13,85 / 13,81 | **13,83 (+12,2%)** | 66,8 ms | 1.290 |

A variação entre repetições é ≤1,3% em todas, bem abaixo dos ganhos. Primeiros ganhos medidos de forma controlada neste trabalho, **no menu**. Ainda não provados: progresso do jogo além do menu com cada variante (rodando `warm-flush-story`), cutscene do helicóptero, gameplay e outros jogos. As duas flags continuam desligadas por padrão.

Mecanismo: a mediana cai para ~66,7 ms nas duas. O `flush` elimina ~1.075 submits/frame (queue_submit 15,7 → ~3,4 ms). O `async` não reduz o número de submits, só tira o custo da thread GPU, mas o `wait` na thread GPU sobe de 27 para ~35 ms, o que limita o ganho.

**Verificação funcional:** com `KYTY_LABEL_FLUSH_EVERY=0` (`benchmarks/warm-flush-story`) o jogo chega à tela de título com a floresta 3D correta (idêntica à da referência) e fica estável por 330 s. Fluxo observado com o X automático: caixa SCREEN READER → X → (INITIAL SETUP em algumas execuções; tela de título "PRESS X TO START" nesta). Pendente: seguir até a cutscene do helicóptero e gameplay com a flag ligada.

Estado do código: sem commit; flags novas todas opt-in (`KYTY_LABEL_FLUSH_EVERY`, `KYTY_ASYNC_SUBMIT`, `KYTY_LABEL_FLUSH_KEEP`, `KYTY_LABEL_DEFER_MODE`, `KYTY_READBACK_BATCH`, `KYTY_SHADER_CSE`). `install/kyty_emulator.exe` estável intacto (`BA74C6D3…`); binário com tudo: `benchmarks/async.exe` (`64328FFB…`).

## Sessão 2026-10-06 (continuação): fork Demon's Souls, boot direto e limpeza

### Fork `chenxiao07/KytyPS5` (Demon's Souls) — o que serve aqui
Skill do fork: branch `docs/ai-optimization-skill`, `.claude/skills/ps5-emu-optimization/` (SKILL.md, references/lessons.md, measurement.md). O código é específico do Demon's Souls; o aproveitável é o método (sonda de causalidade com atraso injetado, limite superior antes de implementar, A/B no mesmo processo com switch ao vivo) e técnicas genéricas (submit/gravação Vulkan em worker, pipelines sem otimização primeiro com `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`, readback esperando só o tick do último escritor, dedupe de barreiras, page tracking mais barato). Números do fork (21 para 40 FPS, RTX 5090) são outra máquina e outro jogo: hipótese para o Wolverine, não resultado. Verificado no nosso fonte: não existe `DISABLE_OPTIMIZATION` (grep sem resultado); compilação de pipelines em worker já existe (`41c7b27b`).

### Boot direto no jogo — resultado NEGATIVO (testado)
- `commandline.txt` na pasta do jogo (`-level levels/i33/i33.level`, `-lobby`, `-legal`, cvars `{ Grupo.Campo=valor }`) é **ignorado** por este build: sem `-legal` nada mudou; com `-level levels/zzz/zzz.level` (inexistente) o jogo seguiu normal até a cena de abertura (floresta) sem erro; com `EngineDebug.FrameTimeGraphEnable=true` e `GPUBudgetInfoEnable=true` nenhum gráfico apareceu (até a tela SCREEN READER). Limite: o log do emulador não registra leituras de arquivo, então não se provou que o arquivo nem é aberto; só que não tem efeito observável. Original restaurado e conferido (SHA-256 `e57a79e7…`, backup `commandline.txt.orig` na pasta do jogo).
- Save: nenhuma pasta `_SaveData` surge depois de passar pelo INITIAL SETUP; a abertura é cena sem checkpoint, então o INITIAL SETUP continua em toda execução. Não se sabe se o jogo chama `SaveDataMount3`/`Commit` (não há log). Caminho aberto, não testado: jogar até o primeiro autosave com log em `libSaveData.cpp`.
- Cena fixa disponível: depois do X (tecla J) no INITIAL SETUP o jogo chega sozinho à abertura (floresta) em ~2 min, a 4 a 7 FPS; `ab-auto.ps1` automatiza.
- Armadilha: `Copy-Item` com caminho contendo `[ ]` trata como curinga e não copia (falha silenciosa); usar `cp` no bash.

### Limpeza
- Backup antes: `G:\kity_work\backup-pre-cleanup\` (`tracked-changes.patch`, `untracked-sources.tar`, docs, hashes dos exes mantidos).
- Removidos: executáveis de teste (mantidos `kyty_emulator.exe` estável SHA `BA74C6D3…`, `async.exe` SHA `64328FFB…`, `launcher.exe` e `benchmarks/pre-diagnostics-1e2185db.exe`), logs soltos sem citação, capturas de testes descartados (`cmd-*`, `cvar-graph`, `save-run1`, `labelkeep-*`), `benchmark.exe` das pastas `renderer-*`, e screenshots das capturas pesadas (`waitsites-heli-ref-01`, `warm-flush-story` ficaram com 3 cada). `_Build/windows/benchmarks`: 903 MB para ~201 MB. Mantidas as pastas citadas nos relatórios (os relatórios citam `warm-*`, `async2-*` e `labelflush-menu-*` por prefixo).
- Código removido (sem ganho comprovado): FIFO de readback (`SendReadbackSync`, `QueuedCommand`, teste de ordenação do `gpu_command_lane`, métricas `readback_queued/queue_depth/tail_eligible/joined`) e CSE de shader (`CommonSubexpressionElimination.*`, `ShaderCseTests.cpp`, `shader_cse_tests`, métricas `shader_cse_*`, `ShaderRecompiler.cpp` de volta ao HEAD). `bufferCache.cpp` voltou a chamar `SendCommandSync`. Flags `KYTY_READBACK_BATCH` e `KYTY_SHADER_CSE` não existem mais. Relatórios FIFO e CSE marcados como arquivados.
- Mantidos: `performanceDiagnostics.*`, `KYTY_LABEL_FLUSH_EVERY/KEEP`, `KYTY_LABEL_DEFER_MODE`, `KYTY_ASYNC_SUBMIT`, harness `tools/performance/`. Commit do HEAD `1e2185db` (de outro autor) intocado. Sem commit nem push.
- Validação: build Release de `kyty_emulator` e dos 57 alvos `*_tests`; CTest serial 53/54 aprovado. `kernel_file_system` falha (`APR rejects concatenated paths exceeding PATH_MAX`, código `src/libs` e teste sem modificação no git; não investigado, não causado por esta limpeza até onde se sabe). Com `-j4` dois testes GPU falharam por contenção do dispositivo e passam isolados: rodar CTest em série. Testes Python (`test_analyze`, `test_waits`, `test_renderer`) OK. O binário rebuilt em `_Build/windows/kyty_emulator.exe` NÃO foi copiado para `install/` (o estável continua BA74C6D3).

### Próximo passo (plano aprovado, Parte B)
B0 sonda de causalidade + switch ao vivo; B1 pipelines sem otimização primeiro; B2 reconfirmar submit assíncrono na floresta; B3 readback esperando só o tick do último escritor; B4/B5/B6 conforme a sonda. Uma hipótese por A/B, 3 execuções por lado, cache quente, mesma cena.

### B0 concluído: sonda de causalidade + switch ao vivo (menu INITIAL SETUP, cache quente)
Implementado em `performanceDiagnostics.cpp`: `KYTY_PERF_DELAY_<CATEGORIA>_NS` (início) e `KYTY_PERF_CONTROL=<arquivo>` com linhas `delay_<categoria>_ns=N`, relido a cada 100 ms pela thread amostradora (o CSV continua a 1 s); a espera ocupada acontece na entrada da categoria na thread GPU, só com diagnósticos ligados. Roteiro `probe-auto.ps1` (scratchpad): um único processo, X automático, 25 s de aquecimento, depois fases A/B/A/B/A/B de 20 s (A sem atraso, B com atraso), id de apresentação de cada troca em `phases.csv`; janelas por flip com 15 flips aparados nas bordas. `async.exe` agora é este build (o anterior, SHA `64328FFB…`, ficou em `benchmarks/async-before-probe.exe`).

`probe-dispatch-01` (cena confirmada por screenshot: INITIAL SETUP), atraso de 1500 ns por entrada na categoria `dispatch`:

| Fase | FPS | média de frame | dispatch ms/frame (medido) |
|---|---:|---:|---:|
| A0 | 12,57 | 79,6 ms | 12,17 |
| B1 | 12,33 | 81,1 ms | 13,87 |
| A2 | 12,74 | 78,5 ms | 11,98 |
| B3 | 12,28 | 81,4 ms | 13,58 |
| A4 | 12,74 | 78,5 ms | 11,89 |
| B5 | 12,36 | 80,9 ms | ~13,6 |

Injetado ≈ +1,7 ms/frame em dispatch (≈1.280 entradas/frame × 1,5 µs); frame time +1,5 / +2,9 / +2,4 ms (média ≈ +2,3 ms; FPS médio 12,68 contra 12,32, -2,8%). **Inclinação ≈ 1** (1,0 a 1,7 com ruído): **tempo de dispatch na thread GPU está no caminho crítico** neste menu, então reduzir o custo por dispatch deve render ~1:1 em ms de frame. Limite: 3 pares A/B em 1 execução, mediana quase não muda (+0,3 a +0,7 ms) enquanto a média sobe; a inclinação tem incerteza de uns ±0,5. Só vale para o menu; falta repetir em floresta/helicóptero e nas outras categorias (draw, wait, submit).

### B0 (continuação): sonda em draw, submit e wait — `probe-multi-01` (menu INITIAL SETUP, 1 processo, A/B/A/B/A/B/A)
Atrasos: draw 10 µs/entrada, submit 1,5 µs, wait 9 µs. Cada B comparado com a média das duas fases A vizinhas (frame médio dos A: 79,1 / 78,9 / 79,2 / 79,1 ms, estáveis).

| Categoria | injetado (ms/frame) | frame time (ms) | inclinação |
|---|---:|---:|---:|
| dispatch (execução anterior) | ~1,7 | +2,3 | ~1,3 |
| draw | 1,95 | +2,44 | 1,25 |
| submit | 2,22 | +1,72 | 0,77 |
| wait | 1,14 | +1,13 | 0,99 |

Todas ≈ 1 (0,8 a 1,3; incerteza ≈ ±0,5 com 1 par por categoria). Leitura: **a thread GPU única é o caminho crítico inteiro**; draw, dispatch, submit e wait somam em série, e cada ms tirado de qualquer um deve aparecer ~1:1 no frame (os ganhos medidos de `LABEL_FLUSH_EVERY=0`, que removeu ~12 ms de submit/frame e deu -16,4 ms de frame, são consistentes com isso, e o do `ASYNC_SUBMIT` também). Cuidado: a inclinação do **wait** diz que tempo extra antes de um wait soma, não que remover o wait economize o tempo todo (parte do wait é a GPU de fato trabalhando). Limite: só menu; 1 par por categoria nesta execução.

### B1 concluído: pipelines sem otimização NÃO ajudam; cache de pipelines do próprio Kyty reduz muito o boot frio
Nome do executável novo = cache do driver NVIDIA frio (cada `cold*.exe` é cópia do mesmo build; só env muda). Cena: menu INITIAL SETUP, X automático.

1. **Hipótese do fork (`VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT`) refutada aqui.** Flag opt-in `KYTY_PIPELINE_NO_OPT=1` (`pipeline/shaders.cpp`, grafico e compute). `cold-ref` x `cold-noopt`: mesmo stall de 58,4 s no frame ~1630 e mesmo FPS (12,68). Com log de compilações lentas (≥200 ms, `SlowPipeline:` em stderr): `cold-slowlog2` 80 criações, **86,1 s no total**; `cold-noopt2` 81 criações, **86,7 s**; a maior é uma única `vkCreateComputePipelines` de ~21,5 s. A flag não reduziu nada (driver NVIDIA atual, esses compute shaders). Os stalls são compilações de **compute** feitas no thread GPU (o graphics já vai para worker, `41c7b27b`).
2. **Cache de pipelines do Kyty (`_PipelineCache/PPSA03671.bin`) estava desligado por "dirty build"** (`pipelineCache.cpp:815`). Novo opt-in `KYTY_PIPELINE_CACHE_ALLOW_DIRTY=1` ignora só essa trava. Salva no fechamento normal da janela (`Save()`); com `Stop-Process -Force` nada é salvo (por isso `ab-exe.ps1` ganhou `-GracefulAfter`, que chama `CloseMainWindow`).

| Execução | compilações lentas | tempo até a apresentação 1900 | stalls >2 s | FPS final (menu) |
|---|---:|---:|---|---:|
| `pc-run1` (cache vazio, popula e salva 28,7 MB) | 82, 87,1 s | 147 s | 5,8 s, 17,0 s, 5,9 s, **58,7 s** | 12,71 |
| `pc-run2` (nome de exe novo, cache do Kyty carregado) | **0**, 0 s | **71 s** | 5,9 s, 6,6 s | 12,72 |

O cache do driver NVIDIA estava frio nas duas (nomes novos); o ganho vem do `VkPipelineCache` do Kyty. **Boot frio ~76 s mais curto neste menu e sem o stall de 58 s; FPS estável igual** (esperado: é custo de início, não de steady state). Limites: 1 execução por lado; o stall de 6,6 s restante em ~1633 não foi investigado; o benefício é de primeira execução/exe novo (com o exe já quente o cache do driver faz o mesmo). A trava de "dirty" existe provavelmente para não reutilizar cache entre builds diferentes; a assinatura inclui `KYTY_GIT_REVISION` e o driver só reaproveita entradas cujo SPIR-V bate por hash, então reutilizar em build dirty parece seguro, mas isso é inferência, não teste. Decisão sobre tornar isso padrão fica com o usuário.
Observação: apaguei o conteúdo anterior de `install/_PipelineCache` (113 MB, regenerável) ao preparar a medição fria; agora contém só o `.bin` de 28,7 MB gerado em `pc-run1`.
Arquivos tocados: `pipeline/shaders.cpp` (NO_OPT + log lento), `pipeline/pipelineCache.cpp` (ALLOW_DIRTY).

### B2 concluído: flags reconfirmadas com cache de pipelines do Kyty; FPS é quantizado em vblanks (`b2-*`)
`async.exe` = build atual, `KYTY_PIPELINE_CACHE_ALLOW_DIRTY=1` em todas (boot mais curto), X automático, janela = últimos 40 s no menu INITIAL SETUP, 2 execuções por variante intercaladas (ref, flush, async, both, ref, ...).

| Variante | FPS (2 execuções) | média | vs ref | mediana de frame | submits/frame |
|---|---|---:|---:|---:|---:|
| ref | 12,86 / 12,90 | 12,88 | - | 82,4 ms | ~1.230 |
| `KYTY_LABEL_FLUSH_EVERY=0` | 15,73 / 15,80 | 15,76 | **+22,4%** | 66,5 ms | ~206 |
| `KYTY_ASYNC_SUBMIT=1` | 14,18 / 14,12 | 14,15 | **+9,9%** | 66,7 ms | ~1.245 |
| as duas | 15,50 / 15,59 | 15,54 | **+20,7%** | 66,6 ms | ~207 |

Repete o resultado anterior (+22,4% e +12,2% antes; async agora +9,9%, dentro da variação de build/cena). **Não se somam**: as duas juntas ficam abaixo do flush sozinho (-1,4% relativo, mesma ordem da variação entre execuções ≈ 0,5%; diferença pequena mas consistente nas 2 rodadas).

**Achado novo: o intervalo de apresentação é quantizado em vblanks de 16,67 ms (60 Hz).** Histograma dos intervalos (últimos 40 s): ref = 4 vblanks (183) e 5 (322); flush = 3 (159) e 4 (435); both = 3 (132) e 4 (443). A mediana 66,5 ms = 4 vblanks, 82,4 = 5. Consequências: (1) FPS e mediana andam em degraus de 16,7 ms; a média de frame é a medida contínua; (2) async e flush chegam ao mesmo patamar (4 vblanks) por motivos diferentes, e ficar abaixo disso exige cruzar 3 vblanks (50 ms) com folga, por isso ganhos menores podem não aparecer na mediana, só na proporção de frames de 3 vblanks (flush 159 de 628 contra 0 em ref); (3) vale comparar a distribuição, não só o FPS.

GPU thread no ref (ms/frame, últimos 40 s): `read_gpu_wait` 10,6, `wait_gpu_label_blocked` 12,7, `wait_readback_*` (cpu_read 3,4 + cpu_write 3,0 + gpu_read 2,6) ≈ 9, ~70 pedidos de readback/frame. `wait_priority_ops` 43 e `wait_command_sync` 41 ms/frame são esperas de threads do jogo/guest, não da thread GPU.

### B3 (teto, não implementação): esperas de readback na thread GPU — `ubb-*`
Interruptor INSEGURO só para medir: `KYTY_READBACK_UPPER_BOUND=1` (`cache/bufferCache.cpp`) pula `Wait(tick)` e `WaitPriorityOperations(tick)` do readback da CPU (mantém o bookkeeping de dirty; a 1ª versão, que também pulava o unmark, abortava com "CPU dirty state conflicts with GPU dirty state"). A GPU não é esperada, então a CPU pode ler dado velho. **Esta variante aborta por volta de 40 s** (`Memory: required direct-backing write failed, addr=0x00000801da6e0000 size=0x120000`, `kernel/memory.cpp:949`): sem chegar ao menu, o A/B do menu não existe para ela. Comparei a janela comum da abertura (ids de apresentação, mesma cena de vídeo/quadrinhos nas 4 execuções; `async.exe`, `KYTY_PIPELINE_CACHE_ALLOW_DIRTY=1`):

| ids | ref | UB | flush | flush+UB |
|---|---:|---:|---:|---:|
| 700 a 1200 (vídeo) | 41,8 FPS (23,9 ms) | 43,0 (23,2) | 59,8 (16,7) | 59,9 (16,7) |
| 1200 a 1500 | 29,4 (34,1) | 43,1 (23,2) | 36,5 (27,4) | **59,8 (16,7)** |

Menu, ref e flush sem UB (últimos 40 s): 12,80 FPS (78,1 ms, `read_gpu_wait` 10,8 ms/frame) e 14,52 (68,9 ms, 10,9 ms/frame). Leitura: (1) `LABEL_FLUSH_EVERY=0` sozinho leva o vídeo de abertura de 41,8 para 59,8 FPS, o limite do vblank de 60 Hz (+43%); (2) o teto de pular a espera do readback é grande na janela 1200 a 1500 (flush 27,4 para 16,7 ms, ≈ 10,7 ms, igual aos ~10,9 ms/frame de `read_gpu_wait`: inclinação ≈ 1); no vídeo (700 a 1200) não há ganho porque o limite de 60 Hz já foi atingido. (3) O teto é de uma versão **incorreta** (dado velho, aborta): serve só como limite superior, não prova que a versão segura recupera isso. A versão segura (esperar só o tick do último escritor dos bytes lidos, exige registrar o tick de escrita GPU por intervalo de buffer e testes de coerência) ficou **não implementada**: é a única etapa da Parte B com risco de corromper memória do guest, por isso fica para decisão do usuário.
Aviso: `capture.ps1` recusa pasta de saída que já existe (a execução falha em 2 s sem erro visível); usar nome novo.

### B3 seguro: readback na segunda fila (`KYTY_READBACK_QUEUE=1`) — implementado, ganho pequeno
Medição de viabilidade (`fresh-*`): em ~98% dos downloads de readback o último escritor GPU já tinha terminado (`IsFree`) no momento do pedido; mesmo assim cada um esperava a cauda (média 146 ticks sem flush, 64 com). Implementação: segunda `vk::Queue` da mesma família (`GraphicContext::readback_queue`, `queue_count`), anel de 1024 escritas GPU com tick (`BufferCache::LastGpuWriteTick`, piso conservador no overflow), `Buffer::created_tick` (um join copia o conteúdo antigo no tick de criação), e `TryDownloadInline` (só na thread GPU, sem download antigo em voo sobre os mesmos bytes, escritor e criação já concluídos): copia na 2ª fila, espera o fence (500 µs de espera ativa, depois bloqueante), publica no guest na hora e pula `Wait(tick)`. O `UnmarkRegionAsGpuModified` continua obrigatório também no caminho inline (a 1ª versão o pulava e abortava com "CPU dirty state conflicts with GPU dirty state").
Resultados (menu, `async.exe`, cache de pipelines do Kyty; 2 execuções por variante, nenhuma falha):

| Variante | FPS | inline/frame | `read_gpu_wait` ms/frame | espera inline ms/frame |
|---|---|---:|---:|---:|
| ref | 12,83 / 12,75 | 0 | 10,6 | 0 |
| fila | 12,92 / 12,95 | 68,4 | 1,4 | 8,1 |
| flush | 13,24 / 14,43 e 14,46 / 15,58 | 0 | 10,9 | 0 |
| flush + fila | 15,59 / 14,43 e 15,88 / 15,92 | ~69 | 1,8 | 7,1 |

Fila sozinha: +1,1% sobre ref (dentro do ruído). Flush + fila contra flush (4 execuções cada, todas): 15,45 contra 14,43 FPS (+7%), com sobreposição de faixas (flush 13,2 a 15,6; fq 14,4 a 15,9); direção consistente, tamanho incerto. A espera ativa do fence não mudou a espera inline (7,1 contra 7,2 ms/frame): o custo de ~0,1 ms por readback é a ida e volta de submit+fence, não o despertar da thread. Conclusão: a espera da GPU saiu da thread (10,6 para ~1,5 ms/frame) mas foi trocada por ~7 a 8 ms/frame de latência de fila; ganho líquido ~+1 a 3 ms/frame. Para chegar perto do teto de ~10,7 ms seria preciso lotar vários readbacks por submit (os pedidos são sequenciais por thread do jogo; o agrupamento FIFO antigo não rendeu), então a estimativa de ganho adicional é incerta.

### Teste do `levels/zzz` com 1 X (resolve a dúvida de 2026-10-06)
`zzz-onepress`: com `-level levels/zzz/zzz.level` e um X, o jogo parou no INITIAL SETUP, igual ao arquivo original; a observação anterior (floresta com 1 X) não se repetiu. O `commandline.txt` continua sem efeito observável; arquivo restaurado (SHA `e57a79e7…`).

### Padrões novos (build dirty)
- `KYTY_LABEL_FLUSH_EVERY` agora vale 0 quando não definida (rótulo só no fim da fatia PM4); `=1` restaura o submit por rótulo.
- Cache de pipelines do Kyty passa a valer também em build "dirty"; `KYTY_PIPELINE_CACHE_NO_DIRTY=1` restaura a recusa.
- CTest serial após a mudança: 53/54 (só `kernel_file_system`, já falhava).

### Verificação funcional na tela de título (2 X) com os novos padrões — `fo-*`
Cena: depois dos 2 X o jogo chega à tela de título "MARVEL WOLVERINE / PRESS X TO START" (floresta ao fundo), não à cutscene de abertura; mesma cena nas 3 execuções, imagens idênticas a olho nos screenshots de 256 s (sem artefato). Janela: últimos 40 s; 1 execução por variante; nenhum `--- Error ---` nos 3 logs.

| Variante | FPS | vs ref |
|---|---:|---:|
| ref (`KYTY_LABEL_FLUSH_EVERY=1`, padrão antigo) | 9,93 | - |
| novo padrão (flush=0 + cache de pipelines) | 11,08 | +11,6% |
| novo padrão + `KYTY_READBACK_QUEUE=1` (38,8 readbacks inline/frame) | 11,25 | +13,3% (+1,5% sobre o padrão novo) |

Limites: 1 execução por variante, só tela de título; a fila de readback ainda não foi testada em gameplay nem em cutscene longa, então **continua opt-in**. Não testados: helicóptero, gameplay, outros jogos. Nada commitado; `kyty_emulator.exe` estável de `install/` intocado (SHA `BA74C6D3…`); `async.exe` é o build atual.
Resumo do que está no código (sem commit): `KYTY_LABEL_FLUSH_EVERY` default 0; cache de pipelines em build dirty (`KYTY_PIPELINE_CACHE_NO_DIRTY=1` desliga); opt-in: `KYTY_ASYNC_SUBMIT=1`, `KYTY_READBACK_QUEUE=1`, `KYTY_PIPELINE_NO_OPT=1` (sem efeito medido), `KYTY_READBACK_UPPER_BOUND=1` (INSEGURO, só medição), `KYTY_PERF_DELAY_<CATEGORIA>_NS` e `KYTY_PERF_CONTROL` (sonda de causalidade).

### Esperas em labels da thread GPU: causa e correção (`KYTY_LABEL_PENDING_VISIBLE`)
**Descoberta que reinterpreta as medições anteriores:** `tools/performance/capture.ps1` liga `KYTY_LABELS_AFTER_GPU=1` em TODA captura (linha 83), então a referência de ~12,8 FPS no menu, os ~1.230 submits/frame e os +22% do flush valem para esse modo de pesquisa (labels só chegam à memória quando a GPU conclui o tick). Sem a flag, os labels são escritos na hora da gravação e não há submit por label nem este bloqueio. Quem roda sem a flag não vê esses números.

Diagnóstico (`KYTY_PERF_LABEL_CSV`, `RecordLabelSuspend/Writer/BlockedNs` em `performanceDiagnostics.cpp`; `labels-menu*.csv`): em ~140 s de menu, 507.849 suspensões de `WAIT_REG_MEM` (~150/frame) em 10.207 endereços, ~29,6 s de thread GPU parada com todas as filas bloqueadas (~20% do tempo). **100% dos endereços esperados têm escritor PM4** (fila 0 espera fila 0: 71% do bloqueio; fila 32 espera fila 0: 26%; fila 0 espera fila 32: 3%); nenhum é escrito só pela CPU do jogo. Causa: em `KYTY_LABELS_AFTER_GPU` o valor só chega à memória na conclusão do tick (thread de completion), mas a GPU já o gravou antes de qualquer comando posterior da mesma fila Vulkan, então a espera é uma sincronização que a ordem da fila garante.

Correção: `GuestGpu::AddPendingLabel/ErasePendingLabel` + `PublishLabelAtCompletion` registra o label como pendente (visível só ao `ReadLabel` da thread GPU) ao gravar e o apaga depois da publicação; a memória do guest continua recebendo o valor só na conclusão (regra "não publicar antes da GPU" intacta). Timestamps (clock) não participam. Ativa por padrão; `KYTY_LABEL_PENDING_VISIBLE=0` desliga.

`pv-*` (menu, 2 execuções por lado, `async.exe`): ref 15,76 / 15,78 FPS, `wait_gpu_label_blocked` 15,6 ms/frame, `wait` total 27,8 ms; pendente-visível 18,01 / 17,95 FPS, 0,0 ms/frame, `wait` ~15 ms: **+14,0% (15,77 para 17,98 FPS)**, repetição de ~0,1 FPS. Tela de título (2 X, `pv-title`): 12,49 FPS contra 11,08 do novo padrão (+12,7%) e 9,93 do padrão antigo (+25,8% no total), cena e imagem corretas, sem `--- Error ---`.
Limites: só menu e tela de título; riscos teóricos: se a CPU do jogo sobrescrever um label antes da GPU publicá-lo, a thread GPU ainda veria o valor pendente (o mesmo comportamento já aceito em `KYTY_LABELS_AT_COMPLETION`); sem teste em gameplay, cutscene longa ou outros jogos. CTest serial: 53/54 (`kernel_file_system`, já falhava).

### Cutscene do helicóptero (3 X automáticos) — `heli-*`, padrão antigo contra o atual
Automação: `ab-exe.ps1 -SecondPressDelay 25 -ThirdPressDelay 40` (J no diálogo SCREEN READER, em "New Game" e em "PRESS X TO START"); depois do 3º X o jogo entra na cutscene do helicóptero (Wolverine sentado, Sabretooth, queda), a 2 a 5 FPS. `old` = `KYTY_LABEL_FLUSH_EVERY=1` + `KYTY_LABEL_PENDING_VISIBLE=0` (como era antes de hoje, ainda com `KYTY_LABELS_AFTER_GPU=1` do capture); `new` = padrões atuais (flush 0, pendente visível, cache de pipelines do Kyty em build dirty). `async.exe`, 2 execuções por variante intercaladas, 330 s cada, nenhum `--- Error ---`.

A cutscene NÃO avança por tempo real: no mesmo instante a variante `new` está bem mais adiante (screenshot a 285 s: old no quadro 3515 com a fala "We don't need you, runt", new no quadro 4462 em outro plano). Por isso janelas de tempo real comparam conteúdo diferente; usei janelas por número de apresentações depois do 3º X (suposição, não verificada quadro a quadro: o conteúdo depende do número de quadros apresentados):

| quadros após o 3º X | old (FPS) | new (FPS) | diferença |
|---|---:|---:|---:|
| 0 a 300 | 6,96 / 6,50 (média 6,73) | 8,74 / 8,70 (8,72) | +29,5% |
| 300 a 600 | 5,22 / 4,85 (5,03) | 7,82 / 8,70 (8,26) | +64,2% |
| 600 a 900 | só old-2: 5,22 | 5,43 / 5,30 (5,37) | +2,7% (1 amostra do old, não conclusivo) |

`wait_gpu_label_blocked`: 18 a 31 ms/frame em old contra 0,0 em new. Em tempo real (janelas iguais de relógio após o 3º X) o new ficou +19% e +34% à frente, mas esse número mistura trechos diferentes da cutscene, por isso não o uso. Limites: 2 execuções por lado; 3ª janela fraca; não verifiquei se há diferença visual quadro a quadro além de olhar screenshots avulsos (sem artefatos aparentes); gameplay após a cutscene não foi medido.
