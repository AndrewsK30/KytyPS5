# Benchmark 3D repetível do renderer Kyty

## Resultado entregue

Implementado e executado um teste de integração 3D offscreen, sem iniciar Wolverine ou passar por filmes. Cena fixa de 64 cubos com perspectiva e depth test, 512×512, câmera/geometria determinísticas. A saída é verificada por cobertura e checksum idêntico entre os três perfis e entre repetições. É uma base para testes de carga e regressão do renderer; não é um executável/demo PS5 completo.

Arquivos no repositório `G:/kity_work/KytyPS5`:

- `tests/RendererBenchmark.inc`: implementação usando `VulkanHarness`, renderer e scheduler existentes.
- `tests/ShaderRecompilerComputeTests.cpp`: entrada `--renderer-benchmark OUTPUT_DIR FRAMES DRAWS` e vertex fixture que aceita posição homogênea 3D. O comportamento padrão dos testes anteriores foi preservado.
- `tests/RunRendererBenchmark.cmake` e CTest `renderer_benchmark_smoke`: renderização, igualdade de imagem, três perfis e nove frames medidos; falha se não houver os recursos GPU necessários.
- `tools/performance/renderer.ps1`: captura repetida, manifest, hash/binário/DLL preservados, patch/fontes exatos, timeout, restauração dos envs de diagnóstico.
- `tools/performance/renderer.py`: valida e analisa distribuições por frame, resume repetições e exporta PNG da imagem renderizada.
- `tools/performance/test_renderer.py`: rejeita frames incompletos/duplicados, NaN, contagens erradas, imagens diferentes e batches com workload/hardware misturados.

## Cargas e cobertura

| Perfil | Trabalho por frame |
| --- | --- |
| `raster` | Todos os draws em um submit, preparação/commit de bindings, aquisição dos targets, hot lookup no pipeline cache e depth test. |
| `submits` | Mesma geometria/resultado, um submit a cada quatro draws; permite aumentar a pressão no scheduler. |
| `readback` | Mesmos draws em um submit, depois download de 4 MiB RGBA32F pelo anel `BufferCache::GetUtilityBuffer(MemoryUsage::Download)`, invalidação e cópia host. |

São 36 vértices/12 triângulos por draw. Contagem múltipla de 64 mantém a cena inteira: 256 draws repetem a grade quatro vezes; 4.096 draws a repetem 64 vezes. As repetições criam overdraw determinístico, não objetos diferentes. Geometry/projeção/upload e compilação ficam fora dos frames medidos. Cada perfil aquece por pelo menos 16 frames **e dois segundos** antes da medição. Um frame em voo, sem swapchain/vsync.

Percorre o compiler RDNA2/IR/SPIR-V do pixel shader, o pipeline cache, a preparação de descriptors, targets/depth, scheduler e o anel de download de produção. O vertex shader é uma fixture SPIR-V nativa com geometria pré-projetada na CPU. O comando final draw é gravado pelo fixture através de Vulkan; não percorre o decode PM4/DrawAuto completo. Não cobre execução CPU guest, APIs PS5, fault handlers CPU de memória, `SendReadbackSync`, labels/interrupções, produtor dos recursos, tessellation, texturas complexas ou compute pesado de Wolverine. As flags experimentais desses caminhos não são avaliadas por esta carga.

## Medição e interpretação

CSV separa `cpu_record_ms`, `cpu_submit_ms`, `cpu_wait_ms`, `cpu_readback_ms`, `host_frame_ms`, `gpu_span_ms`, `gpu_readback_ms`, número de submits render e bytes baixados. São queries timestamp Vulkan reais, convertidas com `timestampPeriod` e máscara `timestampValidBits`, lidas após conclusão do submit. Referência oficial: https://docs.vulkan.org/spec/latest/chapters/queries.html#queries-timestamps

**`gpu_span_ms` entre vários submits inclui períodos ociosos da fila enquanto a CPU produz os próximos comandos. Não chamar esse intervalo de tempo GPU ocupado.** Wait CPU se sobrepõe à GPU; não somar CPU+GPU. Readback host inclui reserva do anel, gravação/cópia, submit adicional, wait, invalidação e cópia para vetor. GPU readback mede o intervalo dessa transferência/barreiras. Query readback, escrita CSV, checksum e PNG estão fora de `host_frame_ms`. Não apresentar 1000/tempo como FPS de um jogo.

## Referência validada

Captura canônica: `KytyPS5/_Build/windows/benchmarks/renderer-production-01/`, workload **`cube-grid-v2`**. Três processos/repetições, 180 frames medidos por perfil, 256 draws/frame. Total 1.620 frames medidos. GPU RTX 4090, CPU desta máquina Ryzen 7 7800X3D. Binário Release SHA256 `72B962D12667CE83F831E1970FD3B0AD511676B970E361D45159A2C230212187`, preservado como `benchmark.exe` com DLL. Arquivamento após captura confirmado contra hash do manifest em `binary-archive.json`.

`batch-summary.json` apresenta a mediana das medianas das três execuções, e faixa das medianas individuais:

| Perfil | Host frame mediano | Faixa entre repetições | GPU span mediano |
| --- | ---: | ---: | ---: |
| raster | 0,4845 ms | 0,4833–0,4952 ms | 0,0428 ms |
| submits (64/frame) | 1,3417 ms | 1,3290–1,3562 ms | 1,2795 ms, inclui gaps |
| readback (4 MiB/frame) | 2,2551 ms | 2,2249–2,3377 ms | 0,0517 ms de render |

No perfil readback, mediana da fase host de download 1,6607 ms, intervalo GPU de download 0,6302 ms. No perfil submits, gravação CPU 0,4199 ms e submit CPU 0,8738 ms. Essas fases já têm papéis definidos; não são evidência de que a mesma proporção exista no jogo.

Todas as nove imagens de perfil/repetição tiveram checksum U32 FNV-1a64 `11224440966537881514`, 65.090 pixels cobertos. Preview: `renderer-production-01/run-01/scene.png`; resultado float original é usado para checksum, preview usa conversão RGB/gamma apenas para inspeção.

Carga pesada concluída em `renderer-stress-01/`: 4.096 draws/frame, **1.024 submits/frame** no perfil submits, três repetições de 60 frames/perfil (540 frames medidos). Mesmo binário/hash e mesma imagem. Medianas host das três execuções: raster **7,073 ms** (faixa 7,044–7,074), submits **22,214 ms** (21,832–23,069), readback **9,562 ms** (9,434–9,588). O intervalo GPU entre os 1.024 submits foi ~22,143 ms, incluindo gaps de produção CPU; a rasterização em um submit foi ~0,695 ms. A carga de submits mostra maior variação e p95, que devem entrar na comparação de futuras mudanças.

Essa diferença mede uma mudança deliberada na carga/submissão do fixture, não uma otimização do emulador. Permite investigar custo por draw/submission sem repetir a abertura. Não demonstra que todos os 143 ms por frame de Wolverine sejam submits: o jogo inclui outros caminhos ausentes desta cena.

## Uso nas próximas sessões

Na raiz do repositório:

```powershell
# Recompilar antes da captura. Git precisa estar no PATH do processo.
$env:Path = 'C:\Program Files\Git\cmd;' + $env:Path
& 'C:\Program Files\CMake\bin\cmake.exe' --build _Build/windows --config Release --target shader_recompiler_compute_tests --parallel 8

# Captura padrão (cria diretório novo automaticamente).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/performance/renderer.ps1

# Carga de submits próxima à ordem de grandeza observada no jogo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/performance/renderer.ps1 -Frames 60 -Draws 4096 -Runs 3

# Teste de integração rápido, sem limiar de FPS sujeito a ruído.
& 'C:\Program Files\CMake\bin\ctest.exe' --test-dir _Build/windows -C Release -R '^renderer_benchmark_smoke$' --output-on-failure
```

Para A/B, preservar o binário A, capturar A, mudar apenas a hipótese necessária, recompilar B e capturar B com mesma versão/workload, GPU/driver, draws, frames e condições da máquina. Não fazer build/captura de jogo/outro teste GPU em paralelo. Comparar medianas, p95/p99 e a variação entre repetições, mantendo o checksum. Mudanças menores que o ruído não demonstram melhoria. Se mudar a carga/política, atualizar versão e não comparar como se só o emulador tivesse mudado. O analisador de batch rejeita hardware/workloads misturados dentro do batch.

Aceitar uma melhoria deste teste só na área efetivamente exercitada. Depois, validar no jogo a hipótese escolhida em uma cena repetível; o benchmark reduz a necessidade de rodar a abertura a cada mudança, mas não substitui a validação de Wolverine.

## Validação e tentativas preservadas

Build Release aprovado. CTest 8/8 aprovado: benchmark smoke, suíte GPU completa, CSE, diagnostics on/off, memory tracker, command lane e buffer cache ranges. Python 5/5 aprovado (três testes renderer, dois anteriores). Sintaxe PowerShell aprovada. Logs `renderer-benchmark-build.log`, `renderer-benchmark-v2-build.log`, `renderer-benchmark-config-build.log` e `renderer-benchmark-tests.log`.

Primeiro smoke `renderer-smoke-01/` usava aquecimento apenas por frames. `renderer-fixed-01/` preserva uma execução válida do primeiro protótipo, cujo wrapper perdeu o ExitCode no Windows PowerShell; o handle foi mantido aberto e o problema corrigido. `renderer-fixed-02/` guarda três execuções **v1** com staging auxiliar host-visible. Esse staging levou a ~19,7 ms no perfil readback e foi substituído pela política/anéis reais de produção. Essa redução entre v1/v2 é uma correção do fixture de benchmark, **não melhoria do emulador ou de Wolverine**. Não reutilizar os números v1 como referência canônica de readback.

Nenhum commit/push. `install/kyty_emulator.exe` permanece estável pré-FIFO, SHA256 `BA74C6D3EB379B2EBE981AEA36502A3DDEC99D8F6018241EC780D638832DFCCC`. O benchmark está no executável de testes; não foi colocado no fluxo do jogo.
