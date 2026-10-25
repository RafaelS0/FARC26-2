# FARC26-2

Servidor HTTP com QoS em C (Pthreads).

## Compilar e executar

```
make
./server <vazao_maxima_kbps>
```

ou, com o `Makefile`:

```
make run                    # usa CAPACITY=10000 (padrão)
make run CAPACITY=5000      # servidor com 5000 kbps
```

## Parâmetro de execução: vazão máxima

| Item       | Valor |
|------------|-------|
| Formato    | argumento posicional, obrigatório: `./server <vazao_maxima_kbps>` |
| Unidade    | **kbps** (quilobits por segundo; 1 kbps = 1000 bits/s), a mesma unidade de `qos.conf` |
| Valores    | inteiro **maior que zero** e até 4294967295 |
| Exemplo    | `./server 10000` → servidor com 10 Mbps de vazão total |

O parâmetro é validado antes de qualquer outra inicialização. Se estiver incorreto
(ausente, `0`, negativo, decimal, com texto ou unidade, grande demais), o servidor **não
inicia**: imprime em `stderr` o motivo e o texto de uso, e termina com código de saída `2`.

```
$ ./server -5
Erro: vazão máxima inválida ("-5"): a vazão não pode ser negativa.

Uso: ./server <vazao_maxima_kbps>
...
```

`./server --help` exibe o uso. Depois de validado, o limite fica disponível aos demais
módulos por `config_get_max_capacity_kbps()` (`config.h`); o módulo de controle de
admissão o recebe em `admission_init()`.

## Controle de admissão

O servidor limita o que **reserva de vazão**, e não o número de conexões.

- Cada IP tem uma taxa máxima (`qos.conf`; IPs não listados usam 1000 kbps).
- Todas as conexões de um mesmo IP **compartilham** a taxa do IP, como já faz o
  `qos_send_limited()`. Por isso a **primeira** conexão de um IP reserva a taxa do IP e as
  seguintes entram sem reservar mais nada.
- A reserva de um IP é **liberada quando a sua última conexão termina**.
- A soma das reservas nunca ultrapassa a vazão máxima configurada.
- **Política para quem não cabe: recusar.** O cliente recebe `503 Service Unavailable` com
  `Retry-After: 5` e a conexão é encerrada; não há fila de espera. Um IP cuja taxa em
  `qos.conf` é maior que a vazão máxima do servidor é sempre recusado.
- A reserva dura **enquanto a conexão estiver aberta**, inclusive ociosa (keep-alive, até
  o timeout de 15 s). A capacidade é devolvida quando a conexão fecha.

Exemplo com `./server 2600` e IPs de 1000 kbps: dois IPs distintos são admitidos (2000
kbps); um terceiro seria recusado (3000 > 2600), mas uma segunda conexão de um dos dois
IPs é admitida normalmente, porque divide a reserva existente.

A capacidade total, reservada e disponível aparece no cabeçalho do monitor (veja abaixo).

As atualizações de capacidade são protegidas por um mutex (`admission.c`).

## Métricas de desempenho

Para cada objeto enviado (resposta `200` com arquivo), o servidor registra três instantes
(relógio monotônico, `CLOCK_MONOTONIC`) e o tamanho transmitido:

| Símbolo | Significado |
|---------|-------------|
| `t_req`  | instante em que a requisição foi recebida (retorno do `recv`) |
| `t_ini`  | instante imediatamente antes do primeiro byte do objeto (depois do cabeçalho HTTP) |
| `t_fim`  | instante em que o servidor terminou de entregar o último byte ao TCP, já considerando a espera de pacing do QoS |
| `B`      | bytes do objeto efetivamente enviados (só o corpo; cabeçalho HTTP não conta) |

**Largura de banda estimada** (medida ao fim de cada objeto):

```
banda_kbps = (B × 8) / (t_fim − t_ini em ms)
```

Bits por milissegundo é exatamente kbps (1 kbps = 1000 bits/s). Um objeto de 125000 B enviado
em 1000 ms dá 125000 × 8 / 1000 = 1000 kbps.

**RTT estimado**: lido do kernel (`TCP_INFO`, campo `tcpi_rtt`, RTT suavizado em µs convertido
para ms) no início da conexão e novamente ao fim de cada objeto.

**Atraso fim a fim estimado** (objetos completos):

```
atraso_ms = (t_fim − t_req em ms) + RTT_ms
```

O cliente espera a requisição chegar (RTT/2), o servidor processa e transmite (`t_fim − t_req`)
e o último byte percorre o caminho de volta (RTT/2); a soma dos dois meios RTT é um RTT.

**Hipóteses e limites**

- O caminho é simétrico: ida e volta valem metade do RTT cada.
- O pacing do QoS emula um enlace com a taxa do IP: a espera depois do último bloco conta como o
  tempo de serialização desse bloco. Um cliente real, em enlace sem limite, mede um atraso
  menor em até o tempo do último bloco (≤ 4096 × 8 / taxa).
- Objetos sem limitação (HTML) são entregues ao buffer do kernel em microssegundos. Transferências
  com menos de 1 ms (`METRICS_MIN_DURATION_MS`) ou com 0 byte não geram banda e aparecem como
  `n/d`. Acima disso, a banda de um HTML reflete a entrega ao kernel e não a capacidade do caminho.
- Cada conexão compartilha a taxa do IP com as demais do mesmo IP; a banda medida de uma
  conexão já inclui a espera por essa divisão.
- Transferência **interrompida** (erro de envio, cliente que fecha ou reinicia a conexão, erro de
  leitura do arquivo): conta só os blocos que foram entregues ao TCP, calcula a banda sobre o
  tempo até a falha (se ≥ 1 ms) e **não estima atraso** (o último byte nunca chegou). O contador
  de interrompidas e o estado `interrompido` são registrados.
- O monitor mostra a última medição válida de cada métrica; antes da primeira, mostra `n/d`.
- Respostas de erro (404, 405...) não são transferências de objeto e não entram nas métricas.
- O RTT depende de `TCP_INFO` (Linux). Se não estiver disponível, RTT e atraso aparecem como `n/d`.

Cada objeto gera uma linha no `server.log`:

```
[metricas] 127.0.0.2:51375 /obj.bin completo bytes=125000/125000 envio=1006.800 ms banda=993.274 kbps rtt=0.046 ms atraso=1006.864 ms
```

## Monitoramento em tempo real

Ao iniciar, o servidor passa a redesenhar o painel a cada 1000 ms (`MONITOR_INTERVAL_MS`), sem
precisar de nenhuma tecla:

```
=== Monitor do servidor === 22:38:24 === atualização a cada 1000 ms
Vazão: total 10000 kbps | reservada 1000 kbps | disponível 9000 kbps | clientes ativos: 1

IP              PORTA  ESTADO                RTT (ms)  ATRASO (ms)  BANDA (kbps) OBJ ok/int  ÚLTIMO
127.0.0.2       51375  ocioso (encerra 13s)     0.014          0.1           n/d      1/0  completo
```

| Coluna | Conteúdo |
|--------|----------|
| IP, PORTA | origem da conexão |
| ESTADO | `atendendo` (requisição em andamento) ou `ocioso (encerra Ns)` (tempo até o timeout de 15 s) |
| RTT (ms) | RTT estimado |
| ATRASO (ms) | atraso fim a fim estimado do último objeto completo |
| BANDA (kbps) | largura de banda estimada do último objeto medido |
| OBJ ok/int | objetos completos / interrompidos |
| ÚLTIMO | situação do último objeto: `completo`, `interrompido` ou `n/d` |

- Métricas ainda indisponíveis (cliente recém-conectado) aparecem como `n/d`.
- São listados até 40 clientes (`MONITOR_MAX_ROWS`); os demais viram a linha `... e mais N cliente(s)`.
- O quadro é montado em um buffer a partir de uma cópia (snapshot) da tabela de clientes feita
  sob o mutex e escrito com uma única chamada, então o atendimento não é bloqueado.
- O terminal é usado só pelo monitor. As mensagens das threads (conexões, requisições, erros,
  métricas por objeto) vão para o arquivo `server.log`, com horário e protegidas por mutex, e
  não se misturam ao painel. O arquivo é recriado a cada execução.
- Em terminal interativo o painel é redesenhado no mesmo lugar; com a saída redirecionada
  (arquivo ou pipe), cada quadro é impresso em sequência, sem códigos de escape.

## Testes

```
make test        # unitários: parâmetro, admissão, métricas, snapshot e monitor (com threads)
make test-e2e    # ponta a ponta: servidor real + conexões TCP (requer python3)
```

Os testes de ponta a ponta usam IPs de origem diferentes do bloco `127.0.0.0/8` (Linux) e a
porta 8080, que deve estar livre. `tests/e2e_metrics.py` cria objetos temporários de tamanho
conhecido (60000, 125000, 68750, 200000 e 300000 bytes) em `objects/`, baixa-os com taxas
conhecidas (550 e 1000 kbps) e confere no `server.log` e no monitor: bytes, tempo de envio,
banda (±4% da taxa), RTT, atraso (≈ tempo esperado + RTT, e coerente com o atraso medido no
cliente), transferência interrompida, `n/d` para cliente novo e atualização contínua do painel.

## Estrutura

| Arquivo | Função |
|---------|--------|
| `config.c/.h`    | Leitura e validação do parâmetro de vazão máxima |
| `admission.c/.h` | Controle de admissão por vazão (reserva, liberação, estatísticas) |
| `qos.c/.h`       | Taxa por IP e limitação da transmissão |
| `server.c/.h`    | Socket, atendimento dos clientes e recusa (503) |
| `http.c/.h`      | Requisições e respostas HTTP |
| `client_manager.c/.h` | Tabela de clientes conectados, métricas por cliente e snapshot |
| `metrics.c/.h`   | Instantes e bytes de cada transferência, banda, RTT e atraso fim a fim |
| `monitor.c/.h`   | Painel em tempo real |
| `log.c/.h`       | Log em arquivo (`server.log`) com mutex |
