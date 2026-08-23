# radio

*Airtime, duty cycle, TX queue, drivers. English below,
[русский](#radio-русский) further down.*

## Purpose

The only module that knows about SPI, GPIO and physics. Everything above it sees
five methods, and only two of them get used often.

It owns the things nobody above may decide for itself: how long a frame will hold
the air, whether the duty cycle allows it now, and what order queued frames go out
in. [routing](../routing/) hands it frames and never asks whether it may transmit.

Files: [radio.h](radio.h), [radio.cpp](radio.cpp),
[udp_radio.cpp](udp_radio.cpp), [sx1262_radio.cpp](sx1262_radio.cpp). Tests:
[test/test_radio.cpp](../../test/test_radio.cpp).

## Public API

### Interface

- **`struct RxMeta`** — `rssi`, `snr`, `at`, `airtimeUs` of a received frame.
- **`struct RxSink`** — `onFrame(frame, meta)`; what a driver calls on reception.
- **`enum class Priority`** — `HIGH`, `NORMAL`, `LOW`. Three levels, and the third
  is the point: acks and returned paths ahead of everything, our own traffic next,
  and other people's packets last. A node that repeats at the same priority as it
  answers spends its air on strangers while its own clients wait.
- **`struct IRadio`** — what every driver implements:
  - `send(frame, priority)` — into the queue, not into the air: the real
    transmission is asynchronous. `false` means the queue overflowed, never "it did
    not go out".
  - `tick(now)` — drains the queue and the socket.
  - `setSink(sink)` — where received frames go.
  - `airtimeUs(length)` — the one physical quantity that leaks upward: routing
    sizes its forwarding delay from it.
  - `canTransmitNow()`.

### `struct Params`

`frequencyHz`, `spreadingFactor`, `bandwidthHz` (MeshCore runs narrow — 62.5 kHz),
`codingRate`, `preambleSymbols`, `syncWord`, plus `dutyCyclePercent`,
`dutyWindowMs` and `queueDepth`. **These must match the network bit for bit. One
wrong field and the node hears nobody, with no error anywhere.**

- **`airtimeUsFor(params, payloadLength)`** — LoRa airtime per the SX1276/SX1262
  datasheet formula.

### `class DutyCycle`

A sliding hour of "when and for how long", so the budget is spent on measured
airtime rather than on a guess. Exceeding it is a regulatory breach, not a little
extra traffic.

`DutyCycle(percent, window = 1h)`, `record(at, airtimeUs)`,
`allows(now, airtimeUs)`, `usedPermille(now)` — the last of which is also what
[repeater](../repeater/) is handed to decide whether there is air left for other
people's packets.

### `class TxQueue`

A priority queue in front of a half-duplex radio: while transmitting it hears
nothing, so the queue is needed even when traffic looks light.

`TxQueue(depth)`, `push(frame, priority)`, `pop(out, priority = nullptr)`,
`size()`. The priority comes back with the frame for the one caller that has to
put it back: a transmission the duty cycle refused belongs where it was, not at
the head of the queue.

### Drivers

- **`class Medium` + `class VirtualRadio`** — *a test fixture, not a driver: it
  is built from code and cannot be selected from a config.* Shared air for tests:
  one process,
  several nodes, a matrix of who hears whom. Airtime is computed with the same
  formula but never actually spent.
  - `Medium`: `attach(node)`, `link(a, b)`, `unlink(a, b)`, `hears(listener,
    speaker)`, `broadcast(from, frame, meta)`.
  - `VirtualRadio`: the `IRadio` methods, plus `deliver(frame, meta)`,
    `setPowered(on)` (off the air entirely — a repeater somebody switched off),
    `queueDepth()`, `usedPermille()`.
- **`struct UdpOptions` + `class UdpRadio`** — several processes on one machine, or
  several machines on a LAN, keeping the real airtime and duty-cycle accounting.
  Each node lists the peers it can hear, so the peer lists *are* the visibility
  matrix: a chain A–B–C is three configs, not a special mode.
  - `UdpOptions`: `bindAddress`, `listenPort` (0 lets the kernel choose), `peers`
    (`"host:port"` each), and optional `multicastGroup`/`multicastPort`, joined for
    receiving and added to the peers for sending, so a LAN needs no peer list at
    all.
  - `UdpRadio`: `open()` (false leaves the reason in the log), `boundPort()`, the
    `IRadio` methods, `queueDepth()`, `usedPermille()`. Datagrams carry an 8-byte
    sender id ahead of the frame — with multicast loopback, or a peer list that
    includes ourselves, we would otherwise receive our own transmissions. That
    header belongs to this transport and never reaches the protocol.
- **`class ReplayRadio`** — a test fixture too, on the same terms. Replays a
  captured dump with its original timing. What
  you reach for when a bug happened on the real network and has to happen again.
  `ReplayRadio(captures, params = {})`, the `IRadio` methods, `transmitted()`,
  `finished()`.

- **`struct Sx1262Options` + `class Sx1262Radio`** — the chip itself, over SPI,
  through RadioLib and lgpio. Compiled only into a build configured with
  `-DRADIO=SX1262`, so the declaration sits behind `SX1262_RADIO`, which that
  build sets. `-DRADIO=UDP`, the default, gets `UdpRadio` instead. One or the
  other is compiled, never both: which radio a node has is settled by the build,
  and there is no config key to disagree with it.
  - `Sx1262Options`: `spiBus`/`spiChipSelect`/`spiSpeedHz`/`gpioChip`, the BCM
    pins `pinNss`/`pinBusy`/`pinReset`/`pinDio1`, the RF switch
    (`pinRxEnable`/`pinTxEnable`/`dio2AsRfSwitch`), and
    `tcxoVoltage`/`useRegulatorLdo`/`txPowerDbm`/`currentLimitMa`. `-1` means "no
    pin"; for `pinNss` that is not "absent" but "the SPI controller drives chip
    select", which is what a board wired to CE0 wants.
  - `Sx1262Radio`: `open()` (false leaves the reason in the log — a radio that
    will not start has no useful degraded mode), the `IRadio` methods,
    `queueDepth()`, `usedPermille()`.

  Driven entirely from `tick()`: the IRQ line is read over SPI rather than
  through a GPIO interrupt, because the node's loop already comes round every few
  milliseconds and a callback would arrive on lgpio's own thread, inside a driver
  that owns no lock. Receive is drained before a transmission starts, or a frame
  already in the buffer would be lost to it. Airtime is charged when a
  transmission begins rather than when it ends: a frame the chip never reports
  finishing still held the air for its whole length. A transmission that misses
  its deadline returns the driver to receive — a node waiting forever for a
  `TX_DONE` that is not coming has left the network without telling anybody.

### Defaults, and the two that fail silently

The defaults are the pinout of the Waveshare SX1262 XXXM LoRaWAN/GNSS HAT, read
off that board's own driver: NSS on CE0 (`-1`), BUSY 20, RESET 18, DIO1 16.
Nothing here can be probed, so another board means reading its schematic.

Two of them give no error when they are wrong, only silence:

- **`tcxoVoltage`.** A board with a TCXO needs its voltage; this one has a plain
  32 MHz crystal and needs `0`. The wrong answer means the chip never leaves
  reset, and `begin()` fails with nothing to say about why.
- **The RF switch**, where this board's silkscreen lies. The pin labelled TXEN is
  high while *receiving* — the vendor driver pulls BCM 6 low to transmit and high
  to listen — so it goes in `pinRxEnable`, and the transmit side is DIO2's, which
  the HAT solders to RXEN. Backwards, the node hears nothing and the PA talks
  into a switch pointed the wrong way.

On the Pi: `apt install liblgpio-dev`, SPI enabled in `raspi-config`, and
`cmake -S . -B build -DRADIO=SX1262`. Without lgpio that configure step fails
outright rather than dropping the driver, which is the whole point of naming the
radio instead of sniffing for it.

---

<a name="radio-русский"></a>

# radio (русский)

## Назначение

Единственный модуль, который знает про SPI, GPIO и физику. Всё, что выше, видит
пять методов, и часто используются из них только два.

Он владеет тем, что никто выше не вправе решать за себя: сколько кадр будет
занимать эфир, позволяет ли рабочий цикл передавать сейчас и в каком порядке уйдут
кадры из очереди. [routing](../routing/) отдаёт ему кадры и никогда не спрашивает,
можно ли передавать.

Файлы: [radio.h](radio.h), [radio.cpp](radio.cpp),
[udp_radio.cpp](udp_radio.cpp), [sx1262_radio.cpp](sx1262_radio.cpp). Тесты:
[test/test_radio.cpp](../../test/test_radio.cpp).

## Публичный интерфейс

### Интерфейс

- **`struct RxMeta`** — `rssi`, `snr`, `at`, `airtimeUs` принятого кадра.
- **`struct RxSink`** — `onFrame(frame, meta)`; это драйвер вызывает при приёме.
- **`enum class Priority`** — `HIGH`, `NORMAL`, `LOW`. Три уровня, и весь смысл в
  третьем: подтверждения и возвращаемые маршруты впереди всего, наш собственный
  трафик следом, чужие пакеты — последними. Узел, ретранслирующий с тем же
  приоритетом, с каким отвечает, тратит эфир на незнакомцев, пока его собственные
  клиенты ждут.
- **`struct IRadio`** — то, что реализует каждый драйвер:
  - `send(frame, priority)` — в очередь, а не в эфир: настоящая передача
    асинхронна. `false` означает переполнение очереди, но никогда — «оно не ушло».
  - `tick(now)` — разбирает очередь и сокет.
  - `setSink(sink)` — куда уходят принятые кадры.
  - `airtimeUs(length)` — единственная физическая величина, просачивающаяся
    наверх: по ней маршрутизация назначает задержку пересылки.
  - `canTransmitNow()`.

### `struct Params`

`frequencyHz`, `spreadingFactor`, `bandwidthHz` (MeshCore работает узко —
62,5 кГц), `codingRate`, `preambleSymbols`, `syncWord`, а также
`dutyCyclePercent`, `dutyWindowMs` и `queueDepth`. **Всё это должно совпадать с
сетью бит в бит. Одно неверное поле — и узел не слышит никого, причём без единой
ошибки где-либо.**

- **`airtimeUsFor(params, payloadLength)`** — эфирное время LoRa по формуле из
  документации SX1276/SX1262.

### `class DutyCycle`

Скользящий час записей «когда и сколько», чтобы бюджет расходовался по измеренному
эфирному времени, а не по догадке. Его превышение — нарушение регламента, а не
«немного лишнего трафика».

`DutyCycle(percent, window = 1 ч)`, `record(at, airtimeUs)`,
`allows(now, airtimeUs)`, `usedPermille(now)` — последнее передаётся ещё и в
[repeater](../repeater/), чтобы тот решил, остался ли эфир на чужие пакеты.

### `class TxQueue`

Очередь с приоритетами перед полудуплексным радио: во время передачи оно не слышит
ничего, поэтому очередь нужна даже при, казалось бы, слабом трафике.

`TxQueue(depth)`, `push(frame, priority)`, `pop(out, priority = nullptr)`,
`size()`. Приоритет возвращается вместе с кадром ради единственного вызывающего,
которому придётся класть кадр обратно: передача, которой отказал рабочий цикл,
должна вернуться туда, где была, а не в голову очереди.

### Драйверы

- **`class Medium` + `class VirtualRadio`** — *тестовая оснастка, а не драйвер:
  собирается из кода и в конфиге не выбирается.* Общий эфир для тестов: один
  процесс,
  несколько узлов, матрица «кто кого слышит». Эфирное время считается по той же
  формуле, но фактически не расходуется.
  - `Medium`: `attach(node)`, `link(a, b)`, `unlink(a, b)`, `hears(listener,
    speaker)`, `broadcast(from, frame, meta)`.
  - `VirtualRadio`: методы `IRadio`, плюс `deliver(frame, meta)`,
    `setPowered(on)` (полностью вне эфира — ретранслятор, который выключили),
    `queueDepth()`, `usedPermille()`.
- **`struct UdpOptions` + `class UdpRadio`** — несколько процессов на одной машине
  или несколько машин в локальной сети, с сохранением настоящего учёта эфирного
  времени и рабочего цикла. Каждый узел перечисляет узлы, которые он слышит,
  поэтому списки соседей *и есть* матрица видимости: цепочка A–B–C это три
  конфига, а не особый режим.
  - `UdpOptions`: `bindAddress`, `listenPort` (0 — порт выбирает ядро), `peers`
    (по `"host:port"` каждый) и необязательные `multicastGroup`/`multicastPort`,
    к которым присоединяются на приём и которые добавляются к адресатам на
    передачу, так что локальной сети список соседей не нужен вовсе.
  - `UdpRadio`: `open()` (при false причина остаётся в логе), `boundPort()`,
    методы `IRadio`, `queueDepth()`, `usedPermille()`. Датаграммы несут перед
    кадром 8-байтный идентификатор отправителя: при multicast-петле или списке
    соседей, включающем нас самих, мы иначе принимали бы собственные передачи. Этот
    заголовок принадлежит транспорту и до протокола не доходит.
- **`class ReplayRadio`** — тоже оснастка, на тех же правах. Воспроизводит
  записанный дамп с исходными таймингами.
  То, за чем тянешься, когда ошибка случилась в настоящей сети и должна случиться
  снова. `ReplayRadio(captures, params = {})`, методы `IRadio`, `transmitted()`,
  `finished()`.

- **`struct Sx1262Options` + `class Sx1262Radio`** — сам чип по SPI, через
  RadioLib и lgpio. Попадает только в сборку, настроенную с `-DRADIO=SX1262`,
  поэтому объявление стоит за `SX1262_RADIO`, который эта сборка и выставляет.
  `-DRADIO=UDP` по умолчанию даёт вместо него `UdpRadio`. Компилируется одно или
  другое, никогда оба: какое у узла радио, решает сборка, и ключа в конфиге,
  который бы с ней спорил, нет.
  - `Sx1262Options`: `spiBus`/`spiChipSelect`/`spiSpeedHz`/`gpioChip`, выводы BCM
    `pinNss`/`pinBusy`/`pinReset`/`pinDio1`, ВЧ-переключатель
    (`pinRxEnable`/`pinTxEnable`/`dio2AsRfSwitch`) и
    `tcxoVoltage`/`useRegulatorLdo`/`txPowerDbm`/`currentLimitMa`. `-1` означает
    «вывода нет»; для `pinNss` это не «отсутствует», а «выборкой кристалла
    занимается контроллер SPI» — именно то, что нужно плате, посаженной на CE0.
  - `Sx1262Radio`: `open()` (при false причина остаётся в логе — у радио, которое
    не запустилось, нет полезного урезанного режима), методы `IRadio`,
    `queueDepth()`, `usedPermille()`.

  Всё делается из `tick()`: линия прерывания читается по SPI, а не через
  прерывание GPIO, потому что цикл узла и так возвращается каждые несколько
  миллисекунд, а колбэк пришёл бы в собственном потоке lgpio, внутрь драйвера, у
  которого нет ни одной блокировки. Приём разбирается до начала передачи, иначе
  уже лежащий в буфере кадр был бы ею потерян. Эфирное время списывается в начале
  передачи, а не в конце: кадр, о завершении которого чип так и не сообщил, всё
  равно занимал эфир целиком. Передача, не уложившаяся в срок, возвращает драйвер
  на приём — узел, вечно ждущий `TX_DONE`, который не придёт, ушёл из сети, никого
  не предупредив.

### Значения по умолчанию и две настройки, отказывающие молча

По умолчанию стоит распиновка Waveshare SX1262 XXXM LoRaWAN/GNSS HAT, взятая из
драйвера самой платы: NSS на CE0 (`-1`), BUSY 20, RESET 18, DIO1 16. Ничего из
этого нельзя опросить, поэтому другая плата означает чтение её схемы.

Две из них при ошибке не дают никакой диагностики, только тишину:

- **`tcxoVoltage`.** Плате с TCXO нужно его напряжение; у этой обычный кварц на
  32 МГц, и нужен `0`. Неверный ответ — и чип не выходит из сброса, а `begin()`
  падает, ничего не сообщая о причине.
- **ВЧ-переключатель** — то место, где надпись на плате врёт. Вывод с
  маркировкой TXEN высок во время *приёма*: драйвер производителя тянет BCM 6
  вниз на передачу и вверх на приём. Поэтому он идёт в `pinRxEnable`, а сторона
  передачи — забота DIO2, к которому плата припаяла RXEN. Наоборот — и узел не
  слышит ничего, а усилитель говорит в переключатель, повёрнутый не туда.

На Pi: `apt install liblgpio-dev`, включённый SPI в `raspi-config` и
`cmake -S . -B build -DRADIO=SX1262`. Без lgpio эта настройка сборки падает
сразу, а не выбрасывает драйвер, — ради чего радио и называют, а не вынюхивают.
