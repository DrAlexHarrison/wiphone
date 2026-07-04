# WiPhone → Meshtastic node — plan, wire format & engineering log

**Goal:** keep the WiPhone's UI and phone functions (SIP calling, phonebook, Messages
screen) exactly as they are, and make its LoRa daughterboard speak the **real
Meshtastic protocol** so it interoperates with a T-Deck and any other Meshtastic node —
replacing the stock proprietary point-to-point LoRa messaging.

Quickstart, feature list and bring-up checklist live in
[`README-MESHTASTIC.md`](README-MESHTASTIC.md). This file is the deep dive.

**Why this is tractable (verified in source, not assumed):**
- The WiPhone LoRa daughterboard is a **HopeRF RFM95W = Semtech SX1276**, which Meshtastic
  supports. Not a hardware wall — a firmware/protocol wall.
- The entire LoRa integration surface in the WiPhone firmware is **one class (`Lora`) with
  three methods** and **one call into the message store**. Nothing in the 389 KB `GUI.cpp`
  or the SIP stack needs to change. Confirmed call sites:
  - `WiPhone.ino:60` `static Lora lora;`
  - `WiPhone.ino:913` `lora.setup();`
  - `WiPhone.ino:1819` `if (lora.loop())`
  - `WiPhone.ino:1830` `lora.send_message(msg->getOtherUri(), msg->getMessageText());`
  - RX stores via `gui.flash.messages.saveMessage(text, "LORA:<hex>", ...)` and the UI
    already routes `LORA:` addresses (`GUI.cpp` 6798–6984).
- WiPhone node IDs are already 32-bit MAC-derived (`chipId`), so they map 1:1 onto
  Meshtastic `NodeNum`s and the existing `LORA:%X` address format is reused unchanged.

---

## The wire format we're matching (verified against meshtastic/firmware master)

| Layer | Value (US LongFast default) | Source |
|---|---|---|
| Frequency | **906.875 MHz** (slot 20 = djb2("LongFast") % 104) | RadioInterface.cpp freq calc |
| Bandwidth / SF / CR | **250 kHz / SF11 / 4:5** | LongFast preset |
| Sync word | **0x2B** | all Meshtastic nets |
| Preamble | 16 symbols | RadioInterface |
| Header (16 B, packed, LE) | `to,from,id` u32 + `flags,channel,next_hop,relay_node` u8 | RadioInterface.h `PacketHeader` |
| `flags` | hop_limit[0..2], want_ack[3], via_mqtt[4], hop_start[5..7] | PACKET_FLAGS_* masks |
| `channel` | **0x08** = xorHash("LongFast") ^ xorHash(PSK) | Channels.cpp generateHash |
| Encryption | **AES-128-CTR** | CryptoEngine |
| Key (default "AQ==") | `d4 f1 bb 3a 20 29 07 59 f0 bc ff ab cf 4e 69 01` | Channels.cpp defaultpsk |
| Nonce (16 B) | `packetId(8 LE) ‖ fromNode(4 LE) ‖ counter(4=0)` | CryptoEngine::initNonce |
| Payload | `Data` protobuf, encrypted | mesh.proto / portnums.proto |

Protobufs are hand-rolled (**no nanopb**) — the transport speaks exactly three:

| Message | Port | Encoding used |
|---|---|---|
| Text | 1 | `Data{portnum=1, payload=<utf8>, bitfield=OK_TO_MQTT}` |
| NodeInfo | 4 | `Data{portnum=4, payload=User{id,long,short,hw_model=PRIVATE_HW}}` |
| ACK/NAK | 5 | `Data{portnum=5, payload=Routing{error_reason}, request_id=<id>}` |

The RX-side `Data` walker skips unknown fields by wire type, so emoji tapbacks,
reply_id, PKI markers etc. from current firmware parse cleanly instead of crashing
the decode.

---

## Status by phase

### ✅ Phase 1 — Arduino IDE → PlatformIO  (DONE, builds green)
`pio run` compiles the **entire stock firmware + the new transport** into a flashable
image: `RAM 26.1%, Flash 91.9%` on arduino-esp32 1.0.6 (`espressif32@3.5.0` — same core
era as the "WiPhone by ESP32" v0.14 board package; the code uses 1.x APIs like
`SYSTEM_EVENT_*` and legacy I2S, so don't bump the platform casually).
The only migration fixes needed, both in `platformio.ini`:
- `build_unflags = -Werror=reorder` (Arduino IDE built with warnings suppressed;
  the PIO core build script forces this warning to an error and the stock GUI trips it)
- `min_spiffs.csv` partitions (fits the 1.8 MB app + OTA + SPIFFS on any 4 MB flash;
  `partitions-wiphone-16mb.csv` provided for boards with the full 16 MB chip)

### ✅ Phase 2 — Meshtastic transport (DONE + adversarially reviewed)
Drop-in `Lora` class rewrite: RadioHead → RadioLib SX1276 on real hardware HSPI,
stock magic-number protocol → Meshtastic packets. Public API identical; zero changes
outside `lora.h` / `lora.cpp`.

**Vet pass results** — the first draft claimed "compile-ready"; a line-by-line review
against RadioLib's API and the stock call sites found and fixed **three real bugs**
before any hardware was touched:
1. `radio->readData()` returns a **status code**, not a length — the draft treated 0
   (success) as "no data", so RX would never have delivered a single message. Length
   now comes from `getPacketLength()`.
2. The GUI strips the `LORA:` prefix before queueing an outgoing message
   (`GUI.cpp:6969`), so the draft's `strchr(to, ':')` parse never matched — **every DM
   would have silently gone out as a broadcast**. Now parses bare hex, tolerates both.
3. `extern NtpClock ntpClock` — the class is `Clock` (`clock.h:136`). Wouldn't compile.

Plus hardening the draft lacked: TxDone-vs-RxDone IRQ disambiguation (DIO0 is shared),
duplicate-packet dedup, current limit raised for the +20 dBm PA (stock ran RadioHead
level 23; default OCP of 60 mA would brown the PA_BOOST rail).

### ✅ Phase 4 — mesh citizenship (DONE, pulled forward)
Originally "later polish", but each is 20–80 lines once the packet plumbing exists,
and together they're the difference between "science project" and "a node you'd
actually hunt with":
- **NodeInfo** broadcast on boot + every 3 h + introduce-on-first-contact + reply to
  `want_response` — the WiPhone shows up *named* on every node list
- **ACKs**: responds to `want_ack` DMs (incl. re-ACK of duplicates when our ACK was
  lost), so senders get delivery checkmarks
- **Reliable DMs**: `want_ack` + retransmit ×2 with backoff; cancels on ACK, NAK, or
  hearing a relay carry the packet (implicit ACK, matches ReliableRouter semantics)
- **Managed-flood relay**: rebroadcasts others' packets (hop-1, random 0.3–1.3 s
  stagger, `relay_node` stamped) — including foreign-channel traffic, which relays
  forward without decrypting. `MESH_RELAY 0` opts out.
- **Dedup**: 32-entry (from,id) ring — flooded copies never reach the Messages UI
- **Node DB**: 16 names learned from NodeInfo, used in logs (`Mesh RX text from
  Nick-TDeck …` instead of hex)

### 🔜 Phase 3 — on-air bring-up (Nick + hardware — the only remaining phase)
Flash, then verify against the T-Deck (checklist in README-MESHTASTIC.md). The honest
unknowns that only hardware can close, in likelihood order:
1. **First RX** — DIO0 (GPIO38) is the only wired radio interrupt (no RST, no DIO1).
   TX/RX needs only DIO0, but this is the untested seam.
2. **TX at +20 dBm** — if the phone browns out on send, drop `MESH_TXPOWER` to 17.
3. **Multi-block CTR** — messages >16 chars exercise counter continuation; mbedtls and
   Meshtastic both increment the counter big-endian from the same nonce, verified in
   both sources, worth one explicit long-message test.

### Later (documented, deliberately not built)
PKI DMs (2.5+ public-key encryption — peers fall back to channel-key DMs
automatically), position/GPS, telemetry, traceroute, MQTT, a "Mesh Nodes" WiPhone app
fed by the node DB. The `Data` walker + `buildPacket()` make each a small add.

---

## Files
- `WiPhone/lora.h`, `WiPhone/lora.cpp` — the Meshtastic transport (the whole change)
- `platformio.ini` — build config (Arduino IDE no longer needed)
- `partitions-wiphone-16mb.csv` — optional full-flash layout
- `README-MESHTASTIC.md` — quickstart / bring-up / knobs
- `MESHTASTIC-PLAN.md` — this file
