# crypto

*Primitives and MeshCore constructions. English below, [русский](#crypto-русский)
further down.*

## Purpose

Two layers, deliberately separate:

- **[core.h](core.h)** — cryptographic primitives and key material: Ed25519,
  X25519, SHA-256, HMAC-SHA256, AES-128, secure random, secure wipe, key files.
  It knows nothing about what a MeshCore packet looks like.
- **[protocol.h](protocol.h)** — the MeshCore-specific constructions: what gets
  fed into those primitives, in which order, and truncated to what. Everything a
  protocol revision could change lives here and only here. Depends on `core`;
  `core` never depends on it.
- **[aes128.h](aes128.h)** — the raw FIPS-197 block cipher, in C. libsodium does
  not expose AES-128 in any form (only AES-256-GCM, and only with AES-NI), so
  the block cipher is in-tree. **Block cipher only** — modes and padding live in
  `core.cpp`; protocol code must never call it directly.

Files: [core.h](core.h)/[core.cpp](core.cpp),
[protocol.h](protocol.h)/[protocol.cpp](protocol.cpp),
[aes128.h](aes128.h)/[aes128.c](aes128.c). Tests:
[test/test_crypto.cpp](../../test/test_crypto.cpp), pinned to published vectors
(FIPS-197 Appendix B / C.1, RFC 2104, RFC 8032) rather than to itself, so a
rewrite that changes behaviour fails instead of agreeing with the new bug.

## Public API — `crypto::core`

### Key material types

- **`Bytes<N>`** — fixed-length *public* data, with `view()`, `span()`, `begin()`,
  `end()`.
- **`Secret<N>`** — the same, but wiped on destruction. For everything secret.
  Exposes only `view()` and `span()`.
- Aliases: `PublicKey` (Ed25519, also the node id), `Signature`, `Hash`
  (SHA-256/HMAC output), `MontPublic` (X25519 after conversion); and, secret,
  `PrivateKey`, `MontPrivate`, `SharedSecret`, `CipherKey` (AES-128), `Seed`.
- **`KeyPair`** — `pk` and `sk` together.
- **`RandomFn`** — `void(*)(ByteSpan)`, a replaceable random source.

### Functions

- **`secureZero(buf)`** — a wipe the optimiser is not allowed to drop. Declared
  first in the header because `Secret`'s inline destructor calls it.
- **`constantTimeEqual(a, b)`** — mandatory for MAC checks; length is not treated
  as a secret.
- **`init(fn)`** — starts the backend. `fn` replaces the random source (tests pass
  a deterministic one), `nullptr` restores the system source.
- **`randomBytes(out)`** — cryptographically strong random bytes.
- **`generateKeypair()`** — a node identity from a fresh 32-byte seed.
- **`loadKeypair(kp, path)`** / **`saveKeypair(kp, path)`** — the identity file.
  Loading rejects a file of the wrong size, corrupt, or readable by others;
  saving uses mode `0600` and **never overwrites an existing file**, because a
  node that quietly changes identity is seen by the whole network as a stranger.
- **`deriveShared(sk, pk) -> optional<SharedSecret>`** — X25519. Symmetric;
  `nullopt` on a degenerate peer key. Expensive — cache it per contact rather
  than deriving it per packet (which is what [identity](../identity/) does).
- **`sha256(chunks)` / `sha256(message)`** — the chunked form hashes the pieces in
  order without joining the buffers first.
- **`hmacSha256(key, message)`** — RFC 2104, key of any length.
- **`sign(sk, message)` / `verify(pk, message, sig)`** — Ed25519 detached
  signature. Deterministic: the same input always signs the same.
- **`aesEncrypt(key, in, out)` / `aesDecrypt(key, in, out)`** — AES-128-ECB over
  whole blocks; `in` and `out` may be the same buffer. `false` if the length is
  not a whole number of blocks or `out` is too small.

## Public API — `crypto::protocol`

- **`Mac`** — `Bytes<PACKET_MAC_SIZE>`, the HMAC truncated to what the packet
  carries (2 bytes).
- **`Sealed`** — what `seal()` returns: the `mac` and the `ciphertextLength`.
- **`cipherKeyFrom(secret)`** — the AES key is the head of the shared secret,
  taken raw, with no KDF.
- **`packetHash(frame)`** — the deduplication key: SHA-256 over the header and the
  payload. The path is left out on purpose — it grows on every hop.
- **`packetSign(sk, frame)`** — signs the advert fields: public key, timestamp and
  appdata.
- **`packetVerify(frame)`** — checks an advert against the key it carries. Proves
  authorship, not freshness; the replay guard is the timestamp check in
  [identity](../identity/).
- **`seal(secret, plaintext, out) -> optional<Sealed>`** — AES-128-ECB, PKCS#7,
  HMAC-SHA256 cut to two bytes. No IV and no nonce, so repeats are visible on air
  and replay defence belongs to the layer above. `out` must hold the plaintext
  rounded up to a whole block.
- **`open(secret, mac, ciphertext, out) -> optional<size_t>`** — checks the MAC in
  constant time, then decrypts; returns the plaintext length. `nullopt` means
  "not for us" *or* "corrupt", indistinguishable on purpose. About one foreign
  packet in 256 lands here by hash collision, so log at debug and count it, never
  warn.
- **`expectedAck(payload, recipient)`** — the delivery receipt: SHA-256 over the
  payload and the recipient key, first four bytes. The payload must be the
  ciphertext that went on air, never the plaintext. A correlation id, not
  authentication — anyone who saw the packet can forge it.

## Public API — `aes128.h` (C)

- **`aes128_ctx`** — the expanded key schedule, 11 round keys.
- **`aes128_init(ctx, key)`** — expands a 16-byte key.
- **`aes128_encrypt_block(ctx, in, out)` / `aes128_decrypt_block(...)`** — a
  single ECB block; `in` and `out` may alias.
- **`aes128_clear(ctx)`** — wipes the key schedule.

---

<a name="crypto-русский"></a>

# crypto (русский)

## Назначение

Два слоя, намеренно разделённые:

- **[core.h](core.h)** — криптографические примитивы и ключевой материал:
  Ed25519, X25519, SHA-256, HMAC-SHA256, AES-128, стойкая случайность, надёжное
  затирание, файлы ключей. Он ничего не знает о том, как выглядит пакет MeshCore.
- **[protocol.h](protocol.h)** — конструкции, специфичные для MeshCore: что и в
  каком порядке подаётся в примитивы и до чего усекается. Всё, что может
  измениться с ревизией протокола, живёт здесь и только здесь. Зависит от `core`;
  `core` от него — никогда.
- **[aes128.h](aes128.h)** — «сырой» блочный шифр по FIPS-197, на C. libsodium не
  предоставляет AES-128 ни в каком виде (только AES-256-GCM и только при наличии
  AES-NI), поэтому блочный шифр лежит в дереве. **Только блочный шифр** — режимы
  и дополнение находятся в `core.cpp`; код протокола не должен вызывать его
  напрямую.

Файлы: [core.h](core.h)/[core.cpp](core.cpp),
[protocol.h](protocol.h)/[protocol.cpp](protocol.cpp),
[aes128.h](aes128.h)/[aes128.c](aes128.c). Тесты:
[test/test_crypto.cpp](../../test/test_crypto.cpp) — привязаны к опубликованным
векторам (FIPS-197 Приложение B / C.1, RFC 2104, RFC 8032), а не к самим себе,
чтобы переписывание, меняющее поведение, падало, а не соглашалось с новой ошибкой.

## Публичный интерфейс — `crypto::core`

### Типы ключевого материала

- **`Bytes<N>`** — *публичные* данные фиксированной длины, с `view()`, `span()`,
  `begin()`, `end()`.
- **`Secret<N>`** — то же самое, но затирается при разрушении. Для всего
  секретного. Наружу отдаёт только `view()` и `span()`.
- Псевдонимы: `PublicKey` (Ed25519, он же идентификатор узла), `Signature`,
  `Hash` (результат SHA-256/HMAC), `MontPublic` (X25519 после преобразования); и
  секретные `PrivateKey`, `MontPrivate`, `SharedSecret`, `CipherKey` (AES-128),
  `Seed`.
- **`KeyPair`** — `pk` и `sk` вместе.
- **`RandomFn`** — `void(*)(ByteSpan)`, заменяемый источник случайности.

### Функции

- **`secureZero(buf)`** — затирание, которое оптимизатору не разрешено выбросить.
  Объявлено первым в заголовке, потому что его вызывает встроенный деструктор
  `Secret`.
- **`constantTimeEqual(a, b)`** — обязательно для проверки MAC; длина секретом не
  считается.
- **`init(fn)`** — запускает бэкенд. `fn` заменяет источник случайности (тесты
  передают детерминированный), `nullptr` возвращает системный.
- **`randomBytes(out)`** — криптографически стойкие случайные байты.
- **`generateKeypair()`** — идентичность узла из свежего 32-байтного seed.
- **`loadKeypair(kp, path)` / `saveKeypair(kp, path)`** — файл идентичности.
  Загрузка отвергает файл неверного размера, повреждённый или доступный на чтение
  посторонним; сохранение использует режим `0600` и **никогда не перезаписывает
  существующий файл**, потому что узел, тихо сменивший идентичность, для всей сети
  выглядит незнакомцем.
- **`deriveShared(sk, pk) -> optional<SharedSecret>`** — X25519. Симметрично;
  `nullopt` при вырожденном ключе собеседника. Дорого — кешируйте на контакт, а
  не вычисляйте на каждый пакет (именно это делает [identity](../identity/)).
- **`sha256(chunks)` / `sha256(message)`** — форма со списком хеширует куски по
  порядку, не склеивая буферы заранее.
- **`hmacSha256(key, message)`** — RFC 2104, ключ любой длины.
- **`sign(sk, message)` / `verify(pk, message, sig)`** — отделённая подпись
  Ed25519. Детерминированная: один и тот же вход всегда даёт одну и ту же подпись.
- **`aesEncrypt(key, in, out)` / `aesDecrypt(key, in, out)`** — AES-128-ECB по
  целым блокам; `in` и `out` могут быть одним буфером. `false`, если длина не
  кратна блоку или `out` мал.

## Публичный интерфейс — `crypto::protocol`

- **`Mac`** — `Bytes<PACKET_MAC_SIZE>`, HMAC, усечённый до того, что несёт пакет
  (2 байта).
- **`Sealed`** — то, что возвращает `seal()`: `mac` и `ciphertextLength`.
- **`cipherKeyFrom(secret)`** — ключ AES это начало общего секрета, взятое как
  есть, без KDF.
- **`packetHash(frame)`** — ключ дедупликации: SHA-256 по заголовку и нагрузке.
  Путь намеренно исключён — он растёт на каждом переходе.
- **`packetSign(sk, frame)`** — подписывает поля объявления: открытый ключ,
  временную метку и appdata.
- **`packetVerify(frame)`** — проверяет объявление по ключу, который оно же несёт.
  Доказывает авторство, но не свежесть; защита от повтора — это проверка
  временной метки в [identity](../identity/).
- **`seal(secret, plaintext, out) -> optional<Sealed>`** — AES-128-ECB, PKCS#7,
  HMAC-SHA256, урезанный до двух байт. Ни IV, ни nonce, поэтому повторы видны в
  эфире, а защита от повторного воспроизведения — забота слоя выше. `out` должен
  вмещать открытый текст, округлённый вверх до целого блока.
- **`open(secret, mac, ciphertext, out) -> optional<size_t>`** — проверяет MAC за
  постоянное время, затем расшифровывает; возвращает длину открытого текста.
  `nullopt` означает «не нам» *или* «повреждено», и это намеренно неразличимо.
  Примерно один чужой пакет из 256 попадает сюда по коллизии хеша, поэтому
  логировать на debug и считать, но никогда не предупреждать.
- **`expectedAck(payload, recipient)`** — квитанция о доставке: SHA-256 по
  нагрузке и ключу получателя, первые четыре байта. Нагрузкой обязан быть
  шифротекст, ушедший в эфир, а не открытый текст. Это идентификатор для
  сопоставления, а не аутентификация: подделать его может любой, кто видел пакет.

## Публичный интерфейс — `aes128.h` (C)

- **`aes128_ctx`** — развёрнутое расписание ключей, 11 раундовых ключей.
- **`aes128_init(ctx, key)`** — разворачивает 16-байтный ключ.
- **`aes128_encrypt_block(ctx, in, out)` / `aes128_decrypt_block(...)`** — один
  блок ECB; `in` и `out` могут совпадать.
- **`aes128_clear(ctx)`** — затирает расписание ключей.
