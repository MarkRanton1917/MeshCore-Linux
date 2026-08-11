# repeater

*Transit policy. English below, [русский](#repeater-русский) further down.*

## Purpose

The one place that decides whether somebody else's packet is carried on. It holds
no radio, no clock and no key: the packet, the time and how much of the airtime
budget is gone are all handed in, and what comes back is yes or no. That is why
the whole module tests in a loop with no network.

Forwarding itself belongs to [routing](../routing/); this only says whether it may
happen. With no policy attached a node forwards everything, which is what the room
server did before this module existed.

Files: [repeater.h](repeater.h), [repeater.cpp](repeater.cpp). Tests:
[test/test_repeater.cpp](../../test/test_repeater.cpp).

## Public API

### `struct Config`

- **`enabled`** — whether other people's packets travel on at all.
- **`maxHops`** (default 12) — deliberately below the `MAX_HOP_COUNT` of 63 the
  format allows. A packet already through a dozen nodes is lost rather than late.
- **`floodPerMinute`** (default 30) — floods per previous hop, per minute. One
  neighbour with a wedged transmitter must not spend the whole node's airtime.
  Direct packets are not counted: they follow a route somebody already learned and
  arrive one at a time, and they are the traffic this node exists to carry.
- **`dutyCeilingPermille`** (default 80) — transit stops once this much of the
  sliding window has gone, leaving the rest for our own traffic. Zero switches the
  check off.
- **`blocked`** — node hashes we refuse to carry for. One byte and ambiguous like
  every other hash here, so this blocks a hash, not a key — and blocks every node
  that happens to share it.

### `struct Stats`

`forwarded`, `hopLimit`, `blocked`, `rateLimited`, `budget` — every refusal
counted apart from the others. "The repeater dropped four thousand packets" is not
an answer; which counter moved is the diagnosis. The `get transit` admin command
reads these back.

### `class Policy`

- **`Policy(config = {})`**
- **`shouldForward(p, now, usedPermille) -> bool`** — the whole module. Time and
  load arrive as arguments for the same reason routing takes its clock through
  `tick()`: a policy that reads a clock of its own cannot be tested at speed, and
  this one is asked about every packet that lands. Every answer bumps exactly one
  counter.
- **`setEnabled(on)` / `enabled()`** — backs `set repeat on|off`.
- **`setMaxHops(hops)` / `maxHops()`** — backs `set hops.max <n>`; the setter
  clamps to what the format allows.
- **`stats()` / `clearStats()`** — the counters, and `clear stats`. Clearing
  changes nothing about what is carried.

Rate limiting uses one bucket per possible previous hop
(`REPEATER_SOURCE_BUCKETS` = 256 hashes plus one for packets heard first-hand,
whose sender hash is nowhere in the frame), so the table never grows and never
needs evicting.

---

<a name="repeater-русский"></a>

# repeater (русский)

## Назначение

Единственное место, где решается, понесём ли мы дальше чужой пакет. У модуля нет
ни радио, ни часов, ни ключей: пакет, время и то, сколько бюджета эфирного
времени израсходовано, передаются внутрь, а наружу возвращается «да» или «нет».
Именно поэтому весь модуль тестируется в цикле без всякой сети.

Сама пересылка — дело [routing](../routing/); здесь только говорится, можно ли её
делать. Без подключённой политики узел пересылает всё — так вёл себя сервер
комнаты до появления этого модуля.

Файлы: [repeater.h](repeater.h), [repeater.cpp](repeater.cpp). Тесты:
[test/test_repeater.cpp](../../test/test_repeater.cpp).

## Публичный интерфейс

### `struct Config`

- **`enabled`** — идут ли чужие пакеты дальше вообще.
- **`maxHops`** (по умолчанию 12) — намеренно меньше, чем `MAX_HOP_COUNT` = 63,
  который допускает формат. Пакет, прошедший десяток узлов, лучше потерять, чем
  доставить с опозданием.
- **`floodPerMinute`** (по умолчанию 30) — лавинных пакетов от одного
  предыдущего перехода в минуту. Один сосед с заклинившим передатчиком не должен
  тратить всё эфирное время узла. Прямые пакеты не считаются: они идут по уже
  выученному кем-то маршруту, приходят по одному, и ради них узел и существует.
- **`dutyCeilingPermille`** (по умолчанию 80) — транзит прекращается, когда
  израсходована эта доля скользящего окна, а остаток остаётся нашему собственному
  трафику. Ноль отключает проверку.
- **`blocked`** — хеши узлов, ради которых мы отказываемся нести пакеты. Один
  байт и та же неоднозначность, что у всех хешей здесь, поэтому блокируется хеш,
  а не ключ — и вместе с ним каждый узел, которому этот байт достался.

### `struct Stats`

`forwarded`, `hopLimit`, `blocked`, `rateLimited`, `budget` — каждый отказ
считается отдельно. «Ретранслятор отбросил четыре тысячи пакетов» — это не ответ;
диагноз это то, какой счётчик сдвинулся. Админская команда `get transit`
показывает их.

### `class Policy`

- **`Policy(config = {})`**
- **`shouldForward(p, now, usedPermille) -> bool`** — весь модуль. Время и
  нагрузка приходят аргументами по той же причине, по которой routing получает
  часы через `tick()`: политика с собственными часами не тестируется на скорости,
  а спрашивают её о каждом прилетевшем пакете. Каждый ответ увеличивает ровно
  один счётчик.
- **`setEnabled(on)` / `enabled()`** — за этим стоит `set repeat on|off`.
- **`setMaxHops(hops)` / `maxHops()`** — за этим стоит `set hops.max <n>`; сеттер
  ограничивает значение тем, что допускает формат.
- **`stats()` / `clearStats()`** — счётчики и `clear stats`. Обнуление ничего не
  меняет в том, что переносится.

Ограничение частоты использует по одной корзине на каждый возможный предыдущий
переход (`REPEATER_SOURCE_BUCKETS` = 256 хешей плюс одна для пакетов, услышанных
напрямую, чей хеш отправителя в кадре отсутствует), поэтому таблица никогда не
растёт и её никогда не нужно чистить.
