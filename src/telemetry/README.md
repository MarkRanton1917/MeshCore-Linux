# telemetry

*Event bus and counters. English below, [русский](#telemetry-русский) further
down.*

## Purpose

The module you can switch off with a flag while everything else keeps working. If
that is not true, something is wired the wrong way round.

[routing](../routing/) and [room](../room/) never call telemetry. They publish
events to a `Bus` and telemetry subscribes — otherwise, in six months, the routing
logic contains an MQTT client and can be neither disabled nor tested without a
broker. Both take a `Bus*` in their config, and a null pointer switches the whole
thing off at no cost.

Files: [telemetry.h](telemetry.h), [telemetry.cpp](telemetry.cpp). Tests:
[test/test_telemetry.cpp](../../test/test_telemetry.cpp).

## Public API

### `enum class EventType`

`FrameRx`, `FrameTx`, `FrameDuplicate`, `DecryptFailed`, `Forwarded`,
`ForwardRefused`, `AckTimeout`, `ContactAdded`, `ClientLogin`, `PostAdded`,
`RouteReset`, and `Count` as the array bound.

`ForwardRefused` is one number on purpose: *why* the transit policy refused is the
[repeater](../repeater/)'s own business and is counted there. What a graph needs
to show is transit falling off.

### `struct Event`

`type`, `at` (wall clock seconds), `detail` — a length, an id, whatever the type
implies.

### `class Bus`

- **`Bus(capacity = 256)`**
- **`publish(type, at, detail = 0)`** — a non-blocking write into a ring. When the
  consumer falls behind, events are lost on purpose: telemetry may not stall
  packet handling or blow an ack deadline.
- **`poll(out) -> bool`** — takes the oldest event, if any.
- **`dropped()`** — how many were thrown away, which is itself a signal.
- **`pending()`** — how many are waiting.

### `struct Counters`

`byType[]` indexed by `EventType`, plus the two that matter most in operation —
`txQueueDepth` and `dutyCyclePermille`: a growing queue and a spent budget mean
the node is overloaded, and you want to see that before people complain. Also
`dropped`.

### `class Collector`

- **`drain(bus)`** — empties the bus into the counters. Called from the main loop,
  never from the packet path.
- **`observe(txQueueDepth, dutyCyclePermille)`** — records the two sampled gauges.
- **`counters()`** — the accumulated totals.

### Rendering

- **`prometheusText(counters) -> string`** — text for a `/metrics` endpoint.
  Rendering is separate from serving on purpose: the HTTP listener belongs to the
  host, not here.
- **`logCounters(counters)`** — always available, and costs nothing when the log
  level is above debug.

---

<a name="telemetry-русский"></a>

# telemetry (русский)

## Назначение

Модуль, который можно выключить флагом, а всё остальное продолжит работать. Если
это не так — что-то соединено не в ту сторону.

[routing](../routing/) и [room](../room/) никогда не вызывают телеметрию. Они
публикуют события в `Bus`, а телеметрия на них подписывается — иначе через
полгода в логике маршрутизации окажется MQTT-клиент, и её нельзя будет ни
отключить, ни протестировать без брокера. Оба принимают `Bus*` в своей
конфигурации, и нулевой указатель отключает всё это без каких-либо издержек.

Файлы: [telemetry.h](telemetry.h), [telemetry.cpp](telemetry.cpp). Тесты:
[test/test_telemetry.cpp](../../test/test_telemetry.cpp).

## Публичный интерфейс

### `enum class EventType`

`FrameRx`, `FrameTx`, `FrameDuplicate`, `DecryptFailed`, `Forwarded`,
`ForwardRefused`, `AckTimeout`, `ContactAdded`, `ClientLogin`, `PostAdded`,
`RouteReset` и `Count` как граница массива.

`ForwardRefused` намеренно один: *почему* транзитная политика отказала — это дело
[repeater](../repeater/), и там это и считается. Графику нужно показать, что
транзит просел.

### `struct Event`

`type`, `at` (астрономическое время, секунды), `detail` — длина, идентификатор,
что подразумевает конкретный тип.

### `class Bus`

- **`Bus(capacity = 256)`**
- **`publish(type, at, detail = 0)`** — неблокирующая запись в кольцевой буфер.
  Когда потребитель отстаёт, события намеренно теряются: телеметрия не имеет права
  тормозить обработку пакетов или сорвать срок подтверждения.
- **`poll(out) -> bool`** — забирает самое старое событие, если оно есть.
- **`dropped()`** — сколько выброшено, что само по себе сигнал.
- **`pending()`** — сколько ожидает.

### `struct Counters`

`byType[]` с индексом по `EventType`, плюс два самых важных в эксплуатации —
`txQueueDepth` и `dutyCyclePermille`: растущая очередь и израсходованный бюджет
означают, что узел перегружен, и это хочется увидеть раньше, чем начнут
жаловаться люди. И ещё `dropped`.

### `class Collector`

- **`drain(bus)`** — опустошает шину в счётчики. Вызывается из главного цикла и
  никогда с пути обработки пакета.
- **`observe(txQueueDepth, dutyCyclePermille)`** — фиксирует два измеряемых
  показателя.
- **`counters()`** — накопленные итоги.

### Отрисовка

- **`prometheusText(counters) -> string`** — текст для эндпойнта `/metrics`.
  Формирование намеренно отделено от отдачи: HTTP-слушатель принадлежит хосту, а
  не этому модулю.
- **`logCounters(counters)`** — доступно всегда и ничего не стоит, когда уровень
  логирования выше debug.
