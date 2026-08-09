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
[udp_radio.cpp](udp_radio.cpp). Tests:
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

- **`class Medium` + `class VirtualRadio`** — shared air for tests: one process,
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
- **`class ReplayRadio`** — replays a captured dump with its original timing. What
  you reach for when a bug happened on the real network and has to happen again.
  `ReplayRadio(captures, params = {})`, the `IRadio` methods, `transmitted()`,
  `finished()`.

There is no SX1262 driver yet; RadioLib is vendored in `lib/` for it.

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
[udp_radio.cpp](udp_radio.cpp). Тесты:
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

- **`class Medium` + `class VirtualRadio`** — общий эфир для тестов: один процесс,
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
- **`class ReplayRadio`** — воспроизводит записанный дамп с исходными таймингами.
  То, за чем тянешься, когда ошибка случилась в настоящей сети и должна случиться
  снова. `ReplayRadio(captures, params = {})`, методы `IRadio`, `transmitted()`,
  `finished()`.

Драйвера SX1262 пока нет; RadioLib для него лежит в `lib/`.
