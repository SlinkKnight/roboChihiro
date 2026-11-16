# Protocolo de controle: braço robótico com Arduino Mega 2560

Documentação do protocolo de comandos do firmware `driver_motores_mega.ino`.
O firmware controla 5 motores de passo (STEP/DIR/EN) e uma garra (servo em D9).

## Conexão

| Canal | Pinos | Baud | Uso |
|---|---|---|---|
| Serial USB (`Serial`) | conector USB | 115200 | depuração e testes pelo computador |
| Serial2 (hardware) | TX2 = pino 16, RX2 = pino 17 | 115200 | ligação com a CYD/ponte |

- Os dois canais aceitam **os mesmos comandos** e respondem no canal em que o comando chegou.
- Os níveis lógicos são de 5 V no Mega. Se a CYD (ESP32) trabalha em 3,3 V, use um conversor de nível no RX2 do Mega ou um divisor de tensão. O TX do ESP32 (3,3 V) costuma ser lido corretamente pelo Mega.
- Ligue TX da CYD em RX2 (17) do Mega, RX da CYD em TX2 (16) do Mega, e una os GND.
- O baud da CYD precisa ser igual a `BAUD_SW` no firmware.
- O I2C do Mega (pinos 20/21) não é usado, porque esses pinos são STEP/DIR do eixo 1.

## Formato das mensagens

- Texto ASCII, um comando por linha, terminado em `\n` (o `\r` também é aceito).
- Máximo de 95 caracteres por linha.
- O comando é a primeira letra, **sem diferenciar maiúscula de minúscula**.
- Números separados por espaço, vírgula ou tabulação. Decimais usam ponto: `45.5`.
- Ângulos em **graus no eixo de saída** (já considerando redução e microstep).
- Eixos numerados de **1 a 5**.

## Comandos

### Movimento

| Comando | Descrição | Exemplo |
|---|---|---|
| `P a1 a2 a3 a4 a5 g` | Posição absoluta dos 5 eixos e da garra (0 a 180) de uma vez | `P 0 30 -45 90 0 100` |
| `M<n> <graus>` | Eixo `n` vai para a posição absoluta | `M3 45.5` |
| `R<n> <graus>` | Eixo `n` se move de forma relativa, a partir do destino atual | `R1 -10` |
| `V<n> <graus/s>` | Jog contínuo: gira o eixo na velocidade dada, sinal define o sentido | `V2 40`, `V2 -40` |
| `V<n> 0` | Para o jog do eixo `n` com desaceleração | `V2 0` |
| `G <graus>` | Garra (0 a 180) | `G 90` |
| `S<n> <graus/s>` | Define a velocidade máxima do eixo `n` | `S2 90` |
| `X` | Para todos os eixos com desaceleração | `X` |
| `Z` | Zera a posição atual de todos os eixos (define o home) | `Z` |

Observações:
- Os destinos são limitados aos limites de software de cada eixo (padrão: -360° a +360°). Valores fora disso são cortados no limite, sem erro.
- `M`, `R`, `V` e `P` apenas definem o destino e retornam `OK` na hora. O movimento acontece depois, em segundo plano. Use `?` para saber quando terminou (`BUSY 0`).
- O `V` funciona definindo como destino o limite do eixo e ajustando a velocidade máxima. O eixo para sozinho ao chegar no limite.
- O `V` e o `S` alteram a velocidade máxima do eixo **até o próximo reset**. Para voltar ao padrão, mande `S<n>` com o valor desejado ou reinicie o Mega.
- Para jog, use `V<n> <vel>` ao pressionar e `V<n> 0` ao soltar. Enviar vários `R` pequenos em sequência deixa o motor lento, porque cada comando acelera e freia do zero.

### Travamento das bobinas

| Comando | Descrição |
|---|---|
| `E 1` | Trava as bobinas (driver habilitado, motor segura a posição) |
| `E 0` | Para os motores e solta as bobinas (o eixo gira à mão) |

- No boot os motores ficam **travados**, e continuam travados depois de cada movimento.
- Qualquer comando de movimento trava os motores de novo automaticamente.
- Soltar o motor pode fazer o braço cair por gravidade e perder a posição física. Depois de `E 0`, confira o home antes de continuar.
- Manter as bobinas travadas esquenta os motores e consome corrente o tempo todo.
- O pino EN de cada eixo está na tabela `axes[]` (padrão: pinos 22 a 26). Use `-1` se o EN não estiver ligado. O nível que habilita é `EN_ACTIVE_LEVEL` (`LOW` por padrão).

### Teste e status

| Comando | Descrição |
|---|---|
| `T` | Testa todos os motores e a garra (±30° em cada eixo, depois volta) |
| `T<n>` | Testa só o eixo `n` |
| `?` | Status |

O `T` **bloqueia** o recebimento de comandos até terminar (alguns segundos por eixo).

## Respostas

| Resposta | Significado |
|---|---|
| `OK` | Comando aceito |
| `ERR args` | Argumentos faltando, inválidos ou eixo fora de 1 a 5 |
| `ERR cmd` | Comando desconhecido |
| `PRONTO` | Enviado no boot, nos dois canais |
| `TESTE eixo n` / `TESTE garra` / `TESTE OK` | Progresso do teste |
| `POS ...` | Resposta do `?` (ver abaixo) |

### Resposta do `?`

```
POS a1 a2 a3 a4 a5 garra BUSY b LOCK l
```

| Campo | Significado |
|---|---|
| `a1` a `a5` | Posição atual de cada eixo, em graus (2 casas decimais) |
| `garra` | Posição da garra em graus (1 casa decimal) |
| `b` | `1` se algum eixo ainda está em movimento, `0` se todos pararam |
| `l` | `1` se as bobinas estão travadas, `0` se estão soltas |

Exemplo: `POS 0.00 30.00 -45.00 90.00 0.00 100.0 BUSY 0 LOCK 1`

O campo `LOCK` vem no **final** da linha. Se o parser da CYD esperava o formato anterior (que terminava em `BUSY b`), ele precisa ignorar o campo extra.

## Exemplos de sessão

Mover o eixo 3 até 45° e esperar terminar:

```
> M3 45
< OK
> ?
< POS 0.00 0.00 12.40 0.00 0.00 0.0 BUSY 1 LOCK 1
> ?
< POS 0.00 0.00 45.00 0.00 0.00 0.0 BUSY 0 LOCK 1
```

Jog do eixo 2 (apertar e soltar):

```
> V2 40
< OK
  ... eixo girando enquanto o botão está pressionado ...
> V2 0
< OK
```

Soltar os motores e travar de novo:

```
> E 0
< OK
> E 1
< OK
```

## Funcionamento interno

- **Perfil trapezoidal:** cada eixo acelera a partir de `MIN_SPEED` (50 passos/s) até a velocidade máxima e desacelera para parar exatamente no destino. A aceleração é independente por eixo.
- **Sem bloqueio:** os passos são gerados no `loop()` por temporização com `micros()`, então os 5 eixos se movem ao mesmo tempo. Cada eixo tem sua própria velocidade e não há interpolação coordenada. Eixos com distâncias diferentes chegam em momentos diferentes.
- **Inversão de sentido:** se o novo destino está atrás do movimento atual, o eixo desacelera, para e reinicia no sentido contrário.
- **Parâmetros por eixo:** `stepsPerDeg`, limites, `maxSpeed` e `accel` ficam na tabela `axes[]`. O macro `SPD(micro, reducao)` calcula passos por grau: `200 * micro * reducao / 360`.

### Parâmetros dos eixos (padrão)

| Eixo | Motor | Microstep | Redução | Vel. máx. (passos/s) | Aceleração (passos/s²) |
|---|---|---|---|---|---|
| 1 | 2x Nema23 | 1/4 | 1:1 | 1500 | 3000 |
| 2 | Nema17 | 1/16 | 5:1 | 4000 | 10000 |
| 3 | Nema17 | 1/16 | 1:1 | 4000 | 10000 |
| 4 | Nema14 | 1/16 | 1:1 | 4000 | 10000 |
| 5 | Nema17 60 mm | 1/16 | 1:1 | 4000 | 10000 |

### Opções de compilação

| Macro | Padrão | Efeito |
|---|---|---|
| `TEST_ON_BOOT` | `false` | Roda o teste ao ligar (cerca de 10 s sem ouvir comandos) |
| `DEBUG_RX` | `false` | Imprime na USB, a cada 2 s, o que chegou pela Serial2. Atrasa os passos, use só para diagnóstico |
| `TEST_DEG` | `30.0` | Amplitude do teste em graus |
| `EN_ACTIVE_LEVEL` | `LOW` | Nível que habilita o driver |
| `PULSE_US` | `3` | Largura do pulso STEP em µs |

## Limitações

- Não há sensores de fim de curso nem de home. A posição é contada por passos, e `Z` define o zero. Se o motor perder passos, o firmware não percebe.
- Uma linha com mais de 95 caracteres é truncada.
- O buffer da Serial2 do Mega é de 64 bytes. Evite enviar muitas linhas de uma vez sem esperar o `OK`, principalmente durante o `T`.
- O `E 0` espera os eixos pararem antes de soltar, e o firmware não atende comandos durante essa espera.
