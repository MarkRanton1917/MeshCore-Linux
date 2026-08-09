# platform

*Clock, store, config, logging. English below, [русский](#platform-русский)
further down.*

## Purpose

The dull module whose only job is that no other file makes a system call. Every
piece of it comes in two forms — a real one and a test one — because that is what
lets the rest of the tree be tested without a filesystem, a network or a wait.

Files: [platform.h](platform.h), [platform.cpp](platform.cpp). Tests:
[test/test_platform.cpp](../../test/test_platform.cpp).

## Public API

### Clocks

Two clocks, and mixing them is the bug you find once a week and cannot explain:
monotonic never jumps and measures timeouts, wall time can move by a minute after
an NTP sync and fills protocol timestamps.

- **`struct IClock`** — `mono()` (milliseconds) and `wall()` (Unix seconds).
- **`class SystemClock`** — the real one. Plus two statics:
  - `wallLooksSynced(seconds)` — a node that advertises itself dated 1970 corrupts
    its neighbours' records, so the host waits for the first sync before starting.
  - `setWall(seconds)` — steps the wall clock, for the node that has no NTP and
    gets told the time by an admin instead (the `time <epoch>` command). `false`
    without the capability to do it, which is the normal case for an unprivileged
    process — the caller reports that, it is not an error worth refusing to run
    over.
- **`class FakeClock`** — hands the test moves by itself: `advance(by)`,
  `setMono(v)`, `setWall(v)`. Without it a timeout test either cannot be written
  or takes twelve seconds.

### Stores

Key-value, not file paths. Three keys exist: our identity, the contacts, the room
state.

- **`struct IStore`** — `read(key) -> optional<vector<uint8_t>>`,
  `write(key, data) -> bool`.
- **`class FileStore`** — `FileStore(directory)`. `write` goes through a temporary
  file and a rename: losing power mid-write must not cost the contact list.
- **`class MemoryStore`** — the same interface, in a map, for tests.

### Logging

- **`enum class LogLevel`** — `ERROR`, `WARN`, `INFO`, `DEBUG`. Levels exist
  because of the failed decrypt on roughly every 256th foreign packet: at warn the
  log drowns, at debug it is a counter you can look at.
- **`class Log`** — `setLevel(level)`, `level()`, `write(level, format, ...)`.

### `class Overlay`

The few settings an admin changed over the air, kept in a file of their own
(`<node.dir>/overrides.json`).

Writing the operator's config back was rejected on three counts: that file is
often not writable at all (root-owned, laid down by configuration management,
mounted read-only), the parser below flattens it to text and cannot reproduce it,
and mixing what was declared with what was changed since leaves no way to answer
either question. This file is plain JSON, readable by eye, and a change is undone
by deleting it. Keys are the config's own dotted names, so an entry shadows
exactly the key it is named after.

- **`Overlay(path)`**
- **`load()`** — a missing file is the normal case, not a failure. A damaged one is
  fatal: coming up with half the settings an admin believes are in force — a
  password among them — is worse than refusing to start.
- **`set(key, value)`** — writes through at once. A setting that waits for a flush
  is a setting lost to the next power cut, and the admin was told it was saved.
- **`get(key) -> optional<string>`**, **`values()`**, **`path()`**.

### `class Config`

Parsed once at startup into a struct; nothing reads it on a hot path.

- **`loadFromString(text)` / `loadFile(path)`** — JSON in, flat dotted keys out.
- **`applyOverlay(overlay)`** — laid over what `loadFile` read; an overlay value
  wins. Called once at startup, after which every getter answers with the
  effective setting and nothing downstream has to know there were two sources.
- **`get(key, fallback = "")`**, **`getInt`**, **`getBool`**, **`getList`**,
  **`has`**.

---

<a name="platform-русский"></a>

# platform (русский)

## Назначение

Скучный модуль, чья единственная задача — чтобы ни один другой файл не делал
системных вызовов. Каждая его часть существует в двух видах — настоящем и
тестовом, — и именно это позволяет тестировать всё остальное дерево без файловой
системы, сети и ожидания.

Файлы: [platform.h](platform.h), [platform.cpp](platform.cpp). Тесты:
[test/test_platform.cpp](../../test/test_platform.cpp).

## Публичный интерфейс

### Часы

Двое часов, и их смешение — та самая ошибка, которую находишь раз в неделю и не
можешь объяснить: монотонные никогда не прыгают и измеряют таймауты,
астрономические могут сдвинуться на минуту после синхронизации NTP и заполняют
временные метки протокола.

- **`struct IClock`** — `mono()` (миллисекунды) и `wall()` (секунды Unix).
- **`class SystemClock`** — настоящие. Плюс два статических метода:
  - `wallLooksSynced(seconds)` — узел, объявляющий себя с датой 1970 года, портит
    записи соседей, поэтому хост ждёт первой синхронизации перед стартом.
  - `setWall(seconds)` — переставляет астрономические часы, для узла, у которого
    нет NTP и которому время сообщает администратор (команда `time <epoch>`).
    `false`, если нет соответствующих прав, — обычное дело для непривилегированного
    процесса; вызывающая сторона это сообщает, но отказываться из-за этого
    работать не стоит.
- **`class FakeClock`** — часы, которые тест двигает сам: `advance(by)`,
  `setMono(v)`, `setWall(v)`. Без них тест на таймаут либо не пишется, либо идёт
  двенадцать секунд.

### Хранилища

Ключ-значение, а не пути к файлам. Ключей всего три: наша идентичность, контакты,
состояние комнаты.

- **`struct IStore`** — `read(key) -> optional<vector<uint8_t>>`,
  `write(key, data) -> bool`.
- **`class FileStore`** — `FileStore(directory)`. `write` идёт через временный
  файл и переименование: пропажа питания посреди записи не должна стоить списка
  контактов.
- **`class MemoryStore`** — тот же интерфейс, но в map, для тестов.

### Логирование

- **`enum class LogLevel`** — `ERROR`, `WARN`, `INFO`, `DEBUG`. Уровни существуют
  из-за неудачной расшифровки примерно каждого 256-го чужого пакета: на warn лог
  тонет, на debug это счётчик, на который можно посмотреть.
- **`class Log`** — `setLevel(level)`, `level()`, `write(level, format, ...)`.

### `class Overlay`

Те несколько настроек, которые администратор изменил по эфиру, в отдельном файле
(`<node.dir>/overrides.json`).

Запись обратно в конфиг оператора была отвергнута по трём причинам: этот файл
часто вообще недоступен на запись (принадлежит root, разложен системой управления
конфигурацией, смонтирован только на чтение), парсер ниже сплющивает его в текст
и воспроизвести не может, а смешение того, что было объявлено, с тем, что с тех
пор изменено, не даёт ответить ни на один из двух вопросов. Этот файл — обычный
JSON, читаемый глазами, и изменение отменяется удалением файла. Ключи здесь —
те же точечные имена, что и в конфиге, поэтому запись перекрывает ровно тот ключ,
по которому названа.

- **`Overlay(path)`**
- **`load()`** — отсутствующий файл это нормальный случай, а не сбой. Повреждённый
  — фатален: подняться с половиной настроек, которые администратор считает
  действующими, включая пароль, хуже, чем отказаться стартовать.
- **`set(key, value)`** — пишет сразу насквозь. Настройка, ждущая сброса на диск,
  это настройка, потерянная при ближайшем отключении питания, — а администратору
  уже сказали, что она сохранена.
- **`get(key) -> optional<string>`**, **`values()`**, **`path()`**.

### `class Config`

Разбирается один раз на старте в структуру; на горячем пути его никто не читает.

- **`loadFromString(text)` / `loadFile(path)`** — на входе JSON, на выходе плоские
  точечные ключи.
- **`applyOverlay(overlay)`** — накладывается на то, что прочитал `loadFile`;
  значение из overlay побеждает. Вызывается один раз на старте, после чего каждый
  геттер отвечает действующей настройкой, и ниже по дереву никому не нужно знать,
  что источников было два.
- **`get(key, fallback = "")`**, **`getInt`**, **`getBool`**, **`getList`**,
  **`has`**.
