# Wolverine — despacho e pedidos de readback pendentes

Data: 2026-10-05. Continuação autorizada após [WOLVERINE_READBACK_REPORT.md](WOLVERINE_READBACK_REPORT.md). Branch `wolverine-performance`; HEAD permanece `1e2185dba7c6a13662e2fb208131950fa33020de`. Sem commit/push.

## Implementação mantida

O CSV agregado separa despacho (início do pedido até entrada do callback GPU), preparo (lookup/registro de cópias, incluindo eventuais waits internos do download ring), espera do scheduler e espera da publicação pelas priority operations. Também conta pedidos com/sem download. São tempos de host, podem se sobrepor e não devem ser somados como custo exclusivo de frame.

`KYTY_PERF_READBACK_CSV` habilita amostras limitadas: 1 em 64 pedidos por thread solicitante, no máximo 20.000 linhas por processo. O harness configura `readbacks.csv` junto com `renderer.csv`; `-DisableDiagnostics` desliga ambos. Sem conteúdos do jogo: somente intervalos/endereço, limites do buffer/janela, ticks do scheduler, fases, bytes efetivos/padded e número de cópias. Não há loop adicional por cópia quando não há amostra ativa.

Origem e dados da amostra atravessam `SendCommandSync`. Um escopo TLS na thread GPU atribui as cópias à amostra correta; o retorno síncrono garante a vida útil do registro no caller. O escritor usa o mutex existente apenas nos pedidos amostrados. O teste concorrente verifica isolamento/restauração de TLS, frequência por thread, tamanhos, flush e limite de linhas. O analisador `tools/performance/readbacks.py` agrupa as amostras por origem/buffer/janela e documenta que a soma de amostras não é o total da execução.

Ticks são os ticks do scheduler de download, **não** a versão do último produtor. Endereços iguais ou ticks consecutivos não autorizam reutilizar valores. Isso ainda requer rastrear dependências do escritor.

## Evidência nova

Captura original com agrupamento 512 KiB: `KytyPS5/_Build/windows/benchmarks/readback-samples-01/`. Janela efetiva do `late.json`: 120,016–179,091 s, duração 59,074 s.

| Medida agregada | Valor |
| --- | ---: |
| Presents / flips | 403 / 403 |
| Taxa de apresentações exploratória | 6,822 FPS |
| Pedidos com / sem download | 54.896 / 35.191 |
| Despacho | 20,864 s |
| Preparo | 0,620 s |
| Espera do scheduler no readback | 10,853 s |
| Espera da publicação | 2,742 s |
| CPU sync agregado | 32,697 s |
| Downloads de buffers | 23,451 GB |

Em `samples-late.json`, 741 amostras CPU pre-write incluem 444 sem cópia (59,9%). Mediana de despacho 0,1874 ms, mediana de pedido completo 0,2594 ms. As leituras CPU tiveram 590 amostras, 105 sem cópia. Houve também 81 amostras GPU-read, todas com cópia. A amostra parcial anterior, entre 60 s e o final disponível naquele momento, tinha 515/685 CPU pre-write sem cópia (75,2%); não confundir essa amostra parcial com os 59,9% da janela final.

Pedidos sem cópia continuam pagando o despacho para a thread GPU. Isso justifica investigar redundância entre pedidos pendentes; não prova que o dado estava disponível antes de entrar na fila.

## Tentativa testada e rejeitada: rechecagem antes de enviar

Foi inserida uma segunda checagem para CPU pre-write antes de `SendCommandSync`, reutilizando `MemoryTracker::InvalidateRegion`. O lock do tracker combina o teste de propriedade GPU e a marcação CPU, preservando a mesma disciplina de concorrência já usada na invalidação. Se qualquer parte continuar GPU-owned, o caminho atual de readback permanece. Agrupamento de 512 KiB não mudou.

O teste GPU verifica tanto a preservação de bytes dirty quanto uma segunda escrita em página já baixada, sem consumir uma janela dirty externa. Quatro testes CTest passaram, incluindo o teste concorrente do tracker. A tentativa foi capturada por 180 segundos em `KytyPS5/_Build/windows/benchmarks/clean-write-candidate-01/`.

Na janela final foram evitados **1 de 57.780 pedidos CPU de escrita**; no processo inteiro, somente **4** envios. O contador inicialmente permaneceu zero. O benefício do mecanismo é desprezível nesta captura, enquanto a rechecagem acrescentaria locks a todos os pedidos. A tentativa foi retirada do fonte e da instalação após a coleta.

A taxa exploratória da tentativa foi 7,958 FPS, frente a 6,822 FPS da referência, mas as progressões/cenas são diferentes e houve 94 compilações na tentativa contra 19 na referência. Uma mudança que evitou um único despacho na janela não explica esse aumento. **Não há ganho de FPS comprovado.** Os valores mostram por que olhar apenas FPS em janelas por tempo de lançamento levaria a uma conclusão falsa.

O patch e as fontes exatos do candidato estão preservados na captura; o executável rejeitado está em `_Build/windows/benchmarks/clean-write-rejected.exe`. A referência instrumentada está em `_Build/windows/benchmarks/readback-samples-baseline.exe`. Nenhuma tentativa rejeitada deve ser usada como versão corrente.

Validação final após retirada: build Release, quatro testes CTest, dois testes Python e `git diff --check` aprovados. Instalação atual com métricas/amostras e política original, SHA256 `BA74C6D3EB379B2EBE981AEA36502A3DDEC99D8F6018241EC780D638832DFCCC`. Capturas concluídas e nenhum processo do emulador ativo. Somente alterações locais, ainda sem commit.

## Próxima correção e restrições concretas

O ponto a estudar está entre pedidos que **já estão na fila**, depois que um download agrupado torna páginas vizinhas atuais. Uma segunda checagem antes de enfileirar chega cedo demais.

Uma implementação futura precisa preservar a ordem dos comandos GPU. Não basta juntar arbitrariamente pedidos CPU e atendê-los antes de outros comandos que já estavam à frente na fila. Também não basta retornar cedo de `SendCommandSync`: o callback atual referencia registros do caller e a vida útil deles termina no retorno. Cancelamento/atendimento antecipado exige estado compartilhado com transições atômicas entre pending, executing, cancelled e completed, além de garantir que dados ainda em publicação não sejam tratados como disponíveis.

Antes disso, registrar sequência e tamanho da fila e correlacionar pedidos sem cópia com o pedido que resolveu a página. Isso distingue duplicatas simultâneas de pedidos que perderam propriedade por outra causa. Depois testar ordem de escritores/leitores, publicação, cancelamento e shutdown; medir o custo do próprio mecanismo. Fixar cenário visual/save e executar três pares A/B antes de anunciar FPS. Overhead da coleta e outros jogos permanecem pendentes.
