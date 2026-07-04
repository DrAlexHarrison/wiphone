# WiPhone → Meshtastic node — full build plan

**Goal:** keep the WiPhone's UI and phone functions (SIP calling, phonebook, Messages
screen) exactly as they are, and make its LoRa daughterboard speak the **real
Meshtastic protocol** so it interoperates with a T-Deck and any other Meshtastic node —
replacing the stock proprietary point-to-point LoRa messaging.

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
- WiPhone node IDs are already 32-bit MAC-derived, so they map 1:1 onto Meshtastic
  `NodeNum`s and the existing `LORA:%X` address format is reused unchanged.

---

## The wire format we're matching (all verified against meshtastic/firmware master)

| Layer | Value (US LongFast default) | Source |
|---|---|---|
| Frequency | **906.875 MHz** (slot 20) | RadioInterface.cpp freq calc + docs |
| Bandwidth / SF / CR | **250 kHz / SF11 / 4:5** | LongFast preset |
| Sync word | **0x2B** | all Meshtastic nets |
| Preamble | 16 | RadioInterface |
| Header (16 B, packed, LE) | `to,from,id` u32 + `flags,channel,next_hop,relay_node` u8 | RadioInterface.h `PacketHeader` |
| `flags` | hop_limit in bits[0..2] (use 3) | PACKET_FLAGS_HOP_LIMIT_MASK |
| `channel` | **0x08** = xorHash("LongFast") ^ xorHash(PSK) | Channels.cpp generateHash |
| Encryption | **AES-128-CTR** | CryptoEngine |
| Key (default "AQ==") | `d4 f1 bb 3a 20 29 07 59 f0 bc ff ab cf 4e 69 01` | Channels.cpp defaultpsk |
| Nonce (16 B) | `packetId(8 LE) ‖ fromNode(4 LE) ‖ counter(4=0)` | CryptoEngine::initNonce |
| Payload | `Data` protobuf `{portnum=1, payload=<utf8>}`, encrypted | mesh.proto / portnums.proto |

For a text message the `Data` protobuf is just: `08 01 12 <len> <utf8 bytes>` — so it's
hand-rolled and **nanopb is not needed**.

---

## Phases

### Phase 0 — hardware sanity (Nick, ~20 min, no code)
- Confirm the **915 MHz LoRa daughterboard (RFM95W)** is present and seated (the bare
  WiPhone is WiFi-only). Confirm his T-Deck is on **US / LongFast / default channel**.

### Phase 1 — migrate WiPhone firmware Arduino IDE → PlatformIO/VS Code
Prerequisite, not polish: you can't manage RadioLib + build flags sanely in Arduino IDE.
- Use the provided `platformio.ini`. Move sources into `src/`, rename `WiPhone.ino →
  src/main.cpp`, add `#include <Arduino.h>`, add function prototypes where `.ino` implicit
  ordering was relied on. Move `data/` for SPIFFS. **Deliverable: stock firmware builds &
  boots unchanged in PlatformIO.** (Migration gotchas enumerated in `platformio.ini`.)

### Phase 2 — swap the radio stack (THIS IS DONE — see `lora.h` / `lora.cpp`)
Drop-in replacement of the `Lora` class internals: RadioHead → RadioLib SX1276, stock
protocol → Meshtastic packets (header + encrypted Data protobuf). Public API identical, so
no other file changes. Covers TX (`send_message`) and RX (`loop`) of **text messages** on
the default channel, broadcast + direct.

### Phase 3 — on-air bring-up (Nick + Claude, iterative — the real loop)
Flash, then verify against the T-Deck. Expected checkpoints:
1. WiPhone boots, serial shows `Meshtastic LoRa up: node=0x… ch=0x08 906.875MHz SF11`.
2. Send from WiPhone → appears on the T-Deck as a message from a new node.
3. Send from T-Deck (broadcast) → lands in the WiPhone Messages screen.
This is where the physical loop lives: if nothing arrives, the usual suspects are
frequency/region, the channel-hash byte, or SPI pin mapping — all isolated in `lora.cpp`
constants. Budget a few flash→test rounds.

### Phase 4 — mesh niceties (optional, after texting works)
- Show sender short-names (needs the `NodeInfo` portnum, portnum=4) instead of raw hex.
- Multi-hop relay (rebroadcast decremented-hop packets) to be a full relay node, not just
  an endpoint.
- ACKs / delivery confirmation (want_ack flag + routing app).
- Position/GPS if a GPS daughterboard is attached.

---

## Honest status & the couple of real unknowns (not hedging — these are the things
## only on-hardware testing can close)

- **DONE:** full protocol implementation, compile-ready, wire-format-correct by
  construction (constants pulled from current Meshtastic source).
- **RX interrupt API:** RadioLib pinned to 6.x → `setPacketReceivedAction`. If you use an
  older RadioLib, it's `setDio0Action(fn, RISING)`. (Noted inline.)
- **DIO1 / RST not wired on the WiPhone** (`RFM95_RST=-1`, no DIO1 pin). Basic TX/RX only
  needs DIO0, which *is* wired (GPIO38). Advanced RadioLib timeouts that want DIO1 aren't
  used here. Low risk, but the one genuinely hardware-dependent thing to watch on first RX.
- **Multi-block CTR counter:** short texts are a single AES block (order-independent).
  Longer messages span blocks; mbedtls and Meshtastic both increment the 128-bit counter
  big-endian from the same initial nonce, so keystreams match — worth an explicit test with
  a >16-char message.
- **Footprint:** adds RadioLib (moderate flash); AES is ESP32 hardware via mbedtls (already
  in the core); protobuf hand-rolled (no nanopb). This fits comfortably alongside the
  existing SIP/GUI firmware — not the RAM scare it first looked like.

## Files in this kit
- `lora.h`, `lora.cpp` — the Meshtastic-speaking drop-in `Lora` class (Phase 2, done).
- `platformio.ini` — the VS Code/PlatformIO project + migration notes (Phase 1).
- `PLAN.md` — this file.
