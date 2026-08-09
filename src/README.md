# src

*Module index and the top-level files. English below, [русский](#src-русский)
further down.*

## Layout

One directory per module, each with a README of its own. Dependencies point
downward only; no module names the one above it.

| Module | Does |
| --- | --- |
| [packet](packet/) | Frame layout: parse, serialize, encode/decode payloads. No heap, no state. |
| [crypto](crypto/) | Primitives (`core`), MeshCore constructions (`protocol`), and the AES-128 block cipher. |
| [identity](identity/) | Our keypair, the contacts learned from adverts, and a shared-secret cache. |
| [routing](routing/) | Flood and direct routing, deduplication, path learning, acks and retries. |
| [repeater](repeater/) | Transit policy: whether somebody else's packet is carried, and on what terms. |
| [room](room/) | The noticeboard: posts, clients, logins, access control, per-client sync, channels. |
| [radio](radio/) | Airtime, duty cycle, TX queue, and the drivers. The only module that would know about SPI. |
| [platform](platform/) | Clock, file store, config, logging — the only system calls in the tree. |
| [telemetry](telemetry/) | Event bus and counters. Switchable off with a flag; nothing else notices. |

The [root README](../README.md) has the protocol notes, the configuration
reference and the admin command list.

## Top-level files

### [defines.h](defines.h)

Every fixed size and bound in one file: frame and payload limits, key and digest
sizes, payload prefix sizes, advert appdata flags, the contact and room store
limits and format versions, the repeater's bucket count. Also the two aliases
everything else is written in terms of — `ByteView` (`span<const uint8_t>`) and
`ByteSpan` (`span<uint8_t>`).

The values marked *fixed by the protocol* — `PACKET_MAC_SIZE`,
`PACKET_CIPHER_KEY_SIZE`, `PACKET_CIPHER_BLOCK_SIZE` — cannot be changed without
breaking interoperability. The rest are this node's own limits.

### [main.cpp](main.cpp)

The wiring, paid for once. It reads the config and the overlay, builds the store,
the radio, the router, the repeater policy and the room, connects them, and runs
the loop. Everything in it is either an adapter between two modules or process
plumbing:

- **`RadioLink`** — `routing::Radio` on top of a `radio::IRadio`, mapping routing
  priorities onto radio priorities.
- **`Receiver`** — `radio::RxSink` into the router. It also picks adverts out of
  the stream before handing the frame on: it verifies the signature, remembers
  the contact, and passes the signal and hop count to
  `identity::Store::noteHeard`, because an advert is the one packet that names
  its sender in the clear. Our own advert, handed back by a neighbour that
  forwarded it, is dropped rather than recorded.
- **`RouterSender`** — `room::Sender` on top of the router.
- **`HostAdmin`** — `room::Admin`: adverts on demand, the wall clock, the node
  name, persisting a setting to the overlay, the reboot request, and uptime.
- Config helpers: `channelsFrom`, `blockedFrom`, `nodeTypeFromName`,
  `levelFromName`, `buildAdvertPayload`, `queueDepthOf`, `usedPermilleOf`.
- `main` — startup checks (an unreadable identity is never replaced; an
  unsynchronised wall clock is fatal), the tick loop, and `SIGINT`/`SIGTERM`
  handling that saves contacts and room state on the way out.

---

<a name="src-русский"></a>

# src (русский)

## Состав

По каталогу на модуль, у каждого свой README. Зависимости направлены только вниз;
ни один модуль не называет тот, что над ним.

| Модуль | Что делает |
| --- | --- |
| [packet](packet/) | Разметка кадра: разбор, сериализация, кодирование и декодирование нагрузок. Ни кучи, ни состояния. |
| [crypto](crypto/) | Примитивы (`core`), конструкции MeshCore (`protocol`) и блочный шифр AES-128. |
| [identity](identity/) | Наша пара ключей, контакты из объявлений и кеш общих секретов. |
| [routing](routing/) | Лавинная и прямая маршрутизация, дедупликация, обучение маршрутам, подтверждения и повторы. |
| [repeater](repeater/) | Транзитная политика: понесём ли мы чужой пакет и на каких условиях. |
| [room](room/) | Доска объявлений: сообщения, клиенты, вход, права, синхронизация по клиентам, каналы. |
| [radio](radio/) | Эфирное время, рабочий цикл, очередь передачи и драйверы. Единственный модуль, который знал бы про SPI. |
| [platform](platform/) | Часы, файловое хранилище, конфигурация, логирование — единственные системные вызовы в дереве. |
| [telemetry](telemetry/) | Шина событий и счётчики. Выключается флагом; остальные этого не замечают. |

В [корневом README](../README.md) — заметки о протоколе, справочник по
конфигурации и список админских команд.

## Файлы верхнего уровня

### [defines.h](defines.h)

Все фиксированные размеры и границы в одном файле: пределы кадра и нагрузки,
размеры ключей и хешей, размеры префиксов нагрузок, флаги appdata объявления,
пределы и версии форматов хранилищ контактов и комнаты, число корзин
ретранслятора. А также два псевдонима, в терминах которых написано всё
остальное, — `ByteView` (`span<const uint8_t>`) и `ByteSpan` (`span<uint8_t>`).

Значения, помеченные как *заданные протоколом* — `PACKET_MAC_SIZE`,
`PACKET_CIPHER_KEY_SIZE`, `PACKET_CIPHER_BLOCK_SIZE`, — нельзя менять, не сломав
совместимость. Остальные это собственные ограничения этого узла.

### [main.cpp](main.cpp)

Сборка, оплаченная один раз. Читает конфигурацию и overlay, создаёт хранилище,
радио, маршрутизатор, политику ретрансляции и комнату, соединяет их и крутит
цикл. Всё, что там есть, — это либо переходник между двумя модулями, либо
обвязка процесса:

- **`RadioLink`** — `routing::Radio` поверх `radio::IRadio`, отображает
  приоритеты маршрутизации на приоритеты радио.
- **`Receiver`** — `radio::RxSink` в маршрутизатор. Он же выхватывает объявления
  из потока до передачи кадра дальше: проверяет подпись, запоминает контакт и
  передаёт уровень сигнала и число переходов в `identity::Store::noteHeard`,
  потому что объявление — единственный пакет, который называет своего
  отправителя открыто. Наше собственное объявление, вернувшееся от соседа,
  который его переслал, отбрасывается, а не записывается.
- **`RouterSender`** — `room::Sender` поверх маршрутизатора.
- **`HostAdmin`** — `room::Admin`: объявления по требованию, астрономические
  часы, имя узла, сохранение настройки в overlay, запрос на перезагрузку и время
  работы.
- Помощники по конфигурации: `channelsFrom`, `blockedFrom`, `nodeTypeFromName`,
  `levelFromName`, `buildAdvertPayload`, `queueDepthOf`, `usedPermilleOf`.
- `main` — проверки на старте (нечитаемая идентичность никогда не заменяется;
  несинхронизированные астрономические часы фатальны), цикл тактов и обработка
  `SIGINT`/`SIGTERM`, сохраняющая контакты и состояние комнаты при выходе.
