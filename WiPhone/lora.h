/*
  WiPhone LoRa transport — Meshtastic-compatible rewrite.

  DROP-IN REPLACEMENT for the original WiPhone lora.h.
  Public API (class name + method signatures) is UNCHANGED, so WiPhone.ino,
  GUI.cpp and the message store need ZERO edits — only the radio guts change:
  the stock proprietary RadioHead protocol is replaced with the real Meshtastic
  over-the-air format, so a WiPhone joins the same mesh as a T-Deck / any node.

  Radio driver:  RadioHead (RH_RF95, bit-banged SPI) -> RadioLib (SX1276, hardware HSPI)
  Encryption:    AES-128-CTR via ESP32 hardware (mbedtls) — no extra flash
  Protobufs:     hand-rolled (Data / User / Routing, the 3 the mesh needs) — no nanopb

  What the WiPhone can do on the mesh with this transport:
    - send/receive text, broadcast + direct, default LongFast channel
    - appears as a NAMED node on other devices (NodeInfo broadcast + response)
    - ACKs direct messages, so senders see the "delivered" checkmark
    - reliable DMs: want_ack + retransmit until ACKed or handed to a relay
    - relays other nodes' packets (managed flood) — a real mesh citizen
    - dedups flooded packets so relayed copies don't duplicate in the UI

  Wire format verified against meshtastic/firmware master (Jul 2026):
    - PacketHeader: to,from,id (u32 LE) + flags,channel,next_hop,relay_node (u8) = 16 B
    - flags: hop_limit[0..2] | want_ack[3] | via_mqtt[4] | hop_start[5..7]
    - Nonce: packetId(8B LE) | fromNode(4B LE) | blockCtr(4B=0)   [CryptoEngine]
    - Default PSK "AQ==" -> the 16B AES128 key in lora.cpp        [Channels.cpp]
    - US LongFast: 906.875 MHz, BW 250, SF 11, CR 4/5, sync 0x2B, preamble 16
*/

#ifndef LORA_H
#define LORA_H

#include "Hardware.h"
#include <RadioLib.h>

// Decoded Meshtastic Data protobuf (the fields this transport acts on).
typedef struct {
  int            portnum;       // 1=text, 4=nodeinfo, 5=routing
  const uint8_t* payload;
  size_t         payloadLen;
  bool           wantResponse;
  uint32_t       requestId;     // set on routing ACK/NAK packets
} MeshData;

class Lora {
public:
  Lora();

  void setup();                                            // called once from WiPhone.ino
  bool loop();                                             // polled each main-loop pass
  int  send_message(const char* to, const char* message);  // hex node id (or "LORA:<hex>"); 0/empty = broadcast

  // ISR sets this when DIO0 fires (packet received / TX done). IRAM-safe flag.
  static volatile bool packetFlag;

private:
  SX1276*  radio = nullptr;
  bool     ready = false;
  uint32_t myNodeNum = 0;                                  // = WiPhone chipId (MAC-derived)

  // -- receive path --
  bool handleIncoming(uint8_t* buf, size_t len, uint32_t now);
  void handleRouting(const MeshData& d, uint32_t from);
  bool deliverText(const uint8_t* text, size_t len, uint32_t from, uint32_t to);

  // -- transmit path --
  bool transmitNow(uint8_t* pkt, size_t len);              // blocking TX + back to RX
  bool buildAndSend(uint32_t to, const char* text);
  size_t buildPacket(uint8_t* pkt, uint32_t to, uint32_t id, uint8_t flags,
                     const uint8_t* data, size_t dataLen); // header + encrypt
  void queueTx(const uint8_t* pkt, size_t len, uint32_t delayMs);
  void queueAck(uint32_t to, uint32_t forId);
  void queueNodeInfo(uint32_t to, uint32_t delayMs);
  void pumpQueues(uint32_t now);

  // -- protobufs (hand-rolled) --
  size_t encodeTextData(const char* text, uint8_t* out, size_t outCap);
  size_t encodeNodeInfoData(uint8_t* out, size_t outCap);
  size_t encodeAckData(uint32_t forId, uint8_t* out, size_t outCap);
  bool   decodeData(const uint8_t* in, size_t len, MeshData* out);

  // -- crypto --
  void cryptCTR(uint32_t fromNode, uint32_t packetId, uint8_t* bytes, size_t len);
};

#endif // LORA_H
