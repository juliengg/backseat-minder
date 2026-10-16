#pragma once
#include "cellular_protocol.h"

// Decode SMS-DELIVER PDUs without interpreting body text as AT responses.
// Concatenated messages are displayed as separate parts; binary SMS is rejected.
namespace received_sms {
struct Message {
  char sender[cellular::PHONE_CAPACITY] = {};
  char text[cellular::MAX_RECEIVED_MESSAGE + 1] = {};
};

inline bool append(char *out, size_t capacity, size_t &used, uint32_t cp) {
  if (cp == 0 || (cp < 32 && cp != '\n' && cp != '\r' && cp != '\t') || cp == 127)
    cp = 0xfffd;
  const size_t n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
  if (used + n >= capacity) return false;
  if (n == 1) out[used++] = static_cast<char>(cp);
  else {
    out[used++] = static_cast<char>((n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) |
                                    (cp >> (6 * (n - 1))));
    for (size_t i = n - 1; i; --i)
      out[used++] = static_cast<char>(0x80 | ((cp >> (6 * (i - 1))) & 0x3f));
  }
  out[used] = '\0';
  return true;
}

inline uint32_t gsm(unsigned c, bool extension) {
  if (extension) {
    switch (c) {
      case 10: return 12; case 20: return '^'; case 40: return '{';
      case 41: return '}'; case 47: return '\\'; case 60: return '[';
      case 61: return '~'; case 62: return ']'; case 64: return '|';
      case 101: return 0x20ac; default: return 0xfffd;
    }
  }
  static const uint16_t low[32] = {
    '@',0xa3,'$',0xa5,0xe8,0xe9,0xf9,0xec,0xf2,0xc7,'\n',0xd8,0xf8,'\r',0xc5,0xe5,
    0x394,'_',0x3a6,0x393,0x39b,0x3a9,0x3a0,0x3a8,0x3a3,0x398,0x39e,0xfffd,0xc6,0xe6,0xdf,0xc9};
  if (c < 32) return low[c];
  switch (c) {
    case 36: return 0xa4; case 64: return 0xa1;
    case 91: return 0xc4; case 92: return 0xd6; case 93: return 0xd1;
    case 94: return 0xdc; case 95: return 0xa7; case 96: return 0xbf;
    case 123: return 0xe4; case 124: return 0xf6; case 125: return 0xf1;
    case 126: return 0xfc; case 127: return 0xe0; default: return c;
  }
}

inline bool decode(const char *hex, Message &message) {
  char raw[256]; size_t size = 0;
  if (!cellular::decode_hex(hex, raw, sizeof(raw), size) || size < 2) return false;
  const auto *p = reinterpret_cast<const unsigned char *>(raw);
  size_t at = 1 + p[0];
  if (at + 3 > size) return false;
  const unsigned flags = p[at++];
  if ((flags & 3) != 0) return false; // SMS-DELIVER only.
  const unsigned digits = p[at++], type = p[at++];
  const size_t addressBytes = (digits + 1) / 2;
  if ((type & 0x70) == 0x50 || at + addressBytes + 10 > size) return false;
  size_t used = 0;
  if ((type & 0x70) == 0x10) message.sender[used++] = '+';
  for (unsigned i = 0; i < digits; ++i) {
    const unsigned d = (p[at + i / 2] >> ((i % 2) * 4)) & 15;
    if (d > 9 || used + 1 >= sizeof(message.sender)) return false;
    message.sender[used++] = static_cast<char>('0' + d);
  }
  message.sender[used] = '\0';
  at += addressBytes;
  ++at; // PID
  const unsigned dcs = p[at++];
  at += 7; // Service centre timestamp (the viewer uses local device uptime).
  const unsigned length = p[at++];
  unsigned alphabet;
  if ((dcs & 0xc0) == 0) {
    if (dcs & 0x20) return false; // Compression unsupported.
    alphabet = (dcs >> 2) & 3;
  } else if ((dcs & 0xf0) == 0xe0) alphabet = 2;
  else if ((dcs & 0xf0) == 0xc0 || (dcs & 0xf0) == 0xd0) alphabet = 0;
  else if ((dcs & 0xf0) == 0xf0) alphabet = (dcs & 4) ? 1 : 0;
  else return false;
  const size_t bytes = alphabet == 0 ? (length * 7 + 7) / 8 : length;
  if (at + bytes != size || bytes > 140 || (alphabet != 0 && alphabet != 2)) return false;
  const auto *data = p + at;
  size_t header = 0;
  if (flags & 0x40) {
    if (!bytes) return false;
    header = data[0] + 1;
    if (header > bytes) return false;
  }
  used = 0;
  message.text[0] = '\0';
  if (alphabet == 0) {
    bool escaped = false;
    const size_t first = (header * 8 + 6) / 7;
    if (first > length) return false;
    for (size_t i = first; i < length; ++i) {
      const size_t bit = i * 7;
      unsigned c = data[bit / 8] >> (bit % 8);
      if (bit % 8 > 1 && bit / 8 + 1 < bytes) c |= data[bit / 8 + 1] << (8 - bit % 8);
      c &= 127;
      if (c == 27 && !escaped) { escaped = true; continue; }
      if (!append(message.text, sizeof(message.text), used, gsm(c, escaped))) return false;
      escaped = false;
    }
    if (escaped && !append(message.text, sizeof(message.text), used, 0xfffd)) return false;
  } else {
    if ((bytes - header) % 2) return false;
    for (size_t i = header; i < bytes; i += 2) {
      uint32_t cp = (data[i] << 8) | data[i + 1];
      if (cp >= 0xd800 && cp <= 0xdbff && i + 3 < bytes) {
        const unsigned low = (data[i + 2] << 8) | data[i + 3];
        if (low >= 0xdc00 && low <= 0xdfff) {
          cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
          i += 2;
        }
      }
      if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
      if (!append(message.text, sizeof(message.text), used, cp)) return false;
    }
  }
  return message.sender[0] != '\0';
}
} // namespace received_sms
