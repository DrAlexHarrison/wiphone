# WiPhone × Meshtastic — start here

This branch makes a WiPhone's LoRa daughterboard speak the **real Meshtastic
protocol**. The phone UI, SIP calling, phonebook and Messages screen are 100%
stock — but LoRa texts now interoperate with a LILYGO T-Deck and every other
Meshtastic node on the default **US / LongFast** channel.

```
   WiPhone UI (unchanged)                      the mesh
  ┌────────────────────────┐               ┌──────────────┐
  │ Messages / phonebook   │   Meshtastic  │  T-Deck      │
  │   └── Lora class ──────┼───packets────►│  Heltec      │
  │        (lora.cpp/.h)   │   906.875 MHz │  RAK, phone  │
  └────────────────────────┘   SF11 LongFast  apps, ...   │
     RFM95W daughterboard                  └──────────────┘
```

The entire change is **two files** (`WiPhone/lora.cpp`, `WiPhone/lora.h`) plus
this PlatformIO project. The stock firmware's `Lora` class API is preserved
exactly, so nothing else in the 389 KB GUI or the SIP stack was touched.

## What the WiPhone does on the mesh

- **Texts, both ways** — broadcast and direct messages, default channel,
  AES-128-CTR encrypted (ESP32 hardware AES)
- **Shows up as a named node** — broadcasts NodeInfo ("WiPhone a1b2"), answers
  info requests, so your T-Deck's node list shows a name, not raw hex
- **Delivery checkmarks** — ACKs direct messages it receives, and retransmits
  its own DMs until they're ACKed or a relay takes over
- **A real mesh citizen** — relays other nodes' packets (managed flood), so a
  WiPhone in camp *extends* everyone's range instead of leeching
- **No duplicate spam** — flooded packets are deduped before the Messages UI

No nanopb, no Meshtastic firmware fork: the three protobufs the mesh actually
needs (Data / User / Routing) are hand-rolled in ~80 lines, and encryption is
the ESP32's mbedtls hardware AES. The whole transport is ~550 lines you can
read in one sitting.

## Build & flash

Prereqs: [VS Code + the PlatformIO extension](https://platformio.org/install/ide?install=vscode),
or CLI-only: `pip install platformio`.

```bash
git clone -b meshtastic-lora https://github.com/DrAlexHarrison/wiphone.git
cd wiphone
pio run                 # build
pio run -t upload       # flash over USB
pio device monitor      # 115200 baud — watch the bring-up
```

(If your board has the 16 MB flash chip — check the "flash size" line esptool
prints during upload — switch to `partitions-wiphone-16mb.csv` in
`platformio.ini` for 4 MB of message storage and roomier OTA slots.)

Within a few seconds of boot you should see:

```
Meshtastic LoRa up: node !a1b2c3d4 ch 0x08 906.875 MHz SF11 BW250 pwr 20dBm
```

~20 s later the WiPhone announces itself; it should appear in your T-Deck's
node list as **WiPhone a1b2**.

## First contact with your T-Deck (5-minute checklist)

1. T-Deck on stock Meshtastic firmware, region **US**, channel **LongFast**,
   default key — i.e. factory settings.
2. Boot the WiPhone, keep the serial monitor open.
3. **T-Deck → WiPhone:** send a channel (broadcast) message from the T-Deck.
   Serial shows `Mesh RX text from …` and it lands in the WiPhone Messages app.
4. **WiPhone → T-Deck:** Messages → create message → send as **LoRa** → address
   is the T-Deck's node id — the 8 hex digits of its `!a1b2c3d4` id, without
   the `!` (find it in the Meshtastic app under the node's details). Leave the
   address as `0` to broadcast to the whole channel instead.
5. Watch for `Mesh DM 0x… DELIVERED — ACK from …` in serial, and the ✓ on the
   T-Deck side.

Tip: save your T-Deck in the WiPhone phonebook with address `LORA:<its hex id>`
and it becomes a normal contact you can message like anyone else.

## Knobs (top of `WiPhone/lora.cpp`)

| Constant | Default | Change it when… |
|---|---|---|
| `MESH_LONG_NAME` / `MESH_SHORT_NAME` | auto: "WiPhone a1b2" / "a1b2" | you want a custom name on the mesh |
| `MESH_FREQ` | 906.875 (US LongFast) | you're outside the US — EU_868 is 869.525 |
| `MESH_CHANNEL_NAME` / `MESH_KEY` | LongFast / default PSK | your group runs a private channel |
| `MESH_TXPOWER` | 20 dBm | you see brownouts/resets during TX → try 17 |
| `MESH_RELAY` | 1 | set 0 to stop relaying (saves battery, weakens the mesh) |

## Serial log cheat-sheet (bring-up lives here)

| Line | Meaning |
|---|---|
| `Meshtastic LoRa up: …` | radio init OK — SPI + daughterboard are alive |
| `Meshtastic LoRa init FAILED: -2` | radio not responding — daughterboard seated? |
| `Mesh node discovered: !… "name"` | heard a NodeInfo — you're receiving packets |
| `Mesh RX text from … rssi -87 snr 9.5: hi` | a text arrived (rssi/snr = link quality) |
| `Mesh TX text -> broadcast id …` | your message went on the air |
| `Mesh DM … DELIVERED — ACK from …` | direct message confirmed received |
| `Mesh DM … picked up by a relay` | a node between you took over delivery |
| `Mesh DM … NOT delivered` | out of retries — target off/out of range |

## Honest status & known limits

**This branch has not been flashed on real hardware yet.** The protocol side is
implemented from current Meshtastic source (wire header, crypto, protobufs,
channel hash all verified), but radio bring-up is physical work. The three
things to watch on first boot, in order of likelihood:

1. **First RX** — the daughterboard only wires DIO0 (GPIO38); RST and DIO1
   aren't connected. TX/RX needs only DIO0, but this is the untested seam.
2. **TX power** — stock firmware drove this PA at RadioHead level 23; we default
   to +20 dBm with the current limit raised to match. If the phone resets when
   sending, drop `MESH_TXPOWER` to 17.
3. **Long messages** — >1 AES block (>16 chars) exercises the CTR counter
   continuation; verified in mbedtls semantics, worth one explicit test.

Design limits (deliberate, documented, all addable later):

- **No PKI DMs** — Meshtastic 2.5+ prefers public-key encryption for direct
  messages; the WiPhone doesn't advertise a public key, so peers fall back to
  channel-key DMs automatically. Your T-Deck will show its "unencrypted DM"
  padlock icon on these. Expected, not a bug.
- **Blocking TX** — the UI freezes ~0.5–2 s while a packet is on the air at
  SF11 (same as stock firmware behavior).
- **Not implemented:** position/GPS, telemetry, traceroute responses, MQTT,
  channel scanning. The `Data` walker already parses everything current
  firmware sends, so adding a port is ~20 lines each.

## Provenance

Base: WiPhone firmware v0.8.30 (playfultechnology mirror of the vaporware'd
official firmware). The Meshtastic transport was written and reviewed with
Claude (Anthropic) against `meshtastic/firmware` master, July 2026 — including
an adversarial second pass that caught and fixed three real bugs (RX length
handling, DM addressing, a type mismatch) before any hardware was flashed.
Full wire-format derivation + phased plan: [`MESHTASTIC-PLAN.md`](MESHTASTIC-PLAN.md).
