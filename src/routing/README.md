# routing

*Flood and direct routing. English below, [русский](#routing-русский) further
down.*

## Purpose

Moving packets between the radio and the node above: deduplication, flood and
direct forwarding, path learning, acks and retries.

Two rules shape the whole module. It is the module with a clock and it never
reads one — time arrives through `tick()`. And the radio is an interface, because
a five-node network has to run inside one process on a laptop. Together they are
why a multi-node routing test finishes in milliseconds.

`routing` knows nothing about posts, clients or ACLs. It deals in packets and
contacts, hands decrypted payloads up through `Delegate`, and puts frames in the
radio queue. Policy lives in `Delegate`, not here — including transit policy,
which is [repeater](../repeater/) behind `shouldForward`.

Files: [routing.h](routing.h), [routing.cpp](routing.cpp). Tests:
[test/test_routing.cpp](../../test/test_routing.cpp).

## Public API

### Types

- **`Millis`**, **`SendId`** — milliseconds of monotonic time; the handle
  `sendDirect` returns and `onAck`/`onDeliveryFailed` report against.
- **`RxMeta`** — `airtime` (how long the frame held the air, which sets the
  backoff scale) and `snr`.
- **`Priority`** — `HIGH`, `NORMAL`, `LOW`. Acks and returned paths go ahead of
  other people's floods: they are short, delay-sensitive, and decide whether the
  sender switches to direct. Transit comes last of all — repeating a stranger must
  never delay an answer we owe a client.

### `struct Radio`

What routing needs from a radio: `enqueue(frame, priority)`. The radio owns the
duty cycle budget and listen-before-talk; routing only hands it frames and never
decides whether it may transmit now.

### `struct Delegate`

The layer above. [room](../room/) implements it.

- **`onPayload(from, type, plain)`** — a decrypted payload from a known contact.
- **`onAnon(from, plain)`** — separate from `onPayload` because an `ANON_REQ`
  arrives from somebody who is not a contact yet: there is no contact to hand
  over, only the raw key.
- **`onGroup(type, payload)`** *(optional)* — channel traffic, handed up exactly
  as it came off the air. Routing holds no channel keys and cannot tell whether
  this one is even addressed to us, so it does not try: the layer that holds the
  key decides by whether the MAC checks out.
- **`onTrace(trace, path)`** *(optional)* — a trace is unencrypted and addressed to
  nobody. Handed up decoded, together with the path accumulated so far; the two
  are read together, one hash and one signal reading per hop.
- **`onAck(id)` / `onDeliveryFailed(id)`** — the fate of a `sendDirect`.
- **`shouldAck(type, plain)`** *(optional, default true)* — acknowledging is the
  default, but some payloads must not be acked at all: a CLI command that gets one
  leaves the client confused. Routing cannot tell those apart; the layer that
  decoded the payload can.
- **`shouldForward(p)`** *(optional, default true)* — the one piece of policy left
  outside: hop limits, blocklists, transport code filtering.

### `struct Config`

- **`floodAckTimeout`** (12 s), **`directAckBase`** (4 s),
  **`directAckPerHop`** (2 s), **`maxAttempts`** (3) — a flood spreads across the
  whole network; a known route should answer fast.
- **`forwardAirtimeFactor`** (2), **`forwardJitter`** (400 ms) — forwarding delay
  is airtime times the factor, plus a random spread. The spread is not decoration:
  three repeaters that heard one flood and answer together drown each other out,
  and the network does not recover on its own.
- **`maxPending`** (32), **`maxRoutes`** (128).
- **`seenSlots`** (128), **`seenTtl`** (5 min) — the duplicate cache, sized from
  how long a flood takes to cross the network. Slots alone cannot say how long a
  packet is remembered: on a busy node the ring wraps in seconds and an echo
  arriving to a clean cache is forwarded a second time, which reads as "the network
  is slow sometimes". So entries carry an age as well, and an entry past its age
  stops suppressing. A zero TTL remembers until the slot is reused.
- **`bus`** — optional telemetry. Routing publishes and never calls telemetry
  back, so leaving this null switches the whole thing off.

### `class Router`

- **`Router(store, radio, delegate, config = {})`**
- **`onFrame(frame, meta)`** — the only entry point from the radio. Parses,
  deduplicates, decrypts what is ours, hands payloads up, and considers
  forwarding the rest.
- **`tick(now)`** — moves time: ack timeouts, deferred retransmissions, draining
  the queue.
- **`sendDirect(to, type, payload, wantAck) -> SendId`** — over a learned route if
  there is one, by flood otherwise. With `wantAck` the frame is retried until the
  ack arrives or the attempts run out.
- **`sendFlood(type, payload)`** — to nobody in particular: adverts, channel
  traffic.
- **`hasRoute(to)` / `forgetRoute(to)`** — the learned direct routes.
- **`pendingCount()` / `queuedCount()`** — unacknowledged sends and delayed
  frames.
- **`reversePath(forward, out) -> size_t`** *(static)* — a path is recorded from
  the sender towards us; sending back needs it the other way round. Getting the
  direction wrong survives the first flood exchange and only breaks direct
  packets, so it is public and tested.

---

<a name="routing-русский"></a>

# routing (русский)

## Назначение

Перемещение пакетов между радио и слоем выше: дедупликация, лавинная и прямая
пересылка, обучение маршрутам, подтверждения и повторы.

Весь модуль определяют два правила. Это модуль с часами, который часов никогда не
читает: время приходит через `tick()`. И радио — это интерфейс, потому что сеть
из пяти узлов должна запускаться внутри одного процесса на ноутбуке. Вместе они и
дают то, что тест маршрутизации на нескольких узлах отрабатывает за миллисекунды.

`routing` ничего не знает ни о сообщениях на доске, ни о клиентах, ни о правах
доступа. Он работает с пакетами и контактами, отдаёт расшифрованные нагрузки
наверх через `Delegate` и кладёт кадры в очередь радио. Политика живёт в
`Delegate`, а не здесь, — включая транзитную политику, то есть
[repeater](../repeater/) за `shouldForward`.

Файлы: [routing.h](routing.h), [routing.cpp](routing.cpp). Тесты:
[test/test_routing.cpp](../../test/test_routing.cpp).

## Публичный интерфейс

### Типы

- **`Millis`**, **`SendId`** — миллисекунды монотонного времени; дескриптор,
  который возвращает `sendDirect` и по которому отчитываются
  `onAck`/`onDeliveryFailed`.
- **`RxMeta`** — `airtime` (сколько кадр занимал эфир, отсюда масштаб задержки) и
  `snr`.
- **`Priority`** — `HIGH`, `NORMAL`, `LOW`. Подтверждения и возвращаемые маршруты
  идут впереди чужих лавинных пакетов: они короткие, чувствительны к задержке и
  определяют, перейдёт ли отправитель на прямую передачу. Транзит — в самом
  конце: ретрансляция незнакомца не должна задерживать ответ, который мы должны
  своему клиенту.

### `struct Radio`

Что нужно маршрутизации от радио: `enqueue(frame, priority)`. Радио владеет
бюджетом рабочего цикла и прослушиванием перед передачей; маршрутизация только
отдаёт ему кадры и никогда не решает, можно ли передавать прямо сейчас.

### `struct Delegate`

Слой выше. Его реализует [room](../room/).

- **`onPayload(from, type, plain)`** — расшифрованная нагрузка от известного
  контакта.
- **`onAnon(from, plain)`** — отдельно от `onPayload`, потому что `ANON_REQ`
  приходит от того, кто ещё не контакт: передавать нечего, кроме сырого ключа.
- **`onGroup(type, payload)`** *(необязательно)* — трафик канала, отдаваемый
  наверх ровно в том виде, в каком он пришёл из эфира. Маршрутизация не хранит
  ключей каналов и не может понять, адресовано ли это нам, поэтому и не пытается:
  решает тот слой, у которого есть ключ, — по тому, сойдётся ли MAC.
- **`onTrace(trace, path)`** *(необязательно)* — трассировка не шифруется и
  никому не адресована. Отдаётся наверх декодированной вместе с накопленным
  путём; их читают вместе, по одному хешу и одному замеру сигнала на переход.
- **`onAck(id)` / `onDeliveryFailed(id)`** — судьба `sendDirect`.
- **`shouldAck(type, plain)`** *(необязательно, по умолчанию true)* —
  подтверждать это поведение по умолчанию, но некоторые нагрузки подтверждать
  нельзя вовсе: CLI-команда, получившая подтверждение, сбивает клиента с толку.
  Маршрутизация их не различает, а слой, декодировавший нагрузку, — различает.
- **`shouldForward(p)`** *(необязательно, по умолчанию true)* — единственный
  кусок политики, оставленный снаружи: лимиты переходов, чёрные списки,
  фильтрация по транспортным кодам.

### `struct Config`

- **`floodAckTimeout`** (12 с), **`directAckBase`** (4 с),
  **`directAckPerHop`** (2 с), **`maxAttempts`** (3) — лавина расходится по всей
  сети, а известный маршрут обязан отвечать быстро.
- **`forwardAirtimeFactor`** (2), **`forwardJitter`** (400 мс) — задержка
  пересылки это эфирное время, умноженное на коэффициент, плюс случайный разброс.
  Разброс здесь не украшение: три ретранслятора, услышавшие одну лавину и
  ответившие одновременно, заглушают друг друга, и сеть сама из этого не выходит.
- **`maxPending`** (32), **`maxRoutes`** (128).
- **`seenSlots`** (128), **`seenTtl`** (5 мин) — кеш дубликатов, размер которого
  взят из того, сколько лавина идёт через сеть. Одни только слоты не задают, как
  долго пакет помнится: на нагруженном узле кольцо оборачивается за секунды, и
  эхо, пришедшее в уже очищенный кеш, пересылается второй раз — а выглядит это как
  «сеть иногда тормозит». Поэтому записи несут ещё и возраст, и запись старше
  своего срока перестаёт подавлять. Нулевой TTL означает «помнить, пока слот не
  переиспользуют».
- **`bus`** — необязательная телеметрия. Маршрутизация только публикует и никогда
  не вызывает телеметрию в ответ, поэтому null здесь отключает её целиком.

### `class Router`

- **`Router(store, radio, delegate, config = {})`**
- **`onFrame(frame, meta)`** — единственная точка входа со стороны радио.
  Разбирает, отсеивает дубликаты, расшифровывает то, что адресовано нам, отдаёт
  нагрузки наверх и решает вопрос о пересылке остального.
- **`tick(now)`** — двигает время: таймауты подтверждений, отложенные повторные
  передачи, разбор очереди.
- **`sendDirect(to, type, payload, wantAck) -> SendId`** — по выученному
  маршруту, если он есть, иначе лавиной. С `wantAck` кадр повторяется, пока не
  придёт подтверждение или не кончатся попытки.
- **`sendFlood(type, payload)`** — никому конкретно: объявления, трафик каналов.
- **`hasRoute(to)` / `forgetRoute(to)`** — выученные прямые маршруты.
- **`pendingCount()` / `queuedCount()`** — неподтверждённые отправки и отложенные
  кадры.
- **`reversePath(forward, out) -> size_t`** *(статический)* — путь записывается от
  отправителя к нам, а для ответа он нужен в обратную сторону. Перепутанное
  направление переживает первый обмен лавинами и ломает только прямые пакеты,
  поэтому функция публичная и покрыта тестом.
