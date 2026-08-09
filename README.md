# MeshCore

*English below, [русский](#meshcore-русский) further down.*

A MeshCore room server for Linux: a LoRa mesh node that keeps a small
noticeboard, forwards other people's packets, and can be run as a whole network
of processes on one laptop.

The stack is written so that every layer can be built and tested without the one
above or below it. Packets are parsed without crypto, crypto knows nothing about
packets, routing knows nothing about posts, and nothing except `platform` makes a
system call. The wiring is paid for once, in [main.cpp](src/main.cpp).

## Modules

Dependencies point downward only; no module names the one above it.

| Module | Does | Depends on |
| --- | --- | --- |
| [packet](src/packet/) | Frame layout: parse, serialize, encode/decode payloads. No heap, no state. | — |
| [crypto/core](src/crypto/core.h) | Primitives and key material: Ed25519, X25519, SHA-256, HMAC, AES-128. | — |
| [crypto/protocol](src/crypto/protocol.h) | MeshCore-specific constructions built from those primitives. | core, packet |
| [identity](src/identity/) | Our keypair and the contacts learned from adverts, plus a shared-secret cache. | core, packet |
| [routing](src/routing/) | Flood and direct routing, deduplication, path learning, acks and retries. | identity, packet, protocol |
| [repeater](src/repeater/) | Transit policy: whether somebody else's packet is carried, and on what terms. | packet |
| [room](src/room/) | The noticeboard: posts, clients, logins, access control, per-client sync, channels. | identity, routing, repeater, protocol |
| [radio](src/radio/) | Airtime, duty cycle, TX queue, and the drivers. The only module that would know about SPI. | platform |
| [platform](src/platform/) | Clock, file store, config, logging — the only system calls in the tree. | — |
| [telemetry](src/telemetry/) | Event bus and counters. Switchable off with a flag; nothing else notices. | platform |

`routing` never reads a clock — time arrives through `tick()`. `room` never sees
a byte off the air. `repeater` reads neither: it is handed a packet, the time and
how much airtime is gone, and answers yes or no. `routing` and `room` publish
telemetry events to a bus and never call telemetry directly.

## Protocol notes

A frame is `header(1) [transport_codes(4)] path_length(1) path(0..64) payload(0..184)`,
255 bytes maximum. The header carries the route type in the low two bits and the
payload type in the next four; `path_length` carries the hop count in six bits and
the per-hop hash size in the other two.

- Adverts are signed with Ed25519 over `public_key || timestamp || appdata` — the
  signature sits between those fields on air, so it is not a contiguous span.
- Direct payloads are sealed with AES-128-ECB, PKCS#7, and HMAC-SHA256 truncated
  to two bytes. The AES key is the head of the X25519 shared secret, taken raw.
  There is no IV and no nonce: replay defence belongs to the layer above.
- Deduplication is SHA-256 over the header and payload, with the path left out
  because it grows on every hop.
- A one-byte node hash is ambiguous by design, so a received packet is matched
  against every contact sharing that byte and settled by whose MAC checks out.
- A trace is `tag(4) auth_code(4) flags(1)` and then one signed byte per hop,
  quarter-decibels of SNR. The frame's own path already says who carried it, so
  this says how well each of them heard it; the two are read side by side, and a
  node that cannot append its reading stops the trace rather than sending one
  where the two lists no longer line up.

## Build

Requirements: CMake ≥ 3.16, a C++20 compiler, `pkg-config`, and libsodium
(`libsodium-dev` on Debian/Ubuntu). `lib/json` (nlohmann/json) and `lib/radiolib`
are vendored in-tree — no submodule init needed.

```sh
cmake -S . -B build
cmake --build build -j
```

This produces `build/meshcore-node` and the `meshcore` library the tests link
against. `-DSANITIZE=ON` adds AddressSanitizer and UndefinedBehaviorSanitizer.

## Run

```sh
./build/meshcore-node [config.json]     # defaults to ./meshcore.json
```

The node refuses to start rather than doing something quietly wrong: an
unreadable identity is never replaced with a fresh one (the network would see a
stranger and nobody would notice), and an unsynchronised wall clock is fatal
because a node advertising itself dated 1970 corrupts its neighbours' records.
`SIGINT`/`SIGTERM` save contacts and room state on the way out.

The contact file is at version 2, which keeps what the radio heard — when last,
how well, how many hops away — beside what each advert claimed. Version 1 files
are read and upgraded on the next save, their missing fields left empty until
the next advert fills them in. Going back to an older build after that loses the
contact list, not the identity.

Configuration is [meshcore.json](meshcore.json); every key has a default:

| Key | Meaning |
| --- | --- |
| `node.dir` | Where the identity, contacts and room state live |
| `node.name` | Name advertised to the network |
| `node.type` | `room` or `repeater` — what the network is told this node is for. Every node carries other people's packets whichever it says |
| `node.flush_ms` | How often state is written (lazily — a write per advert would wear out the card) |
| `node.advert_ms` | Advert interval |
| `log.level` | `error`, `warn`, `info`, `debug` |
| `radio.driver` | `udp` or `virtual` |
| `radio.udp_bind`, `radio.udp_port` | Local socket; port `0` lets the kernel choose |
| `radio.udp_peers` | `"host:port"` each — who hears this node |
| `radio.udp_group`, `radio.udp_group_port` | Optional multicast, so a LAN needs no peer list |
| `radio.frequency`, `radio.spreading_factor`, `radio.bandwidth`, `radio.coding_rate` | LoRa parameters; these must match the network bit for bit |
| `radio.duty_cycle` | Percent per sliding hour (10 on 868 MHz in Europe) |
| `routing.forward_airtime_factor`, `routing.forward_jitter` | Transit delay: airtime times the first, plus a random spread up to the second |
| `routing.max_routes` | Learned direct routes kept at once |
| `routing.seen_slots`, `routing.seen_ttl_ms` | The duplicate cache: how many packets are remembered, and for how long |
| `repeater.enabled` | Whether other people's packets travel on at all |
| `repeater.max_hops` | Transit stops past this many hops, below the 63 the format allows |
| `repeater.per_source_per_minute` | Floods carried per neighbour per minute |
| `repeater.duty_ceiling` | Permille of the sliding hour past which transit stands aside. `0` switches the check off |
| `repeater.blocked` | Node hashes not carried for, `"1f"` each |
| `room.admin_password`, `room.guest_password` | Empty means that role cannot log in |
| `room.anonymous_read` | Let strangers read without a password |
| `room.channels` | `"name:key"` each, the key 64 hex characters. Up to four |
| `telemetry.enabled`, `telemetry.queue`, `telemetry.report_ms` | Event ring and reporting interval |

### Admin commands

An admin — someone who logged in with `room.admin_password` — can drive the node
over the air with CLI text messages. A command is never acknowledged: its reply
*is* the acknowledgement, so every path through the parser answers, refusals
included. A command whose timestamp runs backwards is dropped as a replay; the
same timestamp resent is answered again, because a client that saw no reply
resends the identical frame.

| Command | Does |
| --- | --- |
| `help` | Lists the verbs |
| `ver` | Firmware version |
| `clock` | Reports the time as `HH:MM:SS D/M/YYYY UTC` |
| `time <epoch>` | Sets the wall clock. Needs a privileged process, and anything before 2020 is refused |
| `advert` | Floods an advert now |
| `set name <text>` | Renames the node and announces it |
| `set password <text>` | New admin password. Empty closes the role |
| `set guest.password <text>` | The same for guests |
| `set anonymous.read on\|off` | Reading without a password |
| `set repeat on\|off` | Whether other people's packets travel on |
| `set hops.max <n>` | How far a carried packet may have travelled already, 1..63 |
| `get stats` | Posts, clients, uptime, packets carried and refused, airtime spent |
| `get transit` | The same refusals broken out: hop limit, blocked, rate, budget |
| `get neighbors` | Who this node hears first-hand, with signal and how long ago |
| `get name`, `get time` | Read one value back |
| `clear posts` | Empties the noticeboard, leaving the clients' bookmarks alone |
| `clear stats` | Zeroes the transit counters. Changes nothing about what is carried |
| `reboot` | Exits after the reply is on its way; the supervisor restarts the process |

A post is pushed to a client as `name: text`, the name coming from the advert
that introduced the author and capped so the longest name plus the longest text
still fit one frame — the text is what somebody wrote, so it is the name that
gives way. An author no advert has been heard from becomes `?a1b2c3d4`, and a
post that arrived on a channel has no author at all and gets no prefix.

### Carrying other people's packets

Every node here is a repeater as well as a board. A packet addressed to this
node still travels on — the nodes behind it may have missed the original — and
that is a separate branch in the receive path, never an `else`. What `node.type`
changes is only what the advert claims: nothing on air says whether a node
repeats, and clients look for a room server.

Flood picks up our hash on the way through and stops if it comes back; direct is
carried only when we are the next hop, and our hash is dropped from what remains.
Either way transit waits behind everything of ours in the transmit queue, because
repeating a stranger must never delay an answer we owe a client.

What is not carried, and why, is the whole of [repeater](src/repeater/):

- past `repeater.max_hops`, because a packet that has been through a dozen nodes
  is going in circles;
- more than `repeater.per_source_per_minute` floods from one neighbour, so a
  wedged transmitter cannot spend the node's whole airtime. Direct packets are
  not counted: they follow a route somebody already learned and arrive one at a
  time, and they are the traffic this node exists to carry;
- anything whose path touches a hash in `repeater.blocked`;
- everything, once `repeater.duty_ceiling` of the sliding hour has gone. Our own
  replies keep their air, which is the point of standing aside early rather than
  letting the duty cycle stop us with a queue already full of strangers.

Each of those is counted apart from the others, because "the repeater dropped
four thousand packets" is not an answer and which counter moved is the
diagnosis. `get transit` reads them back; `set repeat off` stops transit
altogether and survives a restart, like every other setting an admin changes.

A duplicate is remembered by hash for `routing.seen_ttl_ms`, not merely until
the ring wraps: on a busy node `routing.seen_slots` entries wrap in seconds, and
an echo arriving to a clean cache is forwarded a second time. The age also has
to expire, or the same packet legitimately resent minutes later is swallowed.

### Channels

A channel is a key and a name, configured as one string per channel:

```json
"room": { "channels": ["public:000102...1f"] }
```

Anyone holding the key is a member. There is no roster, no login and no
signature, so a channel message may only ever become a post — it can never log
anybody in, carry a command, or move a client's bookmark. What the board
receives from a logged-in client goes back out to every channel, so the two do
not drift apart; what arrives on a channel is never sent back to one, which is
where the loop would be.

The channel hash on air is the first byte of `SHA-256(key)`, so both ends derive
it without being told. One byte is ambiguous by design, the same as a node hash:
every channel with a matching byte is tried and the MAC decides.

`routing` holds no channel keys and does not try to guess whether a group
message is for this node — it hands every one of them up and forwards them all.

### The overlay

A `set` command has to outlive a restart, or the config file would undo it at
the next start. The node never writes to that file: it is often not writable at
all — root-owned, laid down by configuration management, mounted read-only —
and the parser here flattens it to text and could not reproduce it. What a
command changed goes to `<node.dir>/overrides.json` instead, and is laid over
the config at startup:

```json
{
  "node.name": "Radio Hut",
  "room.admin_password": "something-else"
}
```

Keys are the config's own dotted names, so an entry shadows exactly the key it
is named after; the nested spelling works too. The overlay wins wherever the two
disagree, which is why the node says so on startup:

```
[WARN] 2 setting(s) come from ./data/overrides.json, not the config
```

Delete the file and everything reverts to what the config declares. A damaged
overlay is fatal rather than ignored: coming up with half the settings an admin
believes are in force, a password among them, is worse than not coming up.
`node.dir` is the one key an overlay cannot shadow — the file lives inside it.

### Radio drivers

`udp` carries frames over UDP between processes on one machine or hosts on a
LAN, keeping the real airtime and duty-cycle accounting. Each node's peer list
is the visibility matrix, so a chain A–B–C is three configs rather than a special
mode. `virtual` is the in-process shared medium the tests use. There is no
SX1262 driver yet; RadioLib is vendored for it.

### A network on one machine

[network.sh](network.sh) generates a config and data directory per node and runs
them all, labelling every log line with the node it came from. Ctrl-C stops the
network with `SIGTERM`, so state is saved.

```sh
./network.sh                       # three nodes, full mesh
./network.sh -n 5 -t chain         # five nodes in a line, 1-2-3-4-5
./network.sh -n 5 -t chain -r 2,3,4  # the middle of that chain as repeaters
./network.sh -n 4 -t star -v       # hub plus three leaves, debug logging
./network.sh -c                    # wipe ./run first, so every node starts a stranger
```

## Tests

Nine suites, one per module, on a [minimal harness](test/check.h) with no
framework and no dependencies. The virtual radio and the fake clock are what
make them fast: a multi-node routing test runs in one process, and a timeout test
does not take twelve seconds.

```sh
cd build && ctest --output-on-failure
```

Crypto is pinned to published vectors rather than to itself, so a rewrite that
changes behaviour fails instead of agreeing with the new bug.

## Formatting

`.clang-format` is authoritative. [format.sh](format.sh) rewrites the tree;
[check-format.sh](check-format.sh) fails without touching it.

## License

MIT — see [LICENSE](LICENSE).

---

<a name="meshcore-русский"></a>

# MeshCore (русский)

Сервер комнаты MeshCore для Linux: узел LoRa-меша, который держит небольшую доску
объявлений, переносит чужие пакеты и может быть запущен целой сетью процессов на
одном ноутбуке.

Стек написан так, чтобы каждый слой собирался и тестировался без соседних сверху
и снизу. Пакеты разбираются без криптографии, криптография ничего не знает о
пакетах, маршрутизация ничего не знает о сообщениях на доске, и никто, кроме
`platform`, не делает системных вызовов. За сборку всего этого платят один раз —
в [main.cpp](src/main.cpp).

## Модули

Зависимости направлены только вниз; ни один модуль не называет тот, что над ним.

| Модуль | Что делает | Зависит от |
| --- | --- | --- |
| [packet](src/packet/) | Разметка кадра: разбор, сериализация, кодирование и декодирование нагрузок. Ни кучи, ни состояния. | — |
| [crypto/core](src/crypto/core.h) | Примитивы и ключевой материал: Ed25519, X25519, SHA-256, HMAC, AES-128. | — |
| [crypto/protocol](src/crypto/protocol.h) | Конструкции, специфичные для MeshCore, собранные из этих примитивов. | core, packet |
| [identity](src/identity/) | Наша пара ключей и контакты, выученные из объявлений, плюс кеш общих секретов. | core, packet |
| [routing](src/routing/) | Лавинная и прямая маршрутизация, дедупликация, обучение маршрутам, подтверждения и повторы. | identity, packet, protocol |
| [repeater](src/repeater/) | Транзитная политика: понесём ли мы чужой пакет и на каких условиях. | packet |
| [room](src/room/) | Доска объявлений: сообщения, клиенты, вход, права доступа, синхронизация по клиентам, каналы. | identity, routing, repeater, protocol |
| [radio](src/radio/) | Эфирное время, рабочий цикл, очередь передачи и драйверы. Единственный модуль, который знал бы про SPI. | platform |
| [platform](src/platform/) | Часы, файловое хранилище, конфигурация, логирование — единственные системные вызовы в дереве. | — |
| [telemetry](src/telemetry/) | Шина событий и счётчики. Выключается флагом; остальные этого не замечают. | platform |

`routing` никогда не читает часы — время приходит через `tick()`. `room` не видит
ни байта из эфира. `repeater` не читает ни того, ни другого: ему передают пакет,
время и то, сколько эфирного времени израсходовано, а он отвечает «да» или «нет».
`routing` и `room` публикуют события телеметрии в шину и никогда не вызывают
телеметрию напрямую.

## Заметки о протоколе

Кадр это `header(1) [transport_codes(4)] path_length(1) path(0..64) payload(0..184)`,
максимум 255 байт. В заголовке младшие два бита — тип маршрутизации, следующие
четыре — тип нагрузки; в байте `path_length` шесть бит занимает число переходов,
остальные два — размер хеша на переход.

- Объявления подписываются Ed25519 по `public_key || timestamp || appdata` — в
  эфире подпись лежит между этими полями, так что подписываемые данные не идут
  подряд.
- Прямые нагрузки запечатываются AES-128-ECB, PKCS#7 и HMAC-SHA256, усечённым до
  двух байт. Ключ AES это начало общего секрета X25519, взятое как есть. Ни IV,
  ни nonce: защита от повторного воспроизведения — забота слоя выше.
- Дедупликация это SHA-256 по заголовку и нагрузке, без пути, потому что путь
  растёт на каждом переходе.
- Однобайтовый хеш узла неоднозначен по замыслу, поэтому принятый пакет
  сопоставляется с каждым контактом, у которого этот байт совпал, а решает то, у
  кого сойдётся MAC.
- Трассировка это `tag(4) auth_code(4) flags(1)`, а затем по одному знаковому
  байту на переход — четверти децибела SNR. Собственный путь кадра уже говорит,
  кто его нёс, поэтому здесь записано, насколько хорошо каждый из них его слышал;
  их читают рядом, и узел, который не может дописать свой замер, останавливает
  трассировку, а не отправляет такую, где два списка перестали совпадать.

## Сборка

Требуется: CMake ≥ 3.16, компилятор C++20, `pkg-config` и libsodium
(`libsodium-dev` в Debian/Ubuntu). `lib/json` (nlohmann/json) и `lib/radiolib`
лежат прямо в дереве — инициализировать подмодули не нужно.

```sh
cmake -S . -B build
cmake --build build -j
```

Так получаются `build/meshcore-node` и библиотека `meshcore`, с которой
линкуются тесты. `-DSANITIZE=ON` добавляет AddressSanitizer и
UndefinedBehaviorSanitizer.

## Запуск

```sh
./build/meshcore-node [config.json]     # по умолчанию ./meshcore.json
```

Узел скорее откажется стартовать, чем сделает что-то тихо неправильное:
нечитаемая идентичность никогда не заменяется свежей (сеть увидела бы незнакомца,
и никто бы этого не заметил), а несинхронизированные астрономические часы
фатальны, потому что узел, объявляющий себя с датой 1970 года, портит записи
соседей. `SIGINT`/`SIGTERM` сохраняют контакты и состояние комнаты при выходе.

Файл контактов версии 2: он хранит то, что услышало радио (когда в последний раз,
насколько хорошо, за сколько переходов), рядом с тем, что заявляло каждое
объявление. Файлы версии 1 читаются и обновляются при следующем сохранении, а
недостающие поля остаются пустыми, пока их не заполнит очередное объявление.
Возврат после этого на старую сборку теряет список контактов, но не идентичность.

Конфигурация это [meshcore.json](meshcore.json); у каждого ключа есть значение по
умолчанию:

| Ключ | Смысл |
| --- | --- |
| `node.dir` | Где живут идентичность, контакты и состояние комнаты |
| `node.name` | Имя, объявляемое сети |
| `node.type` | `room` или `repeater` — что сети сообщают о назначении узла. Чужие пакеты переносит любой узел, что бы там ни стояло |
| `node.flush_ms` | Как часто записывается состояние (лениво — запись на каждое объявление износила бы карту) |
| `node.advert_ms` | Интервал между объявлениями |
| `log.level` | `error`, `warn`, `info`, `debug` |
| `radio.driver` | `udp` или `virtual` |
| `radio.udp_bind`, `radio.udp_port` | Локальный сокет; порт `0` выбирает ядро |
| `radio.udp_peers` | По `"host:port"` каждый — кто слышит этот узел |
| `radio.udp_group`, `radio.udp_group_port` | Необязательный multicast, чтобы локальной сети не нужен был список соседей |
| `radio.frequency`, `radio.spreading_factor`, `radio.bandwidth`, `radio.coding_rate` | Параметры LoRa; должны совпадать с сетью бит в бит |
| `radio.duty_cycle` | Проценты за скользящий час (10 на 868 МГц в Европе) |
| `routing.forward_airtime_factor`, `routing.forward_jitter` | Задержка транзита: эфирное время, умноженное на первое, плюс случайный разброс до второго |
| `routing.max_routes` | Сколько выученных прямых маршрутов держать одновременно |
| `routing.seen_slots`, `routing.seen_ttl_ms` | Кеш дубликатов: сколько пакетов помнить и как долго |
| `repeater.enabled` | Идут ли чужие пакеты дальше вообще |
| `repeater.max_hops` | Транзит прекращается после такого числа переходов; меньше, чем 63, которые допускает формат |
| `repeater.per_source_per_minute` | Сколько лавинных пакетов переносится на соседа в минуту |
| `repeater.duty_ceiling` | Промилле скользящего часа, после которых транзит отходит в сторону. `0` отключает проверку |
| `repeater.blocked` | Хеши узлов, ради которых пакеты не переносятся, по `"1f"` каждый |
| `room.admin_password`, `room.guest_password` | Пустой означает, что эта роль войти не может |
| `room.anonymous_read` | Разрешить незнакомцам читать без пароля |
| `room.channels` | По `"name:key"` каждый, ключ — 64 шестнадцатеричных символа. До четырёх |
| `telemetry.enabled`, `telemetry.queue`, `telemetry.report_ms` | Кольцо событий и интервал отчётов |

### Админские команды

Администратор — тот, кто вошёл с `room.admin_password`, — может управлять узлом
по эфиру текстовыми CLI-сообщениями. Команда никогда не подтверждается: ответ на
неё *и есть* подтверждение, поэтому отвечает каждый путь через разборщик, включая
отказы. Команда, чья метка времени идёт назад, отбрасывается как повтор; на ту же
метку, присланную повторно, отвечают снова, потому что клиент, не увидевший
ответа, шлёт тот же самый кадр.

| Команда | Что делает |
| --- | --- |
| `help` | Перечисляет команды |
| `ver` | Версия прошивки |
| `clock` | Сообщает время в виде `HH:MM:SS D/M/YYYY UTC` |
| `time <epoch>` | Устанавливает астрономические часы. Нужен привилегированный процесс, всё раньше 2020 года отвергается |
| `advert` | Немедленно рассылает объявление |
| `set name <text>` | Переименовывает узел и объявляет об этом |
| `set password <text>` | Новый пароль администратора. Пустой закрывает роль |
| `set guest.password <text>` | То же для гостей |
| `set anonymous.read on\|off` | Чтение без пароля |
| `set repeat on\|off` | Идут ли чужие пакеты дальше |
| `set hops.max <n>` | Сколько переходов уже мог пройти переносимый пакет, 1..63 |
| `get stats` | Сообщения, клиенты, время работы, перенесённые и отклонённые пакеты, потраченное эфирное время |
| `get transit` | Те же отказы по отдельности: лимит переходов, блокировка, частота, бюджет |
| `get neighbors` | Кого этот узел слышит напрямую, с уровнем сигнала и давностью |
| `get name`, `get time` | Прочитать одно значение |
| `clear posts` | Очищает доску, не трогая закладки клиентов |
| `clear stats` | Обнуляет транзитные счётчики. Ничего не меняет в том, что переносится |
| `reboot` | Завершается после того, как ответ ушёл; процесс перезапускает супервизор |

Сообщение отправляется клиенту в виде `имя: текст`, где имя берётся из
объявления, которым автор представился, и ограничено так, чтобы самое длинное имя
вместе с самым длинным текстом всё ещё помещались в один кадр: текст — это то,
что человек написал, поэтому уступает имя. Автор, от которого не слышали
объявления, становится `?a1b2c3d4`, а у сообщения, пришедшего из канала, автора
нет вовсе и префикс не добавляется.

### Перенос чужих пакетов

Каждый узел здесь одновременно и доска, и ретранслятор. Пакет, адресованный этому
узлу, всё равно идёт дальше — узлы за ним могли не услышать оригинал, — и это
отдельная ветка на пути приёма, а не `else`. `node.type` меняет только то, что
заявляет объявление: в эфире нет ничего, что говорило бы, ретранслирует ли узел,
а клиенты ищут сервер комнаты.

Лавинный пакет по дороге забирает наш хеш и останавливается, если возвращается
обратно; прямой переносится, только когда следующий переход — мы, и наш хеш из
остатка пути убирается. В любом случае транзит в очереди передачи ждёт позади
всего нашего, потому что ретрансляция незнакомца не должна задерживать ответ,
который мы должны клиенту.

Что не переносится и почему — это и есть весь [repeater](src/repeater/):

- дальше `repeater.max_hops`, потому что пакет, прошедший десяток узлов, ходит по
  кругу;
- больше `repeater.per_source_per_minute` лавинных пакетов от одного соседа,
  чтобы заклинивший передатчик не мог потратить всё эфирное время узла. Прямые
  пакеты не считаются: они идут по уже выученному кем-то маршруту, приходят по
  одному, и ради этого трафика узел и существует;
- всё, чей путь задевает хеш из `repeater.blocked`;
- всё вообще, как только израсходовано `repeater.duty_ceiling` скользящего часа.
  Нашим собственным ответам эфир остаётся — в этом и смысл отойти в сторону
  заранее, а не дать рабочему циклу остановить нас с очередью, уже полной чужого.

Каждый из этих случаев считается отдельно, потому что «ретранслятор отбросил
четыре тысячи пакетов» — не ответ, а диагноз это то, какой счётчик сдвинулся.
`get transit` показывает их; `set repeat off` останавливает транзит целиком и
переживает перезапуск, как и всякая другая настройка, изменённая администратором.

Дубликат помнится по хешу в течение `routing.seen_ttl_ms`, а не просто до
оборота кольца: на нагруженном узле `routing.seen_slots` записей оборачиваются за
секунды, и эхо, пришедшее в уже очищенный кеш, пересылается второй раз. Возраст
тоже обязан истечь, иначе тот же пакет, законно отправленный повторно через
несколько минут, будет проглочен.

### Каналы

Канал это ключ и имя, задаваемые одной строкой на канал:

```json
"room": { "channels": ["public:000102...1f"] }
```

Участник — любой, у кого есть ключ. Ни списка участников, ни входа, ни подписи,
поэтому сообщение из канала может стать только сообщением на доске — оно не может
никого впустить, нести команду или сдвинуть закладку клиента. То, что доска
получает от вошедшего клиента, уходит во все каналы, чтобы они не расходились;
то, что пришло из канала, обратно в канал не отправляется никогда — вот где
образовалась бы петля.

Хеш канала в эфире это первый байт `SHA-256(ключ)`, так что обе стороны выводят
его без договорённостей. Один байт неоднозначен по замыслу, как и хеш узла:
перебираются все каналы с совпавшим байтом, а решает MAC.

`routing` не хранит ключей каналов и не пытается угадать, нам ли адресовано
групповое сообщение, — он отдаёт наверх каждое из них и пересылает их все.

### Overlay

Команда `set` обязана пережить перезапуск, иначе файл конфигурации отменил бы её
при следующем старте. В этот файл узел не пишет никогда: он часто вообще
недоступен на запись — принадлежит root, разложен системой управления
конфигурацией, смонтирован только на чтение, — а здешний разборщик сплющивает его
в текст и воспроизвести не может. То, что изменила команда, уходит в
`<node.dir>/overrides.json` и накладывается на конфигурацию при старте:

```json
{
  "node.name": "Radio Hut",
  "room.admin_password": "something-else"
}
```

Ключи — это точечные имена самой конфигурации, поэтому запись перекрывает ровно
тот ключ, по которому названа; вложенное написание тоже работает. Overlay
побеждает везде, где они расходятся, и поэтому узел сообщает об этом на старте:

```
[WARN] 2 setting(s) come from ./data/overrides.json, not the config
```

Удалите файл — и всё вернётся к тому, что объявляет конфигурация. Повреждённый
overlay фатален, а не игнорируется: подняться с половиной настроек, которые
администратор считает действующими, включая пароль, хуже, чем не подняться вовсе.
`node.dir` — единственный ключ, который overlay перекрыть не может: сам файл
лежит внутри этого каталога.

### Драйверы радио

`udp` переносит кадры по UDP между процессами на одной машине или узлами в
локальной сети, сохраняя настоящий учёт эфирного времени и рабочего цикла. Список
соседей каждого узла и есть матрица видимости, поэтому цепочка A–B–C это три
конфига, а не особый режим. `virtual` — общая среда внутри процесса, которую
используют тесты. Драйвера SX1262 пока нет; RadioLib для него лежит в дереве.

### Сеть на одной машине

[network.sh](network.sh) создаёт по конфигу и каталогу данных на узел и запускает
их все, помечая каждую строку лога тем узлом, от которого она пришла. Ctrl-C
останавливает сеть через `SIGTERM`, так что состояние сохраняется.

```sh
./network.sh                       # три узла, полносвязная сеть
./network.sh -n 5 -t chain         # пять узлов в линию, 1-2-3-4-5
./network.sh -n 5 -t chain -r 2,3,4  # середина этой цепочки как ретрансляторы
./network.sh -n 4 -t star -v       # центр и три луча, отладочное логирование
./network.sh -c                    # сначала стереть ./run, чтобы каждый узел стартовал незнакомцем
```

## Тесты

Девять наборов, по одному на модуль, на [минимальной обвязке](test/check.h) без
фреймворка и без зависимостей. Быстрыми их делают виртуальное радио и поддельные
часы: тест маршрутизации на нескольких узлах идёт в одном процессе, а тест на
таймаут не занимает двенадцати секунд.

```sh
cd build && ctest --output-on-failure
```

Криптография привязана к опубликованным векторам, а не к самой себе, поэтому
переписывание, меняющее поведение, падает, а не соглашается с новой ошибкой.

## Форматирование

Главный здесь `.clang-format`. [format.sh](format.sh) переписывает дерево;
[check-format.sh](check-format.sh) падает, ничего не трогая.

## Лицензия

MIT — см. [LICENSE](LICENSE).
