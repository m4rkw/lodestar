# Changelog

## 0.2.1 - 2026-04-23

Web

- Gunicorn master-only UDP listener: new gunicorn.conf.py sets preload_app = True and stamps GUNICORN_MASTER_PID at config-load time (before preload, so the env var is set when main.py is imported). main.py's UDP
thread start is now guarded by a pid check, so the listener runs only in the master — workers inherit state without re-executing module-level code, and the dev-mode watcher fork is skipped. Fixes a worker-restart
crash loop on macOS caused by the Obj-C fork-safety abort triggering when a worker forked while the UDP thread was mid DB call.

## 0.2.0 - 2026-04-23

Breaking release: the telemetry wire format and the web authentication model
have both changed. Devices must be re-provisioned with a PSK and the server
schema must be migrated before upgrading — see below.

### Firmware

- **Encrypted UDP transport**: every telemetry and alert datagram is now a
  ChaCha20-Poly1305 envelope with the shape
  `[1]imei_len | IMEI | [12]nonce | ciphertext | [16]tag`. The IMEI is bound
  in as additional authenticated data so envelopes can't be replayed against a
  different device. Keyed by a per-device 32-byte pre-shared key (`PSK_HEX`,
  64 hex chars in `config.h`; matching `psk` column on the server). New
  `crypto.ino`, `chacha20_poly1305.c/h`, and `gen_psk.py` helper.
- **Encrypted server responses**: the UDP reply is now a plain
  `nonce | ciphertext | tag` envelope (IMEI as AAD). Decrypted plaintext is a
  compact CSV `1,int,ao,ma[,cmd]` (first field is the literal status digit;
  slim variant is `1,int[,cmd]` when neither `ALWAYS_ON_POWER` nor
  `RELAY_CONNECTED` is set) — no more JSON.
- **TCP/HTTP support dropped entirely**: removed `USE_UDP`/`PROTO` switch and
  every HTTP code path (`gsm_send_http_current`, `alert_send_http`,
  `gsm_send_data_current`, `gsm_validate_tcp`, `gsm_wait_for_ack`,
  `gsm_http_config`, `parse_receive_reply`). `HTTP_PORT` renamed to
  `UDP_PORT`; `PACKET_SIZE`/`PACKET_SIZE_DELIVERY`/`HTTP_HEADER*` deleted.
  The legacy `parse.ino` file and the one-off HTTP config pull at boot are
  both gone. The cleanup recovers ~2.7 KB of flash, partially offsetting the
  ChaCha20-Poly1305 library's footprint.
- **Auth header retired**: devices no longer send the legacy `IMEI<TAB>KEY`
  header or the `X-KEY` HTTP header. The `KEY` compile-time define and the
  `config.key` field have been removed from the settings struct; settings
  files written by older firmware are silently re-initialised on load.
- Tests: new `test_crypto` suite (RFC 8439 vector + round-trip, tamper, and
  wrong-key cases); `test_parse` removed.

### Web

- **Passkey (WebAuthn) login** replaces the shared `web_password` query-string
  scheme. Registration is by one-shot link generated with the new
  `regtoken.py` CLI; login requires a passkey assertion. Failed-login
  counter with per-account lockout and per-IP rate-limit window.
- **Bearer tokens** for the API and command endpoints replace the previous
  shared secrets (`auth`, `command_key`, `X-Key` header). New
  `gentoken.py` CLI provisions tokens. Dead URL-token code paths removed.
- **Encrypted UDP listener**: packets are decrypted with the device's `psk`
  and the IMEI AAD; unknown IMEI / bad tag / wrong key are dropped with a
  single log line. Legacy `IMEI\tKEY` auth header path removed.
- **Session cookie auth** replaces `web_password`-in-URL across `track`,
  `/api/1.0/car`, replay, and WebSocket paths.
- New `static/js/login.js`, `static/js/register.js`, and `templates/login.tpl`
  / `templates/register.tpl`; `index.tpl` / `index.js` removed.
- `config.yaml.example`: drops `web_password`/`auth`/`command_key`; adds
  `session_secret`. `cryptography` and `webauthn` added to
  `requirements.txt`.
- UDP socket bind now uses exponential backoff retry and exits critically
  after the max attempt count.
- WebSocket payloads carry the raw device timestamp; processed records now
  use server-side current time.
- Security review document added at `web/security.md` covering the gaps this
  release closes.

### Deployment / schema

- `device.device_key` renamed/replaced by `device.psk` (varchar 64 hex).
- New tables: `api_token`, `user`, `registration`, `regoptions`,
  `authoptions`, `authoptions_ip`. Definitions added to
  `deploy/schema.sql`.
- **Migration**: back up the database, migrate each device row (`ALTER TABLE
  device CHANGE device_key psk VARCHAR(64) NOT NULL` and populate with the
  output of `gen_psk.py` per device), then flash the matching `PSK_HEX` into
  each device. After flashing, register at least one passkey user via
  `regtoken.py` before removing the old `web_password` entry from
  `config.yaml`.

## 0.1.2 - 2026-04-18

- Fixed cell carrier not updating in the telemetry when it changes

## 0.1.1 - 2026-04-17

### Firmware

- Cell-tower awareness: new `cell.ino` (URC-driven cache, `+CREG`/`+CEREG`/`+QENG` parsers), cell-as-GPS fallback with `cl=1` flag when no GPS fix, and a `cell_fields_dirty` gate so `mcc/mnc/lac/cid/rat` only ride packets on change or fallback.
- DNS caching: new `dns.ino` and `gsm_resolve_hostname()` — `QIOPEN` uses the cached IP literal when available, cleared on any connect failure.
- Reliability hardening:
  - Pre-STOP2 race guard: re-reads `PIN_S_DETECT` after `attachInterrupt()` and skips STOP2 entry if the ignition edge already fired (fixes the "silent drive" bug where the device would sleep for up to ~18h).
  - `RTC_WAKEUP_MAX_SECONDS` clamp loop: long `loop_interval` values (e.g. 86 400) are split across multiple STOP2 wake cycles.
  - `total_sleep_seconds` accumulator so uptime spans STOP2 (since `millis()` doesn't advance while asleep).
  - GSM send-recovery escalation ladder (`GSM_ESCALATION_POWERCYCLE=3`, `GSM_ESCALATION_SLEEP=5`, `STATE_ERROR_RECOVERY`); legacy single-threshold reboot still available.
  - `should_send_data()` no longer retries after a failed send once ignition is off.
- Telemetry additions on every packet: accelerometer axes (`ax/ay/az`), uptime (`up=`, includes sleep seconds), MCU temperature (`mt=`, via STM32L4 `TS_CAL1/TS_CAL2` factory calibration). Waketime now uses `millis() - wake_start_millis` rather than raw `millis()`.
- Tests: added `test_cell`, `test_dns`, `test_gsm_recovery`; expanded `test_data` with 13 new tests covering cell/accel/uptime/mcu_temp paths; added `test_power` simulator for the RTC clamp loop; added `HIGH_FREQUENCY_TELEMETRY` knob to `generated_config.h`.

### Web

- CSV ingest: field 11 renamed `uptime` → `waketime`; accel/uptime/mcu_temp/cell fields moved into trailing extras groups.
- PLMN lookup: new `lookup_operator(mcc, mnc)` helper (`SELECT operator FROM plmn`) used in track view, replay stream, `carpos` API, and WebSocket broadcasts to tag every point with a carrier name. Replay uses a per-batch `operator_cache` so PLMN is queried once per distinct `(mcc, mnc)` pair.
- New `POST /api/1.0/home` endpoint: Haversine distance from a configurable home coordinate, Pushover alert when the device is outside the radius and the garage flag is 0. Disabled unless `home_check:` is set in `config.yaml`.
- DB write path: carries forward last-known `mcc/mnc/lac/cid/rat` when the current packet omits them (firmware only emits on change or cell-location fallback); `cell_location` stays per-packet. `INSERT INTO log` column list gains 12 new columns (`mcc, mnc, lac, cid, cell_location, rat, accel_x, accel_y, accel_z, waketime, uptime, mcu_temp`).
- Config/commands: `ga` (garage) added to `GET /api/1.0/config` and `garage` added to the alarm command map.
- Track view switched from `gsm_timestamp` to `timestamp` (log write time, avoids UDP transmission drift); date and time formatted server-side as `dd.mm.yyyy` / `HH:MM:SS`. `operator` and `rat` passed to template and returned from replay/carpos.
- Trips listing returns `from_place`/`to_place`.

### Track UI

- Header reorganised into `.line`/`.bigline` rows with new `span.operator`, `span.rat`, `span.date` elements.
- Replay overlay split into `#replay-topbar` (status + stop) and `#replay-playbar` (back / play-pause / forward + progress slider). New seekable, pausable replay engine:
  - Play/pause toggle with glyph swap.
  - Slider drag pauses while dragging and resumes on release.
  - `seekReplay()` with clamping; back/forward buttons skip `REPLAY_SKIP` (10) frames; auto-resume past the end rewinds to 0.
  - `showReplayPoint(idx)` decoupled from `replayStep()` so rendering is shared between playback and seeking.
- `fetchLivePosition()` helper called on page load and after replay stop (replaces an inline ready-handler fetch).
- Voltage alert uses a `.red` class instead of a broken inline `font-color` style.
- `convert_utc_to_local()` helper removed — timestamps arrive pre-formatted from the server.
- Trip list rows show a `from_place → to_place` sub-line (DOM-escaped).

### Config

- `config.yaml.example` documents optional `home_check:` block (`key`, `latitude`, `longitude`, `radius_m`).

## 0.1.0 — 2026-04-11

Initial release.
