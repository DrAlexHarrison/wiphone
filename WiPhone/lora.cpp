/*
  WiPhone LoRa transport — Meshtastic-compatible rewrite (see lora.h header note).

  This file replaces the stock WiPhone lora.cpp. It keeps the exact Lora class
  API the rest of the firmware calls (setup / loop / send_message) but the bytes
  on the air are now real Meshtastic packets, so the WiPhone interoperates with
  a T-Deck and any other Meshtastic node on the default US LongFast channel.

  Region/channel are compile-time constants below — change MESH_FREQ + the region
  band for EU868 etc. Everything is grounded in meshtastic/firmware master.
*/

#include "lora.h"
#include "GUI.h"
#include "mbedtls/aes.h"
#include <esp_system.h>       // esp_random()

// Symbols already provided elsewhere in the WiPhone firmware (unchanged):
extern uint32_t chipId;       // our stable 32-bit id -> used as Meshtastic NodeNum
extern GUI      gui;          // gui.flash.messages.saveMessage(...)
extern NtpClock ntpClock;     // ntpClock.getUnixTime()

// ----------------------------------------------------------------------------
//  Meshtastic constants (US 915 LongFast). VERIFY per region before shipping.
// ----------------------------------------------------------------------------
static const float   MESH_FREQ      = 906.875;   // US LongFast slot 20 (hash("LongFast")%104)
static const float   MESH_BW        = 250.0;     // kHz
static const uint8_t MESH_SF        = 11;        // spreading factor
static const uint8_t MESH_CR        = 5;         // coding rate 4/5
static const uint8_t MESH_SYNC      = 0x2B;      // Meshtastic sync word (all networks)
static const uint16_t MESH_PREAMBLE = 16;
static const int8_t  MESH_TXPOWER   = 20;        // dBm — RFM95W ceiling (~+20)

// Default primary-channel PSK "AQ==" expands to this 16-byte AES-128 key.
static const uint8_t MESH_KEY[16] = {
  0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
  0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01
};
// Header channel-hash byte = xorHash("LongFast") ^ xorHash(MESH_KEY) = 0x08.
// (computed at init below so it stays correct if you change the channel)
static const uint32_t MESH_BROADCAST = 0xFFFFFFFF;
static const uint8_t  MESH_HOP_LIMIT = 3;        // default 3 hops

#define MESH_PORT_TEXT   1                        // PortNum.TEXT_MESSAGE_APP
#define MESH_HEADER_LEN  16
#define MESH_MAX_PAYLOAD 233                       // 255 - header - a little slack

// Wire header — must match Meshtastic PacketHeader exactly (packed, little-endian).
typedef struct __attribute__((packed)) {
  uint32_t to;
  uint32_t from;
  uint32_t id;
  uint8_t  flags;        // bits[0..2]=hop_limit, bit3=want_ack, bit4=via_mqtt
  uint8_t  channel;      // channel hash hint
  uint8_t  next_hop;     // 0 = unknown
  uint8_t  relay_node;   // 0 = unknown
} MeshHeader;

static uint8_t g_channelHash = 0x08;   // recomputed in setup()

// xor of all bytes — Meshtastic Channels.cpp xorHash()
static uint8_t xorHash(const uint8_t* p, size_t len) {
  uint8_t c = 0;
  for (size_t i = 0; i < len; i++) c ^= p[i];
  return c;
}

volatile bool Lora::packetFlag = false;
static void IRAM_ATTR onDio0() { Lora::packetFlag = true; }

Lora::Lora() {}

// ----------------------------------------------------------------------------
//  setup(): bring up SX1276 on the WiPhone daughterboard SPI, Meshtastic params
// ----------------------------------------------------------------------------
void Lora::setup() {
#ifdef LORA_MESSAGING
  myNodeNum = chipId;

  // channel-hash byte for the "LongFast" primary channel + default PSK
  const char* chName = "LongFast";
  g_channelHash = xorHash((const uint8_t*)chName, strlen(chName)) ^ xorHash(MESH_KEY, sizeof(MESH_KEY));

  // The LoRa daughterboard is wired to GPIO12/13/14 — the ESP32 HSPI bus.
  // Use a real hardware SPI (faster/cleaner than the stock bit-banged RHSoftwareSPI).
  static SPIClass loraSpi(HSPI);
  loraSpi.begin(HSPI_SCLK, HSPI_MISO, HSPI_MOSI, RFM95_CS);   // 14, 12, 13, 27

  // SX127x Module(cs, dio0/irq, rst, dio1). On the WiPhone: RST and DIO1 are NOT
  // wired (RFM95_RST == -1, no DIO1 pin) -> RADIOLIB_NC. Basic TX/RX only needs DIO0.
  static Module mod(RFM95_CS, RFM95_INT, RADIOLIB_NC, RADIOLIB_NC, loraSpi);
  radio = new SX1276(&mod);

  int st = radio->begin(MESH_FREQ, MESH_BW, MESH_SF, MESH_CR, MESH_SYNC, MESH_TXPOWER, MESH_PREAMBLE);
  if (st != RADIOLIB_ERR_NONE) {
    log_e("Meshtastic LoRa init FAILED: %d (check daughterboard seated / freq band)", st);
    return;
  }
  radio->setCRC(true);                 // Meshtastic uses hardware CRC
  radio->setPacketReceivedAction(onDio0);   // RadioLib 6.x API (older: setDio0Action(onDio0, RISING))
  radio->startReceive();               // continuous RX; DIO0 ISR flags packets

  ready = true;
  log_i("Meshtastic LoRa up: node=0x%X ch=0x%02X %.3fMHz SF%u BW%.0f free=%d",
        myNodeNum, g_channelHash, MESH_FREQ, MESH_SF, MESH_BW, ESP.getFreeHeap());
#endif
}

// ----------------------------------------------------------------------------
//  loop(): drain any received packet; returns true if a message was stored
// ----------------------------------------------------------------------------
bool Lora::loop() {
  if (!ready || !packetFlag) return false;
  packetFlag = false;

  uint8_t buf[256];
  int len = radio->readData(buf, sizeof(buf));
  bool stored = false;
  if (len > 0) {
    stored = handleIncoming(buf, (size_t)len);
  }
  radio->startReceive();               // re-arm for the next packet
  return stored;
}

// Parse a Meshtastic packet: filter channel + addressee, decrypt, decode, store.
bool Lora::handleIncoming(uint8_t* buf, size_t len) {
  if (len < MESH_HEADER_LEN) return false;
  MeshHeader* h = (MeshHeader*)buf;

  // channel-hash hint: not ours -> skip (avoids decrypting foreign channels)
  if (h->channel != g_channelHash) return false;

  // deliver broadcasts and packets addressed to us; ignore others
  if (h->to != MESH_BROADCAST && h->to != myNodeNum) return false;
  if (h->from == myNodeNum) return false;                 // our own echo

  uint8_t* enc = buf + MESH_HEADER_LEN;
  size_t   encLen = len - MESH_HEADER_LEN;
  if (encLen == 0 || encLen > MESH_MAX_PAYLOAD) return false;

  cryptCTR(h->from, h->id, enc, encLen);                  // decrypt in place

  char text[MESH_MAX_PAYLOAD + 1] = {0};
  if (!decodeTextData(enc, encLen, text, sizeof(text))) return false;  // not a text msg

  char from_str[24] = {0}, to_str[24] = {0};
  snprintf(from_str, sizeof(from_str), "LORA:%X", h->from);
  snprintf(to_str,   sizeof(to_str),   "LORA:%X", h->to);   // broadcast shows as LORA:FFFFFFFF

  // Same store call the stock firmware used -> existing Messages UI just works.
  gui.flash.messages.saveMessage(text, from_str, to_str, true, ntpClock.getUnixTime());
  log_i("Mesh RX from 0x%X: %s", h->from, text);
  return true;
}

// ----------------------------------------------------------------------------
//  send_message(): called by the WiPhone UI with "LORA:<hex>" (or "LORA:0" bcast)
// ----------------------------------------------------------------------------
int Lora::send_message(const char* to, const char* message) {
  if (!ready) return -1;
  if (strlen(message) > MESH_MAX_PAYLOAD - 8) {            // -8 for protobuf overhead
    log_e("Mesh TX too long: %d", strlen(message));
    return -1;
  }
  // "LORA:1a2b3c4d" -> node num; "LORA:0" / empty -> broadcast
  uint32_t dst = MESH_BROADCAST;
  const char* p = strchr(to, ':');
  if (p && *(p + 1)) {
    uint32_t v = strtoul(p + 1, NULL, 16);
    if (v != 0) dst = v;
  }
  return buildAndSend(dst, message) ? 0 : -1;
}

bool Lora::buildAndSend(uint32_t to, const char* text) {
  uint8_t pkt[256];
  MeshHeader* h = (MeshHeader*)pkt;
  h->to        = to;
  h->from      = myNodeNum;
  h->id        = esp_random() | 1;                         // nonzero packet id
  h->flags     = MESH_HOP_LIMIT & 0x07;
  h->channel   = g_channelHash;
  h->next_hop  = 0;
  h->relay_node= 0;

  // build + encrypt the Data protobuf payload
  uint8_t* payload = pkt + MESH_HEADER_LEN;
  size_t plen = encodeTextData(text, payload, sizeof(pkt) - MESH_HEADER_LEN);
  if (plen == 0) return false;
  cryptCTR(h->from, h->id, payload, plen);

  int st = radio->transmit(pkt, MESH_HEADER_LEN + plen);
  radio->startReceive();                                   // back to RX after TX
  if (st != RADIOLIB_ERR_NONE) { log_e("Mesh TX err %d", st); return false; }
  log_d("Mesh TX to 0x%X id 0x%X (%d B)", to, h->id, (int)plen);
  return true;
}

// ----------------------------------------------------------------------------
//  Minimal Data protobuf  { portnum=1 (varint), payload=<bytes> }  — text only.
//  Encoding a 2-field message by hand avoids pulling in nanopb + the full
//  meshtastic .proto set (meaningful flash/RAM saving on the ESP32).
// ----------------------------------------------------------------------------
size_t Lora::encodeTextData(const char* text, uint8_t* out, size_t outCap) {
  size_t tlen = strlen(text);
  if (tlen + 4 > outCap || tlen > 0x7F) return 0;          // keep payload varint 1-byte
  size_t i = 0;
  out[i++] = 0x08;                    // field 1 (portnum), wire type 0 (varint)
  out[i++] = MESH_PORT_TEXT;          // = 1
  out[i++] = 0x12;                    // field 2 (payload), wire type 2 (len-delimited)
  out[i++] = (uint8_t)tlen;           // length
  memcpy(out + i, text, tlen);
  return i + tlen;
}

bool Lora::decodeTextData(const uint8_t* in, size_t len, char* out, size_t outCap) {
  size_t i = 0;
  int portnum = -1;
  const uint8_t* payload = nullptr;
  size_t payloadLen = 0;

  while (i < len) {
    uint8_t tag = in[i++];
    uint8_t field = tag >> 3, wt = tag & 0x07;
    if (wt == 0) {                                   // varint
      uint32_t v = 0; int shift = 0;
      while (i < len && shift < 32) {
        uint8_t b = in[i++];
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
      }
      if (field == 1) portnum = v;                   // portnum
    } else if (wt == 2) {                            // length-delimited
      if (i >= len) return false;
      uint32_t l = 0; int shift = 0;
      while (i < len && shift < 32) {                // (length is itself a varint)
        uint8_t b = in[i++];
        l |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
      }
      if (i + l > len) return false;
      if (field == 2) { payload = in + i; payloadLen = l; }
      i += l;
    } else {
      return false;                                  // unexpected wire type -> not a clean Data msg
    }
  }

  if (portnum != MESH_PORT_TEXT || !payload || payloadLen == 0) return false;
  if (payloadLen >= outCap) payloadLen = outCap - 1;
  memcpy(out, payload, payloadLen);
  out[payloadLen] = 0;
  return true;
}

// ----------------------------------------------------------------------------
//  AES-128-CTR with the Meshtastic nonce.  CTR is symmetric: encrypt == decrypt.
//  Nonce (16B) = packetId(8B LE) | fromNode(4B LE) | blockCounter(4B, starts 0).
//  ESP32 mbedtls provides hardware-accelerated AES — no extra library flash.
// ----------------------------------------------------------------------------
void Lora::cryptCTR(uint32_t fromNode, uint32_t packetId, uint8_t* bytes, size_t len) {
  uint8_t nonce[16] = {0};
  uint64_t pid = (uint64_t)packetId;                 // high 32 bits are 0
  memcpy(nonce,     &pid,      8);                    // packetId, little-endian
  memcpy(nonce + 8, &fromNode, 4);                    // fromNode, little-endian
  // nonce[12..15] = 0 (block counter)

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, MESH_KEY, 128);        // CTR always uses the ENC schedule
  uint8_t streamBlock[16] = {0};
  size_t  ncOff = 0;
  mbedtls_aes_crypt_ctr(&ctx, len, &ncOff, nonce, streamBlock, bytes, bytes);
  mbedtls_aes_free(&ctx);
}
