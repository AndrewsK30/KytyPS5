# Primeira entrega: instrumentação e captura nativa

Data: 2026-10-05. Branch local: `wolverine-performance`, HEAD `1e2185db`, instrumentação ainda sem commit/push.

## Entrega

- Contadores separados por thread, snapshots por apresentação e por segundo, incluindo períodos sem novas apresentações.
- Medição de submits, queue mutex/API, waits Vulkan, labels, barreiras globais, transferências e compilação de shader/pipeline.
- Flips GPU concluídos contados separadamente de apresentações. Não há identificação exata de ownership de cada counter por frame guest nesta versão.
- Harness: `KytyPS5/tools/performance/capture.ps1`, manifesto com SHA/hash do binário, fontes, configuração e logs. Restaurar ambiente do chamador e parar apenas o processo criado.
- Analisador: `KytyPS5/tools/performance/analyze.py`, seleção explícita de intervalo, percentis e totais sem contar heartbeats como frames.
- Corrigido o install prefix no cache CMake para `_Build/windows/install`; Git colocado no PATH do processo de build para evitar tentativa de baixar FFmpeg com SHA vazio. Não foi alterado o PATH global do Windows.

## Validação

- Build Release de `kyty_emulator`: passou.
- CTest: `performance_diagnostics` e `performance_diagnostics_disabled`: 2/2 passaram. O teste concorrente verificou 40.000 eventos e 2.560.000 bytes de quatro threads, sem perda ou contagem dupla, e amostragem sem novos frames.
- Teste Python: verificou seleção de janela, exclusão de deltas parciais, contagem de apresentações e percentis: passou.
- Sintaxe PowerShell: passou. Captura real de aproximadamente três minutos finalizou e encerrou seu próprio processo.

## Captura exploratória

Diretório: `KytyPS5/_Build/windows/benchmarks/diagnostics-opening-01`.
Relatório: `report-120-180.json`. CSV: `renderer.csv`.
Binário SHA256: `435C42C25952F6DCBE7BF4B2B357EFE830C9305A3A4A893241941707673A04B6`.

Execução desde a abertura, sem input. Não corresponde a um save/câmera de gameplay estabelecido. Configuração compatível com Wolverine; interrupt-only batching desativado. O log confirmou cache Vulkan desativado por dirty build. A próxima comparação deve preservar esse estado ou usar builds limpas comparáveis.

Intervalo efetivo selecionado: 120,055–179,082 s desde início da coleta, duração de 59,027 s.

| Métrica | Valor |
| --- | --- |
| Apresentações e flips GPU concluídos | 371 de cada |
| Taxa de apresentação | 6,285/s |
| Frame interval mediano / p95 / p99 | 133,457 / 249,954 / 565,315 ms |
| Submits do renderer | 531.619 (~1.433 por apresentação) |
| RELEASE_MEM / flush por label | 423.408 de cada |
| Flushes do caminho interrupt-only | 0 |
| Barreiras globais | 355.375 |
| Readback de buffers / imagens | 20,737 GB / 0,373 GB (decimal) |
| Upload de buffers | 11,532 GB |
| Tempo host agregado de queue.submit | 6,946 s |
| Tempo host agregado de espera de semáforo | 37,426 s |
| Compilações de shader no caminho medido | 63, somando 0,606 s |
| Espera inicial de pipelines gráficos | 0,132 s |
| Atualização do título | 0,026 s |

Tempos host de várias threads podem se sobrepor. Os 37,426 s de espera não equivalem a 37,426 s perdidos na thread de GPU. Scopes abertos são contabilizados quando terminam. Readback contabiliza bytes programados nesses caminhos de cópia, não necessariamente bytes físicos de PCIe. Compilação medida começa em CompilePermutation, não inclui todo o preparo/tradução anterior.

## Decisão para a próxima implementação

1. Classificar waits por thread e consumidor: thread GPU, completion callbacks, fault handlers do guest e wrap de stream buffers.
2. Contar readbacks por origem e região: leituras de labels, argumentos indiretos, SRT/BDA, conteúdo de recursos, coerência imagem/buffer e pressão de cache. Registrar endereço/intervalo apenas em amostras limitadas, sem dump de dados do jogo.
3. Distinguir tipos de RELEASE_MEM, especialmente timestamps vs labels de memória, e correlacionar produtor/tick com o consumidor da leitura.
4. Medir submits consecutivos redundantes. Somente eliminar um envio se demonstrado que não contém comandos necessários, callbacks ou dependências de semaphore/tick.
5. Escolher a primeira correção onde houver dependência comprovada: reduzir janela de readback ou remover envio redundante, preservando coerência e progresso. Adicionar o teste funcional dessa dependência e fazer A/B no mesmo trecho.

O interrupt-only batching não é prioridade neste cenário: seu caminho medido teve zero flushes. A hipótese de título da janela também tem baixo peso nesta captura. O custo elevado de submissões e transferências está comprovado; sua participação exata no caminho crítico ainda requer a classificação por thread.

## Pendências antes de anunciar ganho

- Medir overhead da instrumentação habilitada/desabilitada no mesmo cenário. A coleta foi projetada para evitar contenção global por evento, mas seu custo percentual ainda não foi estabelecido.
- Fixar um cenário visual repetível e obter três execuções da referência e do candidato.
- Completar correlação por IDs de flip/tick e timestamps GPU amostrados para separar fila sem trabalho, execução GPU e espera host.
- Registrar hardware ao longo da captura; o manifesto atual só contém uma amostra inicial de NVIDIA-SMI.
- Validar outros jogos disponíveis após a primeira correção funcional.

Esta entrega prepara e valida diagnóstico; não altera semântica de renderização e não demonstra melhoria de FPS.
