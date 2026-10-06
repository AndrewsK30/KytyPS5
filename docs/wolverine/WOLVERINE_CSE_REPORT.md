> **ARQUIVADO (2026-10-06): candidata descartada, sem ganho comprovado. O código foi removido do fonte (backup em `backup-pre-cleanup/`); este relatório fica só como registro.**

# Experimento: CSE inteiro no compilador de shaders

## Origem e hipótese

Autorizado pelo usuário em 2026-10-05: adaptar uma melhoria concreta do RPCS3 para verificar sua utilidade no Kyty. O RPCS3 usa `EarlyCSEPass(true)` na pipeline LLVM do recompilador SPU. Fonte oficial consultada e fixada na revisão `3e7477107a356c5d1fae92a7dc3c8d5953984782`:

https://github.com/RPCS3/rpcs3/blob/3e7477107a356c5d1fae92a7dc3c8d5953984782/rpcs3/Emu/Cell/SPULLVMRecompiler.cpp

Adaptação do princípio de eliminação de expressões comuns; não foi transplantado código LLVM/SPU nem implementado o EarlyCSE completo. No Kyty, o alvo é a IR dos shaders que depois gera SPIR-V. Cálculos inteiros idênticos podem reutilizar uma definição já executada. Isso reduz trabalho no programa traduzido, mas o driver Vulkan pode já eliminar essas duplicatas. O efeito sobre FPS depende de custo GPU e do caminho crítico; não existe ganho garantido.

## Implementação

- Novos `CommonSubexpressionElimination.h/.cpp` em `src/graphics/shader/recompiler/ir/passes/`.
- Tabela de expressões por bloco, chave exata opcode + todos os 64 bits de flags + operandos resolvidos. Colisões de hash são verificadas por igualdade completa. A tabela é reiniciada entre blocos.
- Lista explícita de operações puras: soma/subtração/multiplicação inteiras, shifts, operações bitwise, bit fields, select U32 e construção/extração de compostos inteiros.
- Sem leituras de memória, recursos, atomics, floats, relógios, phis ou estado de execução/subgrupo. Não há reordenação nem transformação algébrica/associativa. Nenhum valor é substituído com base apenas em `HasSideEffects`.
- Aplicada depois de especialização dos recursos e remoção das dependências de retenção, antes de coletar informações e emitir SPIR-V. Os alvos indiretos retidos nos metadados dos blocos são excluídos. Condições U1 não estão na lista de operações candidatas.
- Remoção de identities/dead code depois das substituições, usando as passes existentes.
- Contadores CSV: `shader_cse_candidates`, `shader_cse_eliminated`, `shader_cse_ns`. Contam operações estáticas e tempo de compilação host; não são instruções por frame nem timestamps GPU.

## Validação funcional

Build Release aprovado. Novo teste `shader_cse`: comparação de resultados inteiros para 10.000 entradas com overflow U32, dependências encadeadas e limpeza de identities/DCE; isolamento entre blocos, flags e operandos diferentes; retenção de metadados; exclusão de leituras antes/depois de escrita, clocks e estado EXEC. O teste inicial conservava a saída através de um pseudo-register write removível pelo DCE; o fixture foi corrigido para `ReferenceU32`, que é a retenção adequada na IR. Não foi um problema de resultado aritmético da pass.

CTest selecionado 7/7 aprovado: shader_cse, diagnósticos on/off, memory_tracker, GPU command lane, buffer cache ranges, alignbyte GPU. Suíte completa `shader_recompiler_compute` aprovada em 19,11 s. Logs: `_Build/windows/shader-cse-build.log`, `shader-cse-test-build.log`, `shader-cse-compute-tests.log`.

## Capturas e isolamento

Capturas de 180 s usam o harness habitual com GPU 0 e configuração anterior. A candidata FIFO anterior permanece igual nos dois binários deste A/B; portanto não comparar este experimento diretamente com a instalação estável pré-FIFO como se CSE fosse a única diferença. Binários recebem nomes separados; `install/kyty_emulator.exe` permanece na versão estável registrada na sessão.

A candidata está em `benchmarks/shader-cse-candidate.exe`, executada por `install/shader-cse-candidate.exe`. Captura: `benchmarks/shader-cse-candidate-01/`. A referência desta experiência será compilada do mesmo fonte, omitindo somente a chamada CSE no compilador, e terá patch exato preservado pelo harness.

As duas capturas terminaram, sem build concorrente. As janelas por tempo de startup continuam exploratórias: não garantem cena equivalente nem autorizam alegação de ganho percentual de FPS. JSONs `late.json` e `full.json` estão em cada pasta de captura; patches e fontes novos também foram preservados pelo harness.

Primeira captura concluída: no processo inteiro, 531.604 expressões candidatas e 10.445 eliminadas (1,965% **das expressões inteiras elegíveis**, não de todas as instruções GPU). A pass consumiu 71,775 ms acumulados de CPU host em uma captura de 180 s. Esse é o efeito estático comprovado; não permite calcular quanto trabalho dinâmico GPU foi poupado.

Na janela exploratória 120–180 s, a candidata teve 4,896 apresentações/s, mediana 116,958 ms, p95 243,638 ms e p99 2.197,727 ms, com 110 compilações. Mediana e taxa média divergem devido aos stalls. O trecho ainda contém compilação e não representa uma referência aquecida/estável de gameplay.

Mesmo nessa tentativa, foram 391.021 submits do renderer para 289 apresentações (1.353 por apresentação), 12,053 GB de readback e 20,560 s agregados de despacho de readback na janela final. A CSE não altera essa sincronização. Esses números descrevem a janela, não demonstram que o custo predominante seja GPU ou CPU; faltam timestamps GPU e uma cena repetível.

O objetivo desta implementação é testar uma técnica transferível, medir sua aplicação real e preservar uma candidata com testes. O fato de o RPCS3 usar CSE não transfere o ganho do recompilador SPU para o shader Vulkan de um PS5. Uma economia estática de ~2% num subconjunto das operações não sustenta a expectativa de multiplicar FPS por quatro.

### Comparação exploratória final (120–180 s)

| Medida | Referência sem CSE | Candidata com CSE |
| --- | ---: | ---: |
| Intervalo efetivo | 120,063–179,088 s | 120,065–179,092 s |
| Apresentações / flips | 260 / 260 | 289 / 289 |
| Taxa média | 4,405 FPS | 4,896 FPS |
| Mediana entre apresentações | 117,502 ms | 116,958 ms |
| p95 | 251,986 ms | 243,638 ms |
| p99 | 2.413,598 ms | 2.197,727 ms |
| Compilações | 119 | 110 |
| Submits renderer | 358.431 | 391.021 |

Não interpretar o quociente de taxas médias como ganho causado pela pass: carregamento, quantidade de compilações e progresso da cena não são idênticos. A mediana praticamente igual e os longos stalls indicam que não foi demonstrada solução para a lentidão. A redução de expressões estáticas é comprovada; o ganho de FPS continua **não comprovado**. Não houve crash nas capturas, mas isso não substitui comparação visual/correção em cenas e jogos adicionais.

## Estado final e continuidade

- Chamada CSE restaurada no fonte depois de compilar/capturar a referência. Build Release final aprovado; fonte e binário na raiz `_Build/windows/` são candidatos com CSE e FIFO.
- Instalação `install/kyty_emulator.exe` continua na versão estável pré-FIFO: SHA256 `BA74C6D3EB379B2EBE981AEA36502A3DDEC99D8F6018241EC780D638832DFCCC`. Não instalar a candidata como melhoria de FPS aprovada.
- Candidata preservada: `benchmarks/shader-cse-candidate.exe`, SHA256 `0F4D64DF4A9E4BBE7C274927199DBDDF50411037407BF4C82016B52C08C4A2EF`.
- Referência isolada preservada: `benchmarks/shader-cse-control.exe`, SHA256 `E30D0139D86ABB2FF9DC09A103EE9A05DA57F8B01CA0A843EAB60FB8E5640424`.
- Logs adicionais: `shader-cse-control-build.log`, `shader-cse-restored-build.log`. Nenhum commit/push. Capturas encerradas pelo harness ao completar 180 s.
- Python analisador 2/2 e `git diff --check` aprovados. Nenhum emulador ativo ao finalizar. Fonte do compilador restaurado é idêntico ao snapshot da candidata testada. Binário raiz após relink final: SHA256 `F81BC625BA4A47F99FAE6C96630D585C80FF0992011AEE0632F6B938B68664BF`; o binário exato capturado continua preservado separadamente.

Próximo passo recomendado: medir duração GPU de submits/draw/dispatch com queries Vulkan e custo CPU por frame em cena repetível, antes de ampliar a pass ou aplicar LICM/DSE inspirados no RPCS3. Precisamos separar carregamento/compilação dos ~mil submits por apresentação e provar onde se gastam os milissegundos. Não remover leituras ou waits de memória com CSE: isso exigiria uma análise de alias/coerência totalmente diferente. Outros jogos e três pares A/B de gameplay ainda estão pendentes.

Nenhum commit ou push; nenhuma alteração publicada no upstream ou na branch do autor da PR.
