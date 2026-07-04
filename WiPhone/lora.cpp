/*
  WiPhone LoRa transport — Meshtastic-compatible rewrite (see lora.h).

  Replaces the stock WiPhone lora.cpp. Same Lora class API (setup / loop /
  send_message), but the bytes on the air are real Meshtastic packets, so the
  WiPhone interoperates with a T-Deck and any other Meshtastic node on the
  default US LongFast channel — and behaves like a proper mesh citizen:
  named node, ACKs, reliable DMs, flood relay, packet dedup.

  Region/channel/names are the compile-time constants below.
*/

#include "lora.h"
#include "GUI.h"
#include "clock.h"
#include "mbedtls/aes.h"
#include <esp_system.h>       // esp_random()

// Globals provided by WiPhone.ino / clock.h (unchanged stock symbols):
extern uint32_t chipId;       // MAC-derived 32-bit id -> used as Meshtastic NodeNum
extern GUI      gui;          // gui.flash.messages.saveMessage(...)
                              // ntpClock comes from clock.h (extern Clock ntpClock)

// ============================================================================
//  Tunables — the things Nick actually changes
// ============================================================================

// Region + modem preset. Defaults = US (902-928) LongFast, the Meshtastic default.
// EU users: MESH_FREQ 869.525 (EU_868 LongFast). Other regions: see the
// frequency-slot table in https://meshtastic.org/docs/overview/radio-settings/
static const float    MESH_FREQ     = 906.875;  // MHz — US LongFast slot 20 = djb2("LongFast") % 104
static const float    MESH_BW       = 250.0;    // kHz
static const uint8_t  MESH_SF       = 11;
static const uint8_t  MESH_CR       = 5;        // 4/5
static const uint8_t  MESH_SYNC     = 0x2B;     // all Meshtastic networks
static const uint16_t MESH_PREAMBLE = 16;
static const int8_t   MESH_TXPOWER  = 20;       // dBm PA_BOOST — stock fw ran this radio at RadioHead "23"

// Channel: name + PSK must match the other nodes. These are the Meshtastic
// defaults ("LongFast" + the well-known default key), i.e. a factory T-Deck.
static const char*   MESH_CHANNEL_NAME = "LongFast";
static const uint8_t MESH_KEY[16] = {
  0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
  0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};

// How this WiPhone announces itself on the mesh (T-Deck node list, etc).
// Leave "" to auto-generate from the node id: "WiPhone a1b2" / "a1b2".
static const char* MESH_LONG_NAME  = "";
static const char* MESH_SHORT_NAME = "";        // max 4 chars if you set it

#define MESH_RELAY 1          // 1 = rebroadcast other nodes' packets (extend the mesh)

// ============================================================================
//  Meshtastic wire constants (don't change these)
// ============================================================================

static const uint32_t MESH_BROADCAST = 0xFFFFFFFF;
static const uint8_t  MESH_HOP_LIMIT = 3;

// PacketHeader.flags bits (RadioInterface.h)
#define FLAG_HOP_MASK   0x07
#define FLAG_WANT_ACK   0x08
#define FLAG_VIA_MQTT   0x10
#define FLAG_HOP_START(h) ((uint8_t)((h) << 5))

#define PORT_TEXT       1     // PortNum.TEXT_MESSAGE_APP
#define PORT_NODEINFO   4     // PortNum.NODEINFO_APP
#define PORT_ROUTING    5     // PortNum.ROUTING_APP

#define MESH_HEADER_LEN 16
#define MESH_PKT_MAX    255   // SX127x FIFO
#define MESH_MAX_TEXT   200   // Meshtastic app-level text cap

// Wire header — must match Meshtastic PacketHeader exactly (packed, little-endian).
typedef struct __attribute__((packed)) {
  uint32_t to;
  uint32_t from;
  uint32_t id;
  uint8_t  flags;
  uint8_t  channel;      // channel hash hint
  uint8_t  next_hop;     // 0 = flood
  uint8_t  relay_node;   // low byte of the last relayer
} MeshHeader;

static uint8_t g_channelHash = 0x08;   // recomputed in setup() from name+PSK

// ============================================================================
//  Small state tables (file-static; there is exactly one Lora instance)
// ============================================================================

// Dedup: flooded packets arrive multiple times (direct + via relays). Remember
// the last 32 (from,id) pairs so duplicates are dropped before the UI sees them.
static uint64_t g_seen[32] = {0};
static uint8_t  g_seenIdx = 0;

static bool seenBefore(uint32_t from, uint32_t id) {
  uint64_t key = ((uint64_t)from << 32) | id;
  for (size_t i = 0; i < 32; i++) if (g_seen[i] == key) return true;
  g_seen[g_seenIdx] = key;
  g_seenIdx = (g_seenIdx + 1) & 31;
  return false;
}

// Node DB: names learned from NodeInfo broadcasts, so logs (and future UI work)
// can say "Nick-TDeck" instead of a raw hex id.
typedef struct { uint32_t num; char shortName[8]; char longName[24]; } MeshNodeEntry;
static MeshNodeEntry g_nodes[16];
static uint8_t g_nodeCount = 0, g_nodeNext = 0;

static const char* nodeName(uint32_t num) {
  static char fallback[12];
  for (uint8_t i = 0; i < g_nodeCount; i++)
    if (g_nodes[i].num == num) return g_nodes[i].longName[0] ? g_nodes[i].longName : g_nodes[i].shortName;
  snprintf(fallback, sizeof(fallback), "!%08x", num);
  return fallback;
}

// returns true if this node was previously unknown
static bool nodeDbUpdate(uint32_t num, const char* shortName, const char* longName) {
  for (uint8_t i = 0; i < g_nodeCount; i++) {
    if (g_nodes[i].num == num) {
      if (shortName[0]) strlcpy(g_nodes[i].shortName, shortName, sizeof(g_nodes[i].shortName));
      if (longName[0])  strlcpy(g_nodes[i].longName,  longName,  sizeof(g_nodes[i].longName));
      return false;
    }
  }
  MeshNodeEntry* e = &g_nodes[g_nodeNext];
  g_nodeNext = (g_nodeNext + 1) & 15;
  if (g_nodeCount < 16) g_nodeCount++;
  e->num = num;
  strlcpy(e->shortName, shortName, sizeof(e->shortName));
  strlcpy(e->longName,  longName,  sizeof(e->longName));
  return true;
}

// Delayed-TX queue: ACKs, NodeInfo replies and relays go out after a short
// randomized delay (lets the far radio settle back into RX; staggers floods).
typedef struct { uint16_t len; uint32_t at; uint8_t buf[MESH_PKT_MAX]; } QueuedTx;
static QueuedTx g_txq[4];

// Reliable DM in flight: retransmit until ACKed / relayed / out of tries.
static struct {
  bool     active = false;
  uint32_t id = 0, dest = 0, nextAt = 0;
  int8_t   triesLeft = 0;
  uint16_t len = 0;
  uint8_t  buf[MESH_PKT_MAX];
} g_dm;

static uint32_t g_nextNodeInfoAt = 0;   // periodic self-announce
static uint32_t g_lastIntroduceAt = 0;  // rate-limit new-node introductions
static uint32_t g_lastRadioMs = 0;      // RX watchdog

static inline bool timeReached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

// ============================================================================
//  Radio bring-up
// ============================================================================

volatile bool Lora::packetFlag = false;
static void IRAM_ATTR onDio0() { Lora::packetFlag = true; }

Lora::Lora() {}

void Lora::setup() {
#ifdef LORA_MESSAGING
  myNodeNum = chipId ? chipId : (uint32_t)ESP.getEfuseMac();
  if (myNodeNum == 0 || myNodeNum == MESH_BROADCAST) myNodeNum = 0x1CEB00DA;  // never valid on a real MAC

  g_channelHash = 0;
  for (const char* c = MESH_CHANNEL_NAME; *c; c++) g_channelHash ^= (uint8_t)*c;   // Channels.cpp xorHash
  for (size_t i = 0; i < sizeof(MESH_KEY); i++)    g_channelHash ^= MESH_KEY[i];

  // The LoRa daughterboard sits on GPIO14/12/13 — the ESP32's native HSPI pins,
  // unused by anything else (TFT+SD share VSPI). Real hardware SPI replaces the
  // stock bit-banged RHSoftwareSPI.
  static SPIClass loraSpi(HSPI);
  loraSpi.begin(HSPI_SCLK, HSPI_MISO, HSPI_MOSI, RFM95_CS);   // 14, 12, 13, 27

  // RST and DIO1 are not wired on the WiPhone daughterboard (RFM95_RST == -1);
  // TX/RX only needs DIO0 (GPIO38). RadioLib takes RADIOLIB_NC for both.
  static Module mod(RFM95_CS, RFM95_INT, RADIOLIB_NC, RADIOLIB_NC, loraSpi);
  radio = new SX1276(&mod);

  int st = radio->begin(MESH_FREQ, MESH_BW, MESH_SF, MESH_CR, MESH_SYNC, MESH_TXPOWER, MESH_PREAMBLE);
  if (st != RADIOLIB_ERR_NONE) {
    log_e("Meshtastic LoRa init FAILED: %d (daughterboard seated? right band?)", st);
    return;
  }
  radio->setCurrentLimit(140);           // PA_BOOST at +20 dBm needs ~120 mA (default OCP is 60)
  radio->setCRC(true);
  radio->setPacketReceivedAction(onDio0);   // RadioLib 6.x (older: setDio0Action(fn, RISING))
  radio->startReceive();

  ready = true;
  g_lastRadioMs = millis();
  g_nextNodeInfoAt = millis() + 20000 + (esp_random() % 10000);   // announce ourselves shortly after boot

  log_i("Meshtastic LoRa up: node !%08x ch 0x%02X %.3f MHz SF%u BW%.0f pwr %ddBm free=%d",
        myNodeNum, g_channelHash, MESH_FREQ, MESH_SF, MESH_BW, MESH_TXPOWER, ESP.getFreeHeap());
#endif
}

// ============================================================================
//  Main loop: drain RX, pump delayed/reliable TX, periodic NodeInfo, watchdog
// ============================================================================

bool Lora::loop() {
  if (!ready) return false;
  uint32_t now = millis();
  bool stored = false;

  if (packetFlag) {
    packetFlag = false;
    size_t len = radio->getPacketLength();
    if (len >= MESH_HEADER_LEN && len <= MESH_PKT_MAX) {
      uint8_t buf[MESH_PKT_MAX];
      // NB: readData returns a STATUS CODE (0 = ok), not the length — the
      // length comes from getPacketLength() above.
      int16_t st = radio->readData(buf, len);
      if (st == RADIOLIB_ERR_NONE) {
        g_lastRadioMs = now;
        stored = handleIncoming(buf, len, now);
      } else if (st != RADIOLIB_ERR_CRC_MISMATCH) {
        log_w("Mesh RX read err %d", st);
      }
    }
    radio->startReceive();
  }

  pumpQueues(now);

  if (timeReached(now, g_nextNodeInfoAt)) {
    queueNodeInfo(MESH_BROADCAST, 500 + (esp_random() % 1500));
    g_nextNodeInfoAt = now + 3UL * 3600UL * 1000UL;          // every 3 h, like stock firmware
  }

  // RX watchdog: SX127x occasionally wedges out of RX; a periodic re-arm is cheap.
  if (now - g_lastRadioMs > 5UL * 60UL * 1000UL) {
    radio->standby();
    radio->startReceive();
    g_lastRadioMs = now;
  }

  return stored;
}

void Lora::pumpQueues(uint32_t now) {
  // one delayed TX per pass keeps the phone's main loop responsive
  for (size_t i = 0; i < 4; i++) {
    if (g_txq[i].len && timeReached(now, g_txq[i].at)) {
      transmitNow(g_txq[i].buf, g_txq[i].len);
      g_txq[i].len = 0;
      break;
    }
  }

  if (g_dm.active && timeReached(now, g_dm.nextAt)) {
    if (g_dm.triesLeft > 0) {
      log_i("Mesh DM 0x%08x to %s: no ACK yet, retransmitting (%d left)", g_dm.id, nodeName(g_dm.dest), g_dm.triesLeft);
      transmitNow(g_dm.buf, g_dm.len);
      g_dm.triesLeft--;
      g_dm.nextAt = now + 4500 + (esp_random() % 1500);
    } else {
      log_e("Mesh DM 0x%08x to %s NOT delivered (no ACK after retries)", g_dm.id, nodeName(g_dm.dest));
      g_dm.active = false;
    }
  }
}

// ============================================================================
//  Receive path
// ============================================================================

bool Lora::handleIncoming(uint8_t* buf, size_t len, uint32_t now) {
  MeshHeader h;
  memcpy(&h, buf, MESH_HEADER_LEN);

  if (h.from == myNodeNum) {
    // our own packet echoed back by a relay — for a pending DM that's an
    // implicit ACK (a neighbor took over delivery; stop retransmitting)
    if (g_dm.active && h.id == g_dm.id) {
      log_i("Mesh DM 0x%08x picked up by a relay — handing off", g_dm.id);
      g_dm.active = false;
    }
    return false;
  }
  if (seenBefore(h.from, h.id)) {
    // duplicate — but if it's a want_ack DM to us, our ACK may have been the
    // thing that got lost: re-ACK (matches stock firmware behavior)
    if (h.to == myNodeNum && (h.flags & FLAG_WANT_ACK) && h.channel == g_channelHash)
      queueAck(h.from, h.id);
    return false;
  }

  uint8_t hopBits = h.flags & FLAG_HOP_MASK;
  bool wantAck    = h.flags & FLAG_WANT_ACK;
  bool forUs      = (h.to == myNodeNum);
  bool isBcast    = (h.to == MESH_BROADCAST);
  bool stored     = false;

  if ((forUs || isBcast) && h.channel == g_channelHash && len > MESH_HEADER_LEN) {
    uint8_t plain[MESH_PKT_MAX];
    size_t  plen = len - MESH_HEADER_LEN;
    memcpy(plain, buf + MESH_HEADER_LEN, plen);
    cryptCTR(h.from, h.id, plain, plen);

    MeshData d;
    if (decodeData(plain, plen, &d)) {
      switch (d.portnum) {
        case PORT_TEXT:
          stored = deliverText(d.payload, d.payloadLen, h.from, h.to);
          if (forUs && wantAck) queueAck(h.from, h.id);
          break;

        case PORT_NODEINFO: {
          // learn who's out there: User { id=1, long_name=2, short_name=3, ... }
          char shortName[8] = {0}, longName[24] = {0};
          size_t i = 0;
          while (i + 1 < d.payloadLen) {
            uint8_t tag = d.payload[i++], field = tag >> 3, wt = tag & 7;
            if (wt != 2) break;                    // User's interesting fields are all strings
            uint8_t l = d.payload[i++];
            if (i + l > d.payloadLen) break;
            if (field == 2) { size_t n = l < 23 ? l : 23; memcpy(longName,  d.payload + i, n); }
            if (field == 3) { size_t n = l < 7  ? l : 7;  memcpy(shortName, d.payload + i, n); }
            i += l;
          }
          bool isNew = nodeDbUpdate(h.from, shortName, longName);
          if (isNew) log_i("Mesh node discovered: !%08x \"%s\" (%s)", h.from, longName, shortName);
          if (forUs && wantAck) queueAck(h.from, h.id);
          if (forUs && d.wantResponse) {
            queueNodeInfo(h.from, 500 + (esp_random() % 1000));
          } else if (isNew && isBcast && now - g_lastIntroduceAt > 10UL * 60UL * 1000UL) {
            g_lastIntroduceAt = now;               // introduce ourselves to newcomers
            queueNodeInfo(MESH_BROADCAST, 1000 + (esp_random() % 2000));
          }
          break;
        }

        case PORT_ROUTING:
          if (forUs) handleRouting(d, h.from);
          break;

        default:                                   // position, telemetry, ... — not our business
          if (forUs && wantAck) queueAck(h.from, h.id);
          break;
      }
    } else if (forUs && wantAck) {
      queueAck(h.from, h.id);                      // addressed to us on our channel; be polite
    }
  }

#if MESH_RELAY
  // Managed flood: rebroadcast others' packets (even foreign channels — relays
  // don't need to decrypt) so the WiPhone extends the mesh instead of leeching.
  if (!forUs && hopBits > 0) {
    uint8_t relay[MESH_PKT_MAX];
    memcpy(relay, buf, len);
    MeshHeader* rh = (MeshHeader*)relay;
    rh->flags      = (uint8_t)((h.flags & ~FLAG_HOP_MASK) | (hopBits - 1));
    rh->relay_node = (uint8_t)(myNodeNum & 0xFF);
    queueTx(relay, len, 330 + (esp_random() % 1000));
    log_d("Mesh relay !%08x->!%08x id 0x%08x (hop %u->%u)", h.from, h.to, h.id, hopBits, hopBits - 1);
  }
#endif

  return stored;
}

bool Lora::deliverText(const uint8_t* text, size_t len, uint32_t from, uint32_t to) {
  if (!len || len > MESH_MAX_TEXT) return false;
  char msg[MESH_MAX_TEXT + 1];
  memcpy(msg, text, len);
  msg[len] = 0;

  char from_str[24], to_str[24];
  snprintf(from_str, sizeof(from_str), "LORA:%X", from);   // stock address format — the
  snprintf(to_str,   sizeof(to_str),   "LORA:%X", to);     // Messages UI routes these as-is

  gui.flash.messages.saveMessage(msg, from_str, to_str, true, ntpClock.getUnixTime());
  log_i("Mesh RX text from %s (!%08x) rssi %.0f snr %.1f: %s",
        nodeName(from), from, radio->getRSSI(), radio->getSNR(), msg);
  return true;
}

void Lora::handleRouting(const MeshData& d, uint32_t from) {
  if (!g_dm.active || d.requestId != g_dm.id) return;

  // Routing { oneof: error_reason = field 3 varint } ; NONE(0) = ACK
  uint32_t err = 0;
  if (d.payloadLen >= 2 && d.payload[0] == 0x18) err = d.payload[1];
  if (err == 0) {
    log_i("Mesh DM 0x%08x DELIVERED — ACK from %s", g_dm.id, nodeName(from));
  } else {
    log_e("Mesh DM 0x%08x FAILED — NAK %u from %s (3=no channel, 5=no interface)", g_dm.id, err, nodeName(from));
  }
  g_dm.active = false;
}

// ============================================================================
//  Transmit path
// ============================================================================

// UI hands us the destination as bare hex ("A1B2C3D4" — GUI strips "LORA:"
// before queueing, see GUI.cpp extractAddress), but tolerate the prefixed form
// too. 0 / empty / FFFFFFFF = broadcast (stock semantics).
int Lora::send_message(const char* to, const char* message) {
  if (!ready) return -1;
  if (strlen(message) > MESH_MAX_TEXT) {
    log_e("Mesh TX too long: %u (max %u)", (unsigned)strlen(message), MESH_MAX_TEXT);
    return -1;
  }
  const char* hex = to ? to : "";
  if (strncasecmp(hex, "LORA:", 5) == 0) hex += 5;
  uint32_t v = strtoul(hex, NULL, 16);
  uint32_t dst = (v == 0) ? MESH_BROADCAST : v;
  return buildAndSend(dst, message) ? 0 : -1;
}

bool Lora::buildAndSend(uint32_t to, const char* text) {
  uint8_t data[MESH_PKT_MAX];
  size_t dlen = encodeTextData(text, data, sizeof(data));
  if (!dlen) return false;

  bool reliable = (to != MESH_BROADCAST);
  uint8_t flags = (MESH_HOP_LIMIT & FLAG_HOP_MASK) | FLAG_HOP_START(MESH_HOP_LIMIT)
                | (reliable ? FLAG_WANT_ACK : 0);
  uint32_t id = esp_random(); if (!id) id = 1;

  uint8_t pkt[MESH_PKT_MAX];
  size_t plen = buildPacket(pkt, to, id, flags, data, dlen);
  if (!plen || !transmitNow(pkt, plen)) return false;

  log_i("Mesh TX text -> %s id 0x%08x (%u B, ~%u ms airtime)",
        to == MESH_BROADCAST ? "broadcast" : nodeName(to), id, (unsigned)plen,
        (unsigned)(radio->getTimeOnAir(plen) / 1000));

  if (reliable) {                                   // arm retransmit-until-ACKed
    g_dm.active = true;
    g_dm.id = id; g_dm.dest = to;
    g_dm.triesLeft = 2;
    g_dm.nextAt = millis() + 4500 + (esp_random() % 1500);
    g_dm.len = (uint16_t)plen;
    memcpy(g_dm.buf, pkt, plen);
  }
  return true;
}

// header + payload-encrypt into pkt; returns total length
size_t Lora::buildPacket(uint8_t* pkt, uint32_t to, uint32_t id, uint8_t flags,
                         const uint8_t* data, size_t dataLen) {
  if (MESH_HEADER_LEN + dataLen > MESH_PKT_MAX) return 0;
  MeshHeader* h = (MeshHeader*)pkt;
  h->to = to; h->from = myNodeNum; h->id = id;
  h->flags = flags;
  h->channel = g_channelHash;
  h->next_hop = 0;
  h->relay_node = (uint8_t)(myNodeNum & 0xFF);
  memcpy(pkt + MESH_HEADER_LEN, data, dataLen);
  cryptCTR(h->from, h->id, pkt + MESH_HEADER_LEN, dataLen);
  return MESH_HEADER_LEN + dataLen;
}

bool Lora::transmitNow(uint8_t* pkt, size_t len) {
  int st = radio->transmit(pkt, len);
  packetFlag = false;                 // DIO0 doubles as TxDone — don't read it as an RX
  radio->startReceive();
  if (st != RADIOLIB_ERR_NONE) { log_e("Mesh TX err %d", st); return false; }
  return true;
}

void Lora::queueTx(const uint8_t* pkt, size_t len, uint32_t delayMs) {
  for (size_t i = 0; i < 4; i++) {
    if (!g_txq[i].len) {
      memcpy(g_txq[i].buf, pkt, len);
      g_txq[i].len = (uint16_t)len;
      g_txq[i].at = millis() + delayMs;
      return;
    }
  }
  log_d("Mesh TX queue full, dropping");           // busy mesh moment; flood redundancy covers it
}

void Lora::queueAck(uint32_t to, uint32_t forId) {
  uint8_t data[32], pkt[64];
  size_t dlen = encodeAckData(forId, data, sizeof(data));
  uint32_t id = esp_random(); if (!id) id = 1;
  size_t plen = buildPacket(pkt, to, id, (MESH_HOP_LIMIT & FLAG_HOP_MASK) | FLAG_HOP_START(MESH_HOP_LIMIT), data, dlen);
  if (plen) queueTx(pkt, plen, 200 + (esp_random() % 300));
}

void Lora::queueNodeInfo(uint32_t to, uint32_t delayMs) {
  uint8_t data[96], pkt[128];
  size_t dlen = encodeNodeInfoData(data, sizeof(data));
  uint32_t id = esp_random(); if (!id) id = 1;
  size_t plen = buildPacket(pkt, to, id, (MESH_HOP_LIMIT & FLAG_HOP_MASK) | FLAG_HOP_START(MESH_HOP_LIMIT), data, dlen);
  if (plen) queueTx(pkt, plen, delayMs);
}

// ============================================================================
//  Hand-rolled protobufs. Encoding the 3 tiny messages this transport uses
//  avoids nanopb + the full meshtastic proto set (real flash/RAM savings).
// ============================================================================

static size_t pbPutVarint(uint8_t* out, uint32_t v) {
  size_t n = 0;
  do { out[n++] = (v & 0x7F) | (v > 0x7F ? 0x80 : 0); v >>= 7; } while (v);
  return n;
}

static size_t pbPutString(uint8_t* out, uint8_t field, const char* s) {
  size_t l = strlen(s), n = 0;
  out[n++] = (uint8_t)(field << 3) | 2;
  n += pbPutVarint(out + n, (uint32_t)l);
  memcpy(out + n, s, l);
  return n + l;
}

// Data { portnum=1, payload=2, bitfield=9 } carrying UTF-8 text
size_t Lora::encodeTextData(const char* text, uint8_t* out, size_t outCap) {
  size_t tlen = strlen(text);
  if (tlen + 8 > outCap) return 0;
  size_t i = 0;
  out[i++] = 0x08; out[i++] = PORT_TEXT;
  out[i++] = 0x12; i += pbPutVarint(out + i, (uint32_t)tlen);
  memcpy(out + i, text, tlen); i += tlen;
  out[i++] = 0x48; out[i++] = 0x01;      // bitfield: OK_TO_MQTT (modern firmware sets this)
  return i;
}

// Data { portnum=4, payload=User{ id=1, long_name=2, short_name=3, hw_model=5 } }
size_t Lora::encodeNodeInfoData(uint8_t* out, size_t outCap) {
  char idStr[12], longName[24], shortName[8];
  snprintf(idStr, sizeof(idStr), "!%08x", myNodeNum);
  if (MESH_LONG_NAME[0])  strlcpy(longName, MESH_LONG_NAME, sizeof(longName));
  else snprintf(longName, sizeof(longName), "WiPhone %04x", myNodeNum & 0xFFFF);
  if (MESH_SHORT_NAME[0]) strlcpy(shortName, MESH_SHORT_NAME, sizeof(shortName));
  else snprintf(shortName, sizeof(shortName), "%04x", myNodeNum & 0xFFFF);

  uint8_t user[80];
  size_t u = 0;
  u += pbPutString(user + u, 1, idStr);
  u += pbPutString(user + u, 2, longName);
  u += pbPutString(user + u, 3, shortName);
  user[u++] = 0x28; user[u++] = 0xFF; user[u++] = 0x01;    // hw_model = 255 (PRIVATE_HW)

  if (u + 6 > outCap) return 0;
  size_t i = 0;
  out[i++] = 0x08; out[i++] = PORT_NODEINFO;
  out[i++] = 0x12; i += pbPutVarint(out + i, (uint32_t)u);
  memcpy(out + i, user, u);
  return i + u;
}

// Data { portnum=5, payload=Routing{ error_reason=NONE }, request_id=<acked id> }
size_t Lora::encodeAckData(uint32_t forId, uint8_t* out, size_t outCap) {
  if (outCap < 11) return 0;
  size_t i = 0;
  out[i++] = 0x08; out[i++] = PORT_ROUTING;
  out[i++] = 0x12; out[i++] = 0x02; out[i++] = 0x18; out[i++] = 0x00;
  out[i++] = 0x35;                                          // field 6 (request_id), fixed32
  memcpy(out + i, &forId, 4);                               // little-endian
  return i + 4;
}

// Generic Data walker: tolerates every field current firmware sends (emoji
// tapbacks, reply_id, bitfield, ...) by skipping unknown fields per wire type.
bool Lora::decodeData(const uint8_t* in, size_t len, MeshData* out) {
  memset(out, 0, sizeof(*out));
  out->portnum = -1;
  size_t i = 0;
  while (i < len) {
    uint8_t tag = in[i++], field = tag >> 3, wt = tag & 7;
    if (wt == 0) {                                          // varint
      uint32_t v = 0; int shift = 0;
      while (i < len) {
        uint8_t b = in[i++];
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        if ((shift += 7) > 28) return false;
      }
      if (field == 1) out->portnum = (int)v;
      if (field == 3) out->wantResponse = (v != 0);
    } else if (wt == 2) {                                   // length-delimited
      uint32_t l = 0; int shift = 0;
      while (i < len) {
        uint8_t b = in[i++];
        l |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        if ((shift += 7) > 28) return false;
      }
      if (i + l > len) return false;
      if (field == 2) { out->payload = in + i; out->payloadLen = l; }
      i += l;
    } else if (wt == 5) {                                   // fixed32
      if (i + 4 > len) return false;
      if (field == 6) memcpy(&out->requestId, in + i, 4);   // request_id (routing ACKs)
      i += 4;
    } else if (wt == 1) {                                   // fixed64
      if (i + 8 > len) return false;
      i += 8;
    } else {
      return false;
    }
  }
  return out->portnum > 0;
}

// ============================================================================
//  AES-128-CTR with the Meshtastic nonce. CTR is symmetric: encrypt == decrypt.
//  Nonce (16B) = packetId(8B LE) | fromNode(4B LE) | blockCounter(4B, starts 0).
//  mbedtls handles the multi-block counter increment; ESP32 AES is hardware.
// ============================================================================

void Lora::cryptCTR(uint32_t fromNode, uint32_t packetId, uint8_t* bytes, size_t len) {
  uint8_t nonce[16] = {0};
  uint64_t pid = (uint64_t)packetId;
  memcpy(nonce,     &pid,      8);
  memcpy(nonce + 8, &fromNode, 4);

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, MESH_KEY, 128);   // CTR always uses the ENC schedule
  uint8_t streamBlock[16] = {0};
  size_t  ncOff = 0;
  mbedtls_aes_crypt_ctr(&ctx, len, &ncOff, nonce, streamBlock, bytes, bytes);
  mbedtls_aes_free(&ctx);
}
