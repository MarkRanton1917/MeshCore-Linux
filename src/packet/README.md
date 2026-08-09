# packet

*Frame layout. English below, [русский](#packet-русский) further down.*

## Purpose

The wire format and nothing else: how a MeshCore frame is split into header,
transport codes, path and payload, and how each payload type is read and
written. It has no heap, no state, no clock and no keys — a `Packet` is fixed-size
arrays and lives on the stack of whoever received it. Every bound is known at
compile time, which is why this is the one module that depends on nothing but
[defines.h](../defines.h).

Parsing is deliberately separate from crypto: a repeater has to read a header
and a path for packets it can never decrypt, and the `crypto` module has no
opinion about frames.

Files: [packet.h](packet.h), [packet.cpp](packet.cpp). Tests:
[test/test_packet.cpp](../../test/test_packet.cpp).

## Frame layout

```
header(1) [transport_codes(4)] path_length(1) path(0..64) payload(0..184)
```

255 bytes maximum. The header carries the route type in the low two bits and the
payload type in the next four. `path_length` carries the hop count in six bits
and the per-hop hash size in the other two. Transport codes are present only for
the two transport route types and are little-endian on air.

## Public API

### Enums

- **`RouteType`** — `TRANSPORT_FLOOD`, `FLOOD`, `DIRECT`, `TRANSPORT_DIRECT`.
  The low two bits of the header.
- **`PayloadType`** — `REQ`, `RESPONSE`, `TXT_MSG`, `ACK`, `ADVERT`, `GRP_TXT`,
  `GRP_DATA`, `ANON_REQ`, `PATH`, `TRACE`, `MULTIPART`, `CONTROL`, `RAW_CUSTOM`.
  Bits 2..5 of the header. It says which decoder applies; no decoder re-checks it.

### `struct Packet`

A parsed frame. Holds the raw `header`, the two transport codes, `hashSize`,
`hopCount`, the fixed `path` array, and the `payload` array with its
`payloadSize`. Accessors:

- `routeType()`, `payloadType()` — the two fields packed into the header byte.
- `hasTransportCodes()` — true for the two transport route types, i.e. whether
  the four transport bytes are present on air.
- `pathSize()` — `hopCount * hashSize`, the bytes the path actually occupies.
- `payloadView()` — the payload as a `ByteView`, without copying.

### Payload structs

All of them hold views into the buffer they were decoded from; **that buffer must
outlive them**.

- **`Advert`** — `publicKey`, `timestamp`, `signature`, `appdata`.
  `signedMessage(out)` builds what the signature actually covers —
  `publicKey || timestamp || appdata`, the signature itself left out — because on
  air those fields are not contiguous. Returns bytes written, `0` on failure.
- **`TextMsg`** — a decrypted text payload: `timestamp`, `txtType` (upper six
  bits of the flags byte), `attempt` (lower two), `message`.
- **`AnonReq`** — a login from somebody who is not a contact yet:
  `destinationHash`, `publicKey`, `cipherMac`, `ciphertext`.
- **`LoginResponse`** — a four-byte random `nonce` plus an application-defined
  `body`. The nonce is not decoration: without it two identical replies would
  serialize to the same frame and the second would be dropped as a duplicate.
- **`Envelope`** — the addressed wrapper worn by `REQ`, `RESPONSE`, `TXT_MSG` and
  `PATH`: `destinationHash`, `sourceHash`, `cipherMac`, `ciphertext`.
- **`GroupMsg`** — channel traffic: `channelHash`, `cipherMac`, `ciphertext`. One
  hash instead of two, because a channel message is addressed to whoever holds
  the key rather than to a node.
- **`Trace`** — `tag`, `authCode`, `flags`, and `snr`, one signed byte per hop in
  quarter-decibels (so ±32 dB fits a byte). Read side by side with the frame's own
  path: the path says who carried it, this says how well each of them heard it.
- **`PathReturn`** — a learned route sent back: `path`, `extraType`, `extra`.

### Frame functions

- **`parse(frame) -> optional<Packet>`** — splits a frame into its parts.
  `nullopt` on anything malformed. Garbage off the air is routine, and a declared
  path length that overruns the buffer is exactly what this has to catch.
- **`serialize(p, out) -> optional<size_t>`** — writes the packet back out as a
  frame; returns bytes written, `nullopt` if `out` is too small.
- **`appendSelf(p, selfHash) -> bool`** — flood forwarding: append our own hash to
  the path. `false` when the path is full, which means the packet travels no
  further.
- **`stripSelf(p, selfHash) -> bool`** — direct forwarding: if our hash is first in
  the path, drop it. `false` means the packet is not routed through us and should
  be discarded without a word.
- **`appendTraceHop(p, snrQuarterDb) -> bool`** — writes how we heard this trace
  into the packet, in place. `false` when the payload is not a trace or there is
  no room for another hop — and then the trace stops here rather than travelling
  on with a hop missing, because a path and a list of readings that no longer
  line up is worse than a trace that ends early.

### Payload codecs

Decoders return `nullopt` on a malformed payload; encoders return bytes written
or `nullopt` when `out` is too small. The caller picks the codec from
`Packet::payloadType()`.

| Decode | Encode | Used by |
| --- | --- | --- |
| `decodeAdvert` | `encodeAdvert` | everyone, repeaters included |
| `decodeText` | `encodeText` | room server, after decryption |
| `decodeAnonReq` | `encodeLoginResponse` | room server: login and its reply |
| `decodeEnvelope` | `encodeEnvelope` | every direct payload type |
| `decodeGroup` | `encodeGroup` | anyone holding a channel key — nobody in `routing` |
| `decodePath` | `encodePath` | room server as the end node |
| `decodeTrace` | `encodeTrace` | every repeater on the way |

---

<a name="packet-русский"></a>

# packet (русский)

## Назначение

Формат кадра и больше ничего: как кадр MeshCore разбирается на заголовок,
транспортные коды, путь и полезную нагрузку, и как читается и пишется каждый тип
нагрузки. Нет ни кучи, ни состояния, ни часов, ни ключей — `Packet` состоит из
массивов фиксированного размера и живёт на стеке того, кто принял кадр. Все
границы известны на этапе компиляции, поэтому это единственный модуль, который не
зависит ни от чего, кроме [defines.h](../defines.h).

Разбор намеренно отделён от криптографии: ретранслятор обязан читать заголовок и
путь у пакетов, которые он никогда не сможет расшифровать, а модуль `crypto`
ничего не знает о кадрах.

Файлы: [packet.h](packet.h), [packet.cpp](packet.cpp). Тесты:
[test/test_packet.cpp](../../test/test_packet.cpp).

## Разметка кадра

```
header(1) [transport_codes(4)] path_length(1) path(0..64) payload(0..184)
```

Максимум 255 байт. В заголовке младшие два бита — тип маршрутизации, следующие
четыре — тип нагрузки. В байте `path_length` шесть бит занимает число переходов,
остальные два — размер хеша на переход. Транспортные коды присутствуют только у
двух транспортных типов маршрутизации и передаются по эфиру little-endian.

## Публичный интерфейс

### Перечисления

- **`RouteType`** — `TRANSPORT_FLOOD`, `FLOOD`, `DIRECT`, `TRANSPORT_DIRECT`.
  Младшие два бита заголовка.
- **`PayloadType`** — `REQ`, `RESPONSE`, `TXT_MSG`, `ACK`, `ADVERT`, `GRP_TXT`,
  `GRP_DATA`, `ANON_REQ`, `PATH`, `TRACE`, `MULTIPART`, `CONTROL`, `RAW_CUSTOM`.
  Биты 2..5 заголовка. Он говорит, какой декодер применять; сами декодеры его не
  перепроверяют.

### `struct Packet`

Разобранный кадр. Хранит сырой `header`, два транспортных кода, `hashSize`,
`hopCount`, массив `path` фиксированного размера и массив `payload` вместе с
`payloadSize`. Методы доступа:

- `routeType()`, `payloadType()` — два поля, упакованные в байт заголовка.
- `hasTransportCodes()` — истина для двух транспортных типов маршрутизации, то
  есть присутствуют ли в эфире четыре транспортных байта.
- `pathSize()` — `hopCount * hashSize`, сколько байт реально занимает путь.
- `payloadView()` — нагрузка как `ByteView`, без копирования.

### Структуры нагрузок

Все они хранят представления (views) в тот буфер, из которого были декодированы;
**этот буфер обязан жить дольше них**.

- **`Advert`** — `publicKey`, `timestamp`, `signature`, `appdata`.
  `signedMessage(out)` собирает то, что на самом деле покрыто подписью —
  `publicKey || timestamp || appdata`, без самой подписи, — потому что в эфире
  эти поля не идут подряд. Возвращает число записанных байт, `0` при ошибке.
- **`TextMsg`** — расшифрованная текстовая нагрузка: `timestamp`, `txtType`
  (старшие шесть бит байта флагов), `attempt` (младшие два), `message`.
- **`AnonReq`** — вход от того, кто ещё не является контактом:
  `destinationHash`, `publicKey`, `cipherMac`, `ciphertext`.
- **`LoginResponse`** — четыре случайных байта `nonce` плюс определяемое
  приложением `body`. Nonce здесь не для красоты: без него два одинаковых ответа
  сериализовались бы в один и тот же кадр, и второй был бы отброшен как дубликат.
- **`Envelope`** — адресная обёртка, которую носят `REQ`, `RESPONSE`, `TXT_MSG` и
  `PATH`: `destinationHash`, `sourceHash`, `cipherMac`, `ciphertext`.
- **`GroupMsg`** — трафик канала: `channelHash`, `cipherMac`, `ciphertext`. Один
  хеш вместо двух, потому что сообщение канала адресовано тому, у кого есть ключ,
  а не узлу.
- **`Trace`** — `tag`, `authCode`, `flags` и `snr`: по одному знаковому байту на
  переход, в четвертях децибела (так ±32 дБ помещаются в байт). Читается вместе с
  путём самого кадра: путь говорит, кто нёс пакет, а это — насколько хорошо
  каждый из них его слышал.
- **`PathReturn`** — выученный маршрут, отправляемый обратно: `path`,
  `extraType`, `extra`.

### Функции над кадром

- **`parse(frame) -> optional<Packet>`** — разбирает кадр на части. `nullopt` при
  любой некорректности. Мусор из эфира — обычное дело, и объявленная длина пути,
  выходящая за буфер, это ровно то, что здесь обязано отлавливаться.
- **`serialize(p, out) -> optional<size_t>`** — пишет пакет обратно в виде кадра;
  возвращает число записанных байт, `nullopt`, если `out` мал.
- **`appendSelf(p, selfHash) -> bool`** — лавинная пересылка: добавить свой хеш в
  путь. `false`, когда путь заполнен, — значит, пакет дальше не пойдёт.
- **`stripSelf(p, selfHash) -> bool`** — прямая пересылка: если наш хеш первый в
  пути, убрать его. `false` означает, что пакет идёт не через нас и его следует
  молча отбросить.
- **`appendTraceHop(p, snrQuarterDb) -> bool`** — записывает в пакет, как мы
  услышали эту трассировку, прямо на месте. `false`, если нагрузка не трассировка
  или в кадре нет места ещё на один переход, — и тогда трассировка
  останавливается здесь, а не идёт дальше с пропущенным переходом, потому что
  путь и список замеров, которые больше не совпадают, хуже, чем трассировка,
  оборвавшаяся раньше.

### Кодеки нагрузок

Декодеры возвращают `nullopt` при некорректной нагрузке; кодировщики — число
записанных байт или `nullopt`, если `out` мал. Вызывающая сторона выбирает кодек
по `Packet::payloadType()`.

| Декодирование | Кодирование | Кто использует |
| --- | --- | --- |
| `decodeAdvert` | `encodeAdvert` | все, включая ретрансляторы |
| `decodeText` | `encodeText` | комната, после расшифровки |
| `decodeAnonReq` | `encodeLoginResponse` | комната: вход и ответ на него |
| `decodeEnvelope` | `encodeEnvelope` | все прямые типы нагрузки |
| `decodeGroup` | `encodeGroup` | тот, у кого есть ключ канала, — в `routing` таких нет |
| `decodePath` | `encodePath` | комната как конечный узел |
| `decodeTrace` | `encodeTrace` | каждый ретранслятор по пути |
