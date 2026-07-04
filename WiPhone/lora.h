/*
  WiPhone LoRa transport — Meshtastic-compatible rewrite.

  DROP-IN REPLACEMENT for the original WiPhone lora.h.
  Public API (class name + method signatures) is UNCHANGED, so WiPhone.ino,
  GUI.cpp and the message store need ZERO edits — only the radio guts change:
  the stock proprietary RadioHead protocol is replaced with the real Meshtastic
  over-the-air format, so a WiPhone joins the same mesh as a T-Deck / any node.

  Radio driver swapped: RadioHead (RH_RF95) -> RadioLib (SX1276).
  Encryption: AES-128-CTR via ESP32 hardware (mbedtls) — no extra flash.
  Protobuf: the 2-field text Data message is hand-rolled — nanopb NOT required.

  Verified against meshtastic/firmware master (Jul 2026):
    - PacketHeader: to,from,id (u32 LE) + flags,channel,next_hop,relay_node (u8) = 16 B
    - Nonce: packetId(8B LE) | fromNode(4B LE) | blockCtr(4B=0)  [CryptoEngine::initNonce]
    - Default PSK "AQ==" -> 16B AES128 key below                 [Channels.cpp]
    - US LongFast: 906.875 MHz, BW 250, SF 11, CR 4/5, sync 0x2B, preamble 16
*/

#ifndef LORA_H
#define LORA_H

#include "Hardware.h"
#include <RadioLib.h>
#include "tinySIP.h"          // for TextMessage (unchanged)

class Lora {
public:
  Lora();

  void setup();                                         // called once from WiPhone.ino
  bool loop();                                          // polled each main-loop pass
  int  send_message(const char* to, const char* message);   // "LORA:<hex>" + UTF-8 text

  // ISR sets this when DIO0 fires (packet received). IRAM-safe flag.
  static volatile bool packetFlag;

private:
  SX1276* radio = nullptr;
  bool    ready = false;

  // ---- Meshtastic packet plumbing ----
  uint32_t myNodeNum = 0;                               // = WiPhone chipId (extern)

  bool  buildAndSend(uint32_t to, const char* text);
  bool  handleIncoming(uint8_t* buf, size_t len);

  // hand-rolled Data{portnum,payload} protobuf (text messages only)
  size_t encodeTextData(const char* text, uint8_t* out, size_t outCap);
  bool   decodeTextData(const uint8_t* in, size_t len, char* out, size_t outCap);

  // AES-128-CTR, Meshtastic nonce; symmetric (same call enc/dec)
  void   cryptCTR(uint32_t fromNode, uint32_t packetId, uint8_t* bytes, size_t len);
};

#endif // LORA_H
