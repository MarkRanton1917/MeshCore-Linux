# identity

*Who we are and who we know. English below, [русский](#identity-русский) further
down.*

## Purpose

Our own keypair, the contacts learned from adverts, and a cache of derived
shared secrets. Unlike [packet](../packet/) and [crypto](../crypto/), this module
holds state — split by how long each part lives: the identity and the contacts
survive a reboot, the derived secrets do not.

It is also where the one-byte node hash is dealt with honestly. A hash byte is
ambiguous by design, so lookups return *every* candidate and the caller settles
it by whose MAC checks out; returning a single contact would silently drop other
people's messages.

Files: [identity.h](identity.h), [identity.cpp](identity.cpp). Tests:
[test/test_identity.cpp](../../test/test_identity.cpp).

## On-disk state

`<node.dir>` holds the identity file (mode `0600`, never overwritten) and the
contacts file. The contacts format is at version 2, which keeps what the radio
heard — when last, how well, how many hops away — beside what each advert
claimed. Version 1 files are read and upgraded on the next save, their missing
fields left empty until the next advert fills them in.

## Public API

### `enum class NodeType`

`UNKNOWN`, `CHAT`, `REPEATER`, `ROOM_SERVER`, `SENSOR` — the low four bits of the
advert flags byte.

### `enum class Update`

What `remember()` did: `ADDED` (worth logging), `UPDATED`, `STALE` (worth
counting — it means somebody is replaying old adverts, or a peer's clock went
backwards), `REJECTED` (malformed advert, should not reach us at all).

### `struct Contact`

A peer learned from an advert. What the advert *claimed*: `pk`, `timestamp` (the
advert time, and the replay guard), `type`, `hasLocation`, `latitude`/`longitude`
(degrees × 1e6), `name`. What the radio *heard*, which is the link rather than
the peer: `lastHeard` (wall clock, seconds), `snr` (dB of the last frame),
`hops` (0 is a neighbour). Those are filled from adverts because an advert is the
one packet that names its sender before anything is decrypted — everything else
arrives behind a one-byte hash and is only attributed after the MAC checks out.

- `hash()` — the first byte of the public key: the node hash carried on air.
- `isNeighbour()` — heard at least once, and at zero hops. What "neighbour" means
  here and nowhere else, so a second opinion cannot creep in later.

### `class Store`

- **`loadOrCreate(dir)`** — reads the identity and contacts from `dir`, creating
  the identity on first run. `false` only on a real failure: no permission, full
  disk, corrupt file. A damaged key is never replaced silently.
- **`selfPk()` / `selfSk()`** — our keypair.
- **`selfHash()`** — our node hash, precomputed because every received packet
  needs it: `stripSelf` compares it against the first hop, deduplication against
  our own returning packets.
- **`find(pk) -> const Contact*`** — exact lookup by full public key.
- **`findByHash(hash) -> span<const Contact* const>`** — every contact sharing
  that hash byte. Collisions are near certain past a couple of hundred nodes, so
  the caller tries them all and lets the MAC decide.
- **`remember(advert) -> Update`** — takes an advert whose signature the caller
  has **already verified**, and records or refreshes the contact. Rejects one
  whose timestamp is not newer than the stored one, otherwise anyone could roll
  our records back by replaying a genuine old advert.
- **`noteHeard(pk, at, snr, hops)`** — what the radio heard, kept apart from what
  the advert said. Silently does nothing for a key we do not know: an unknown
  contact is not worth a slot until it has introduced itself.
- **`neighbours(limit) -> vector<const Contact*>`** — neighbours first, most
  recently heard first within that. Pointers into the store, so they live exactly
  as long as it does. This is what backs the `get neighbors` admin command.
- **`secretFor(pk) -> const SharedSecret*`** — cached ECDH. Not `const`: it fills
  the cache, and hiding that behind `mutable` would be worse. The pointer stays
  valid until a later call evicts the slot.
- **`flush()`** — writes the contacts out through a temporary file and a rename.
  Called on a timer and at shutdown, never per advert: adverts arrive constantly,
  and a synchronous write for each would stall the node and wear out an SD card.
- **`contactCount()`** — how many contacts are held.

Contacts are bounded (`MAX_CONTACTS`) so their storage never reallocates and the
hash index can hold plain pointers.

---

<a name="identity-русский"></a>

# identity (русский)

## Назначение

Наша собственная пара ключей, контакты, выученные из объявлений (adverts), и кеш
выведенных общих секретов. В отличие от [packet](../packet/) и
[crypto](../crypto/), этот модуль хранит состояние — разделённое по времени
жизни: идентичность и контакты переживают перезагрузку, выведенные секреты — нет.

Здесь же честно решается вопрос однобайтового хеша узла. Байт хеша неоднозначен
по замыслу, поэтому поиск возвращает *всех* кандидатов, а вызывающая сторона
решает, у кого сошёлся MAC; возврат одного контакта молча терял бы чужие
сообщения.

Файлы: [identity.h](identity.h), [identity.cpp](identity.cpp). Тесты:
[test/test_identity.cpp](../../test/test_identity.cpp).

## Состояние на диске

В `<node.dir>` лежат файл идентичности (режим `0600`, никогда не
перезаписывается) и файл контактов. Формат контактов — версии 2: он хранит то,
что услышало радио (когда в последний раз, насколько хорошо, за сколько
переходов), рядом с тем, что заявляло объявление. Файлы версии 1 читаются и
обновляются при следующем сохранении, а недостающие поля остаются пустыми, пока
их не заполнит очередное объявление.

## Публичный интерфейс

### `enum class NodeType`

`UNKNOWN`, `CHAT`, `REPEATER`, `ROOM_SERVER`, `SENSOR` — младшие четыре бита
байта флагов объявления.

### `enum class Update`

Что сделал `remember()`: `ADDED` (стоит залогировать), `UPDATED`, `STALE` (стоит
посчитать — значит, кто-то повторяет старые объявления или у соседа часы ушли
назад), `REJECTED` (некорректное объявление, до нас доходить вообще не должно).

### `struct Contact`

Узел, выученный из объявления. Что объявление *заявило*: `pk`, `timestamp`
(время объявления, оно же защита от повтора), `type`, `hasLocation`,
`latitude`/`longitude` (градусы × 1e6), `name`. Что *услышало радио* — это про
канал, а не про узел: `lastHeard` (астрономическое время, секунды), `snr` (дБ
последнего кадра), `hops` (0 — сосед). Эти поля заполняются из объявлений, потому
что объявление — единственный пакет, который называет отправителя до всякой
расшифровки: всё остальное приходит за однобайтовым хешем и приписывается
отправителю только после того, как сойдётся MAC.

- `hash()` — первый байт открытого ключа: хеш узла, который идёт по эфиру.
- `isNeighbour()` — слышали хотя бы раз и на нулевом числе переходов. Определение
  «соседа» здесь и больше нигде, чтобы позже не завелось второе мнение.

### `class Store`

- **`loadOrCreate(dir)`** — читает идентичность и контакты из `dir`, создавая
  идентичность при первом запуске. `false` только при настоящем сбое: нет прав,
  диск полон, файл повреждён. Повреждённый ключ никогда не заменяется молча.
- **`selfPk()` / `selfSk()`** — наша пара ключей.
- **`selfHash()`** — наш хеш узла, вычисленный заранее, потому что он нужен
  каждому принятому пакету: `stripSelf` сравнивает его с первым переходом, а
  дедупликация — с нашими же вернувшимися пакетами.
- **`find(pk) -> const Contact*`** — точный поиск по полному открытому ключу.
- **`findByHash(hash) -> span<const Contact* const>`** — все контакты с этим
  байтом хеша. За парой сотен узлов коллизии практически неизбежны, поэтому
  вызывающая сторона перебирает всех и даёт решить MAC.
- **`remember(advert) -> Update`** — принимает объявление, подпись которого
  вызывающая сторона **уже проверила**, и записывает или обновляет контакт.
  Отвергает объявление, чья метка времени не новее сохранённой, иначе любой мог
  бы откатить наши записи, повторив подлинное старое объявление.
- **`noteHeard(pk, at, snr, hops)`** — то, что услышало радио, отдельно от того,
  что заявило объявление. Для неизвестного ключа молча ничего не делает:
  незнакомый контакт не стоит слота, пока не представился.
- **`neighbours(limit) -> vector<const Contact*>`** — сначала соседи, внутри —
  недавно слышанные первыми. Возвращаются указатели внутрь хранилища, так что они
  живут ровно столько же, сколько оно. На этом построена админская команда
  `get neighbors`.
- **`secretFor(pk) -> const SharedSecret*`** — кешированный ECDH. Не `const`: он
  заполняет кеш, а прятать это за `mutable` было бы хуже. Указатель действителен,
  пока более поздний вызов не вытеснит слот.
- **`flush()`** — записывает контакты через временный файл и переименование.
  Вызывается по таймеру и при завершении, но не на каждое объявление: объявления
  идут постоянно, и синхронная запись на каждое тормозила бы узел и изнашивала
  карту памяти.
- **`contactCount()`** — сколько контактов хранится.

Число контактов ограничено (`MAX_CONTACTS`), поэтому их хранилище никогда не
перевыделяется, а хеш-индекс может держать обычные указатели.
