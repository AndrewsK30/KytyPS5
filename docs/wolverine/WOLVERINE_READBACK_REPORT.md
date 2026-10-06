# Wolverine — origem dos readbacks e teste de granularidade

Data: 2026-10-05. Branch local: `wolverine-performance`, HEAD `1e2185dba7c6a13662e2fb208131950fa33020de`. Nenhum commit ou push nesta sessão.

## Resultado do diagnóstico

A nova instrumentação separa transferências de buffers pela origem do pedido: leitura/escrita da CPU guest, leitura/escrita solicitada pela thread GPU, materialização de metadados e garbage collection. A origem é capturada antes de `SendCommandSync` e aplicada dentro do callback; atribuir tudo à thread que executa o download esconderia os pedidos da CPU.

Os waits agora distinguem thread GPU, worker de completion e outras threads. Tempos de host podem se sobrepor e são contabilizados quando o escopo termina. Waits do worker de completion não são automaticamente tempo bloqueado da thread GPU.

RELEASE_MEM também distingue dword, qword, timestamp, GDS e nenhum dado. Não há dump do conteúdo da memória do jogo.

## Referência com janela original de 512 KiB

Artefatos: `KytyPS5/_Build/windows/benchmarks/readback-origin-01/`. Abertura automática, sem input, 180 segundos; análise `late.json` entre 120,059 e 179,081 segundos, duração 59,022 segundos. Configuração fixa do harness; batching de interrupções desligado. Cache de driver desativado pelo build dirty, portanto não chamar o trecho de cache aquecido.

| Medida | Referência |
| --- | ---: |
| Apresentações / flips completos | 346 / 346 |
| Taxa de apresentações | 5,862 FPS |
| Mediana / p95 / p99 | 133,474 / 313,230 / 646,178 ms |
| Submits do renderer | 500.374 |
| Submits por apresentação | 1.446,17 |
| Readback total de buffers | 18,137 GB |
| Leitura solicitada pela CPU | 8,458 GB |
| Preservação antes de escrita da CPU | 7,700 GB |
| Leitura solicitada pela thread GPU | 1,971 GB |
| Preservação antes de escrita solicitada pela thread GPU | 0,009 GB |
| Metadados / GC / origem desconhecida | 0 / 0 / 0 |
| Pedidos CPU de leitura / escrita | 28.304 / 40.823 |
| Waits na thread GPU / completion | 9,411 / 25,543 s |
| RELEASE_MEM qword / dword / timestamp | 365.633 / 19.505 / 12.302 |

GB significa 10^9 bytes. Os contadores de download incluem padding de empacotamento; não medem diretamente tráfego PCIe. A soma das origens fecha com o total nesta janela. O tamanho de um pedido originado por fault pode ser 1 byte: não representa a largura da instrução.

Conclusões comprovadas nesta referência: pedidos da CPU dominam o download de buffers; GC e metadados não explicam esse volume; grande parte dos waits antes agregados ocorre no worker assíncrono. Compilação (84 ocorrências, 0,667 s de host) e atualização de título (0,023 s) não explicam sozinhas o desempenho observado.

## Experimento: preservar somente as páginas antes de escrita

`ReadMemory(..., is_write=true)` foi alterado experimentalmente para baixar as páginas de tracker afetadas (4 KiB, incluindo todas as páginas de um pedido que atravesse fronteiras), mantendo a janela de leitura em 512 KiB. A correção preservava os bytes GPU da página antes de torná-la gravável pela CPU e mantinha páginas externas sob propriedade GPU.

O teste real `buffer_cache_ranges` passou com essa política: valores na mesma página preservados, vizinho externo ainda GPU-owned, escrita CPU reenviada sem destruir valores preservados. Build Release passou. Essa validação de coerência não implica melhora de desempenho.

Captura experimental: `KytyPS5/_Build/windows/benchmarks/write-page-candidate-01/`. O manifesto, patch e fontes não rastreadas preservam a versão exata do experimento. As capturas `window-mid.png` e `window-heavy.png` identificam respectivamente o logo Marvel e Wolverine no trecho pesado; são capturas de tela e podem conter sobreposição da janela do terminal.

Os primeiros números altos ocorreram no logo, não no trecho pesado. A análise final `late.json` inclui 120,060–179,089 segundos (59,029 segundos):

| Medida | Referência 512 KiB | Tentativa 4 KiB |
| --- | ---: | ---: |
| Apresentações / flips completos | 346 / 346 | 103 / 103 |
| Taxa de apresentações | 5,862 FPS | 1,745 FPS |
| Mediana de intervalo | 133,474 ms | 567,976 ms |
| Readback de buffers | 18,137 GB | 5,777 GB |
| Readback por apresentação | 52,419 MB | 56,085 MB |
| Pedidos CPU de escrita | 40.823 | 530.286 |
| Pedidos CPU de escrita por apresentação | 117,99 | 5.148,41 |
| Submits por apresentação | 1.446,17 | 6.326,11 |
| Wait na thread GPU | 9,411 s | 31,953 s |

Esta tabela é exploratória: os intervalos cobrem progressões diferentes da abertura. A redução do volume por segundo acompanha a queda do número de apresentações, e o volume por apresentação não caiu. A transferência média por pedido CPU de escrita caiu de aproximadamente 188.620 para 3.928 bytes, mas vieram muito mais pedidos e sincronizações. O código de faults e a preservação de páginas dão suporte à hipótese de perda de coalescing; não há ainda trace causal de cada fault.

A tentativa foi rejeitada e retirada do código e do executável instalado. A janela original foi restaurada e os testes passaram novamente. O teste de coerência pré-escrita foi mantido, agora validando que os dados externos à janela de 512 KiB continuam GPU-owned. O experimento original permanece no patch da captura e em `benchmarks/write-page-rejected.exe`; a referência foi preservada em `benchmarks/readback-origin-baseline.exe`.

Validação final: build Release de emulador/diagnóstico/teste GPU, três testes CTest (`performance_diagnostics`, `performance_diagnostics_disabled`, `buffer_cache_ranges`), teste Python do analisador e `git diff --check`, todos aprovados. SHA256 do executável instalado restaurado: `1992C862E48FA0EC8130E1F5F45222C3765C5A6CA6AD0805850C796505B16000`. Ambos os processos de captura terminaram; nenhuma janela do emulador ficou ativa.

## Limites e continuação

As duas execuções avançam de maneira diferente pela abertura. Uma janela de tempo desde o lançamento não garante a mesma cena ou workload. Não anunciar porcentagem de ganho/regressão como benchmark controlado; a tentativa não mostrou benefício e deve ser rejeitada. Falta cenário repetível e três pares A/B de pelo menos 60 segundos, além da avaliação de overhead da coleta e de outros jogos.

A próxima correção precisa reduzir o número de sincronizações mantendo coalescing e coerência. Antes de implementá-la, registrar amostras limitadas dos intervalos/endereço do pedido, tick produtor/consumidor, tamanho efetivamente sujo baixado e quantidade de cópias. Procurar reutilização de uma mesma versão e submits que já possam ser satisfeitos por um tick existente. Não adiar labels sem identificar os consumidores nem publicar dados antes de a GPU concluir.
