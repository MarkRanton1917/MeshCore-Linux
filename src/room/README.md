# room

*The noticeboard. English below, [русский](#room-русский) further down.*

## Purpose

Posts, clients, logins, access control, per-client synchronisation, channels and
the admin CLI.

By this point the lower layers have absorbed the hard parts: `room` never sees a
byte off the air, knows nothing about the radio, and counts no airtime. It works
on decoded objects and one state row per client, which is why the whole module
tests without a network. Time and radio load are injected — `setServerTime`,
`setRadioLoad` — rather than read.

`Room` is the [routing](../routing/) `Delegate`, and it is the delegate on every
node type, not only the ones with a board. Transit is a separate branch and never
an `else`: a packet addressed to us is still carried on, if a
[repeater](../repeater/) policy is attached to say so.

Which of the three node types this is comes in as one field, `Config::type`, and
everything else follows from it. Two questions, answered by `keepsBoard(type)`
and `repeats(type)`, and nothing anywhere asks them a third way:

| `NodeType` | `keepsBoard` | `repeats` |
| --- | --- | --- |
| `ROOM` | yes | no |
| `REPEATER` | no | yes |
| `ROOM_REPEATER` | yes | yes |

A `REPEATER` has nothing to post to: posts are refused, channel messages are
dropped, `clear posts` says why, and `get stats` leaves the board out of the line
rather than reporting zeroes. Logins, commands, status replies and transit all
work exactly as they do on a room.

A `ROOM` carries nothing, whatever else is attached: the type is asked before the
policy, so a `forwarder` handed to a room by mistake does not turn it into a
repeater.

Files: [room.h](room.h), [room.cpp](room.cpp). Tests:
[test/test_room.cpp](../../test/test_room.cpp).

## Public API

### `struct Sender`

What `room` needs from routing, as an interface so a test can collect the
outgoing calls in a vector instead of transmitting them.

- `sendDirect(to, type, payload, wantAck) -> SendId`
- `sendFlood(type, payload)` — channel traffic goes to nobody in particular, so it
  floods. There is no ack to wait for and no route to learn: whoever holds the key
  and hears it, has it, and whoever does not is not counted.

### Channels

- **`struct Channel`** — `name`, `secret`, and the one-byte `hash` carried on air.
  Anyone holding the key is a member: no roster, no login, no signature — which is
  exactly why a channel post is never trusted with anything a client login guards.
  It may only ever become a post; it can never log anybody in, carry a command, or
  move a bookmark.
- **`channelHashOf(secret) -> uint8_t`** — derives the on-air hash from the key, so
  both ends agree without being told. Ambiguous by design like a node hash;
  collisions are settled by whose MAC checks out.

### `struct Admin`

What a command needs and the room does not own: the radio, the system clock and
the process itself all sit above it. Optional like the bus — with no host attached
those commands answer "unsupported" rather than pretending.

- `sendAdvert()` — floods a fresh advert. Wanted on demand and after a rename.
- `setClock(unixSeconds)` — `false` when the host refuses, usually not root.
- `nodeName()` / `setNodeName(name)`
- `saveSetting(key, value)` — persists under the same key the config file uses, so
  what an admin changed over the air is still in force after a restart. `false`
  means it could not be stored — and then the room refuses the change outright
  rather than applying one that quietly expires at the next reboot.
- `requestReboot()` — requested, not performed: the loop finishes the tick and
  saves state first.
- `uptime()` — seconds since the process started. The room has no clock of its own.

### Enums

- **`TextType`** — `PLAIN` (somebody posting to the board), `CLI` (an admin
  command, answered rather than acked), `SIGNED` (what the room pushes back out,
  author prefix and all). This is the `txt_type` field; [packet](../packet/)
  carries it as a plain number and never looks at it, because what the values mean
  is the room's business.
- **`RequestType`** — `STATUS`, `KEEP_ALIVE`: the leading byte of a `REQ`
  plaintext. Like the response bodies, these are ours to define — the protocol
  calls a `REQ` payload application data and says nothing about what goes in it.
- **`Access`** — `NONE`, `READ_ONLY` (only when anonymous reading is allowed),
  `GUEST`, `ADMIN`.
- **`Action`** — `READ`, `POST`, `COMMAND`.
- **`NodeType`** — `ROOM`, `REPEATER`, `ROOM_REPEATER`, with `keepsBoard(type)`
  and `repeats(type)` beside it. The composition root asks those same two
  functions when it decides what to advertise and whether to build a transit
  policy at all.

### `struct Post`

- **`seq`** — ours, monotonic, and the only thing synchronisation is allowed to
  count on. Timestamps come from client clocks: two posts can share one to the
  second, and a bookmark that is a timestamp then cannot name the boundary between
  them, so whichever arrived second would be skipped for good.
- `timestamp`, `author` (a four-byte key prefix), `text`.

### `struct Client`

`pk`, `access`, `syncSeq` (everything up to this seq has been handed over), three
separate replay guards — `lastLogin`, `lastCommand`, `lastRequest`, kept apart so
one cannot block another — and the in-flight push: `pending`, `pendingId`,
`pendingSeq`, `retryAfter`.

### `struct Config`

`type` (`ROOM`, `REPEATER` or `ROOM_REPEATER` — see above), `adminPassword`,
`guestPassword` (empty means that role cannot log in), `allowAnonymousRead`,
`clockWindow` (outside it a client's timestamp is replaced with ours, or one post
from the future breaks sorting and everybody else's `syncSince` with it),
`retryDelay`, `firmwareVersion`, `maxLoginAttempts` and `loginLockout` (guessing
a password over the air is slow, but free and unwatched, so a key that keeps
getting it wrong is made to wait), `channels`, and three optional pointers: `bus`
([telemetry](../telemetry/)), `admin`, and `forwarder` ([repeater](../repeater/)
— the terms transit is carried on; absent, nothing is).

### `class Room`

- **`Room(store, sender, config = {})`**
- **`setServerTime(unixSeconds)`** — server time is the truth: a Pi has NTP.
  Injected, never read here.
- **`setRadioLoad(permille)`** — how much of the airtime budget is gone. Used for
  one decision only: whether there is air left for other people's packets.
- **`load(dir)` / `flush()`** — room state on disk. Format version 2 gave posts a
  sequence number of their own; version 1 files are read and upgraded, their
  timestamp bookmarks converted on the way in.
- **The `routing::Delegate` methods** — `onAnon` (a login from a stranger),
  `onPayload`, `onGroup` (channel traffic, tried against every channel whose hash
  byte matches; ignored outright with no board), `onAck`, `onDeliveryFailed`,
  `shouldAck` (false for CLI commands), `shouldForward` (the repeater policy's
  answer, and `false` when there is no policy to ask).
- **`tick(now)`** — retries, pushes, timers.
- **`addPost(author, timestamp, text)`** — the board is a ring: post 33 overwrites
  post 1. A post that came in directly is republished to every channel; a post that
  arrived *on* a channel is never sent back to one — that is the loop, and it does
  not stop.
- **`authenticate(pk, password, timestamp) -> Client*`** — three outcomes: admin,
  guest, neither — and if anonymous reading is on, neither still gets in with
  cut-down rights. Wrong passwords are counted per key, not globally: one shared
  counter would let anybody lock the whole room out by guessing badly on purpose.
- **`handleCommand(client, timestamp, line)`** — admin only, and the reply is the
  whole acknowledgement: a CLI command is never acked, so one that answers nothing
  is indistinguishable from a node that has stopped listening. Every path through
  here replies, refusals included. The verbs are listed in the
  [root README](../../README.md#admin-commands).
- **`nextClientToPush() -> Client*`** — round robin from where we stopped, not from
  the head of the list: otherwise the first client with a bad antenna starves
  everybody behind it.
- **`pushNextPost(client)`** — sends the next post the client has not seen, as
  `name: text`. The name comes from the advert that introduced the author and is
  capped so the longest name plus the longest text still fit one frame — the text
  is what somebody wrote, so it is the name that gives way. An author no advert has
  been heard from becomes `?a1b2c3d4`; a post that arrived on a channel has no
  author and gets no prefix.
- **`can(client, action)`** — the access check, in one place and called from
  three. Scattering the checks is how a new branch ends up without one. `POST` is
  also where `board` is enforced: whether there is anywhere to write is a property
  of the node, not of the client asking.
- **`findClient(pk)`, `clientCount()`, `postCount()`, `posts()`**
- **`unreadFor(client)`** — how much this client has still to receive. Reported in a
  status reply so a client can tell "nothing new" from "the room never heard me".

---

<a name="room-русский"></a>

# room (русский)

## Назначение

Сообщения, клиенты, вход по паролю, права доступа, синхронизация по каждому
клиенту, каналы и админский CLI.

К этому моменту нижние слои уже впитали всё сложное: `room` не видит ни байта из
эфира, ничего не знает о радио и не считает эфирное время. Он работает с
декодированными объектами и одной строкой состояния на клиента — поэтому весь
модуль тестируется без сети. Время и загрузка радио передаются внутрь
(`setServerTime`, `setRadioLoad`), а не читаются.

`Room` — это `Delegate` для [routing](../routing/), причём на узле любого типа, а
не только там, где есть доска. Транзит — отдельная ветка, а не `else`: пакет,
адресованный нам, всё равно идёт дальше, если подключена политика
[repeater](../repeater/), которая это разрешает.

Какой это из трёх типов узла, приходит одним полем `Config::type`, и всё
остальное следует из него. Два вопроса, на которые отвечают `keepsBoard(type)` и
`repeats(type)`, и больше нигде они не задаются третьим способом:

| `NodeType` | `keepsBoard` | `repeats` |
| --- | --- | --- |
| `ROOM` | да | нет |
| `REPEATER` | нет | да |
| `ROOM_REPEATER` | да | да |

У `REPEATER` писать некуда: сообщения отклоняются, сообщения из каналов
отбрасываются, `clear posts` объясняет почему, а `get stats` просто не включает
доску в строку, вместо того чтобы показывать нули. Вход, команды, ответы о
состоянии и транзит работают ровно так же, как в комнате.

`ROOM` не переносит ничего, что бы к нему ни подключили: тип спрашивают раньше
политики, поэтому `forwarder`, отданный комнате по ошибке, не превращает её в
ретранслятор.

Файлы: [room.h](room.h), [room.cpp](room.cpp). Тесты:
[test/test_room.cpp](../../test/test_room.cpp).

## Публичный интерфейс

### `struct Sender`

Что нужно `room` от маршрутизации — в виде интерфейса, чтобы тест мог собирать
исходящие вызовы в вектор вместо передачи в эфир.

- `sendDirect(to, type, payload, wantAck) -> SendId`
- `sendFlood(type, payload)` — трафик канала не адресован никому конкретно,
  поэтому идёт лавиной. Ждать нечего и маршрут учить не у кого: у кого есть ключ и
  кто услышал — тот получил, а кто нет, того и не считают.

### Каналы

- **`struct Channel`** — `name`, `secret` и однобайтовый `hash`, идущий по эфиру.
  Участник — любой, у кого есть ключ: ни списка, ни входа, ни подписи, — и именно
  поэтому сообщению из канала никогда не доверяют ничего из того, что защищено
  входом клиента. Оно может стать только сообщением на доске; оно не может никого
  впустить, нести команду или сдвинуть закладку.
- **`channelHashOf(secret) -> uint8_t`** — выводит эфирный хеш из ключа, так что
  обе стороны получают его без договорённостей. Неоднозначен по замыслу, как и хеш
  узла; коллизии разрешаются тем, у кого сойдётся MAC.

### `struct Admin`

То, что нужно команде и чем комната не владеет: радио, системные часы и сам
процесс находятся выше. Необязателен, как и шина: без подключённого хоста такие
команды отвечают «не поддерживается», а не притворяются.

- `sendAdvert()` — рассылает свежее объявление. Нужно по требованию и после
  переименования.
- `setClock(unixSeconds)` — `false`, когда хост отказывает, обычно из-за
  отсутствия прав root.
- `nodeName()` / `setNodeName(name)`
- `saveSetting(key, value)` — сохраняет настройку под тем же ключом, что и файл
  конфигурации, чтобы изменённое по эфиру осталось в силе после перезапуска.
  `false` означает, что сохранить не удалось, — и тогда комната отказывает в
  изменении вовсе, вместо того чтобы применить то, что тихо исчезнет при
  перезагрузке.
- `requestReboot()` — запрашивается, а не выполняется: цикл сначала дорабатывает
  такт и сохраняет состояние.
- `uptime()` — секунды с запуска процесса. Собственных часов у комнаты нет.

### Перечисления

- **`TextType`** — `PLAIN` (кто-то пишет на доску), `CLI` (админская команда, на
  которую отвечают, а не подтверждают), `SIGNED` (то, что комната рассылает
  обратно, вместе с префиксом автора). Это поле `txt_type`; [packet](../packet/)
  несёт его как обычное число и в него не заглядывает, потому что смысл значений —
  дело комнаты.
- **`RequestType`** — `STATUS`, `KEEP_ALIVE`: первый байт открытого текста `REQ`.
  Как и тела ответов, это наше дело — протокол называет нагрузку `REQ` данными
  приложения и ничего не говорит о её содержимом.
- **`Access`** — `NONE`, `READ_ONLY` (только когда разрешено анонимное чтение),
  `GUEST`, `ADMIN`.
- **`Action`** — `READ`, `POST`, `COMMAND`.
- **`NodeType`** — `ROOM`, `REPEATER`, `ROOM_REPEATER`, рядом с ним
  `keepsBoard(type)` и `repeats(type)`. Композиционный корень спрашивает эти же
  две функции, когда решает, что объявлять и создавать ли политику транзита
  вообще.

### `struct Post`

- **`seq`** — наш, монотонный, и единственное, на что синхронизации разрешено
  опираться. Временные метки приходят с часов клиентов: два сообщения могут
  совпасть с точностью до секунды, и закладка-метка тогда не может назвать границу
  между ними, так что пришедшее вторым было бы пропущено навсегда.
- `timestamp`, `author` (четырёхбайтный префикс ключа), `text`.

### `struct Client`

`pk`, `access`, `syncSeq` (всё до этого номера уже передано), три отдельные защиты
от повтора — `lastLogin`, `lastCommand`, `lastRequest`, разнесённые, чтобы одна не
блокировала другую, — и текущая отправка: `pending`, `pendingId`, `pendingSeq`,
`retryAfter`.

### `struct Config`

`type` (`ROOM`, `REPEATER` или `ROOM_REPEATER` — см. выше), `adminPassword`,
`guestPassword` (пустой означает, что эта роль войти не может),
`allowAnonymousRead`, `clockWindow` (за его пределами метка клиента заменяется
нашей, иначе одно сообщение «из будущего» ломает сортировку, а с ней и `syncSince`
у всех остальных), `retryDelay`, `firmwareVersion`, `maxLoginAttempts` и
`loginLockout` (подбор пароля по эфиру медленный, но бесплатный и никем не
наблюдаемый, поэтому ключ, который всё время ошибается, заставляют ждать),
`channels` и три необязательных указателя: `bus` ([telemetry](../telemetry/)),
`admin` и `forwarder` ([repeater](../repeater/) — условия, на которых идёт
транзит; без него не идёт ничего).

### `class Room`

- **`Room(store, sender, config = {})`**
- **`setServerTime(unixSeconds)`** — время сервера это истина: на Pi есть NTP.
  Передаётся внутрь, здесь его никогда не читают.
- **`setRadioLoad(permille)`** — сколько бюджета эфирного времени израсходовано.
  Используется ровно для одного решения: остался ли эфир на чужие пакеты.
- **`load(dir)` / `flush()`** — состояние комнаты на диске. Версия формата 2 дала
  сообщениям собственные порядковые номера; файлы версии 1 читаются и обновляются,
  а закладки по времени преобразуются на входе.
- **Методы `routing::Delegate`** — `onAnon` (вход от незнакомца), `onPayload`,
  `onGroup` (трафик канала, проверяемый по каждому каналу с совпадающим байтом
  хеша; без доски игнорируется целиком), `onAck`, `onDeliveryFailed`, `shouldAck`
  (false для CLI-команд), `shouldForward` (ответ политики ретрансляции, а без
  политики — `false`).
- **`tick(now)`** — повторы, рассылка сообщений, таймеры.
- **`addPost(author, timestamp, text)`** — доска это кольцо: 33-е сообщение
  затирает первое. Сообщение, пришедшее напрямую, публикуется во все каналы;
  сообщение, пришедшее *из* канала, обратно в канал не уходит никогда — это и есть
  петля, которая сама не останавливается.
- **`authenticate(pk, password, timestamp) -> Client*`** — три исхода: админ,
  гость, никто, — а если включено анонимное чтение, то «никто» всё равно входит с
  урезанными правами. Неверные пароли считаются по ключу, а не глобально: один
  общий счётчик позволил бы кому угодно запереть всю комнату, нарочно ошибаясь.
- **`handleCommand(client, timestamp, line)`** — только для админа, и ответ здесь
  и есть подтверждение: CLI-команда никогда не подтверждается, поэтому команда, не
  ответившая ничего, неотличима от узла, который перестал слушать. Каждый путь
  отсюда отвечает, включая отказы. Список команд — в
  [корневом README](../../README.md#admin-commands).
- **`nextClientToPush() -> Client*`** — по кругу с того места, где остановились, а
  не с начала списка: иначе первый клиент с плохой антенной морит голодом всех, кто
  за ним.
- **`pushNextPost(client)`** — отправляет следующее непрочитанное сообщение в виде
  `имя: текст`. Имя берётся из объявления, которым автор представился, и
  ограничено так, чтобы самое длинное имя вместе с самым длинным текстом всё ещё
  помещались в один кадр: текст это то, что человек написал, поэтому уступает имя.
  Автор, от которого не слышали объявления, становится `?a1b2c3d4`; у сообщения из
  канала автора нет вовсе и префикс не добавляется.
- **`can(client, action)`** — проверка прав, в одном месте и вызываемая из трёх.
  Разбросанные проверки — это то, как новая ветка оказывается без проверки. Здесь
  же, на `POST`, проверяется `board`: есть ли куда писать — свойство узла, а не
  того, кто спрашивает.
- **`findClient(pk)`, `clientCount()`, `postCount()`, `posts()`**
- **`unreadFor(client)`** — сколько этому клиенту ещё предстоит получить.
  Сообщается в ответе о состоянии, чтобы клиент отличал «нового нет» от «комната
  меня вообще не слышала».
