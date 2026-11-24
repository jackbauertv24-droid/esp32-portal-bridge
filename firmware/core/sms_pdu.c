#include "sms_pdu.h"

#include <stdio.h>
#include <string.h>

#define E_HEX -1
#define E_SHORT -2
#define E_TYPE -3

const char *sms_pdu_error(int code) {
  switch (code) {
    case E_HEX: return "not valid hex";
    case E_SHORT: return "PDU truncated";
    case E_TYPE: return "not an SMS-DELIVER PDU";
    default: return code < 0 ? "unknown error" : "ok";
  }
}

// GSM 03.38 default alphabet -> Unicode code points.
static const uint16_t GSM7_BASIC[128] = {
    0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC, 0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8,
    0x000D, 0x00C5, 0x00E5, 0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8, 0x03A3, 0x0398,
    0x039E, 0x00A0, 0x00C6, 0x00E6, 0x00DF, 0x00C9, 0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026,
    0x0027, 0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033,
    0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x00A1,
    0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D,
    0x004E, 0x004F, 0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A,
    0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7, 0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073, 0x0074,
    0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0};

// Characters reached through the 0x1B escape.
static uint16_t gsm7_ext(uint8_t c) {
  switch (c) {
    case 0x0A: return 0x000C;
    case 0x14: return 0x005E;
    case 0x28: return 0x007B;
    case 0x29: return 0x007D;
    case 0x2F: return 0x005C;
    case 0x3C: return 0x005B;
    case 0x3D: return 0x007E;
    case 0x3E: return 0x005D;
    case 0x40: return 0x007C;
    case 0x65: return 0x20AC;
    default: return 0x0020;
  }
}

typedef struct {
  char *buf;
  size_t cap;
  size_t len;
} sbuf_t;

static void put_byte(sbuf_t *b, char c) {
  if (b->len + 1 < b->cap) b->buf[b->len++] = c;
  b->buf[b->len < b->cap ? b->len : b->cap - 1] = 0;
}

static void put_cp(sbuf_t *b, uint32_t cp) {
  if (cp < 0x80) {
    put_byte(b, (char)cp);
  } else if (cp < 0x800) {
    if (b->len + 2 >= b->cap) return;
    put_byte(b, (char)(0xC0 | (cp >> 6)));
    put_byte(b, (char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    if (b->len + 3 >= b->cap) return;
    put_byte(b, (char)(0xE0 | (cp >> 12)));
    put_byte(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    put_byte(b, (char)(0x80 | (cp & 0x3F)));
  } else {
    if (b->len + 4 >= b->cap) return;
    put_byte(b, (char)(0xF0 | (cp >> 18)));
    put_byte(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
    put_byte(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    put_byte(b, (char)(0x80 | (cp & 0x3F)));
  }
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Converts the hex string to bytes; returns the byte count or E_HEX.
static int unhex(const char *hex, uint8_t *out, size_t cap) {
  size_t n = 0;
  while (hex[0] && hex[0] != '\r' && hex[0] != '\n') {
    if (!hex[1]) return E_HEX;
    int hi = hexval(hex[0]), lo = hexval(hex[1]);
    if (hi < 0 || lo < 0) return E_HEX;
    if (n >= cap) return E_SHORT;
    out[n++] = (uint8_t)((hi << 4) | lo);
    hex += 2;
  }
  return (int)n;
}

// Reads septet number `idx` from GSM 7-bit packed data.
static uint8_t septet_at(const uint8_t *d, size_t idx) {
  size_t bit = idx * 7;
  size_t byte = bit / 8;
  unsigned shift = bit % 8;
  unsigned v = d[byte] >> shift;
  if (shift > 1) v |= (unsigned)d[byte + 1] << (8 - shift);
  return (uint8_t)(v & 0x7F);
}

// Decodes `count` septets starting at septet `first` into UTF-8.
static void gsm7_decode(const uint8_t *d, size_t first, size_t count, sbuf_t *b) {
  int esc = 0;
  for (size_t i = first; i < first + count; i++) {
    uint8_t c = septet_at(d, i);
    if (esc) {
      put_cp(b, gsm7_ext(c));
      esc = 0;
    } else if (c == 0x1B) {
      esc = 1;
    } else {
      put_cp(b, GSM7_BASIC[c]);
    }
  }
}

static void bcd_digits(const uint8_t *d, int ndigits, sbuf_t *b) {
  for (int i = 0; i < ndigits; i++) {
    uint8_t v = (i & 1) ? (d[i / 2] >> 4) : (d[i / 2] & 0x0F);
    if (v <= 9) put_byte(b, (char)('0' + v));
    else if (v == 0x0A) put_byte(b, '*');
    else if (v == 0x0B) put_byte(b, '#');
  }
}

static int swapped(uint8_t v) { return (v & 0x0F) * 10 + (v >> 4); }

int sms_pdu_decode(const char *hex, sms_t *out) {
  uint8_t p[200];
  memset(p, 0, sizeof(p));
  memset(out, 0, sizeof(*out));
  out->concat_ref = -1;
  out->concat_total = 1;
  out->concat_seq = 1;

  int n = unhex(hex, p, sizeof(p));
  if (n < 0) return n;
  size_t i = 0;
#define NEED(k) \
  if (i + (k) > (size_t)n) return E_SHORT

  NEED(1);
  i += 1 + p[0];  // skip SMSC info
  NEED(1);
  uint8_t first = p[i++];
  if ((first & 0x03) != 0x00) return E_TYPE;  // MTI 00 = SMS-DELIVER
  int udhi = (first & 0x40) != 0;

  // Originating address.
  NEED(2);
  int oa_digits = p[i++];
  uint8_t oa_type = p[i++];
  int oa_bytes = (oa_digits + 1) / 2;
  NEED((size_t)oa_bytes);
  sbuf_t sb = {out->sender, sizeof(out->sender), 0};
  out->sender[0] = 0;
  if ((oa_type & 0x70) == 0x50) {
    gsm7_decode(&p[i], 0, (size_t)(oa_digits * 4 / 7), &sb);
  } else {
    if ((oa_type & 0x70) == 0x10) put_byte(&sb, '+');
    bcd_digits(&p[i], oa_digits, &sb);
  }
  i += oa_bytes;

  NEED(2 + 7 + 1);
  i++;  // protocol identifier
  uint8_t dcs = p[i++];
  const uint8_t *ts = &p[i];
  i += 7;
  int tz_q = swapped(ts[6] & 0xF7);
  int tz_neg = (ts[6] & 0x08) != 0;
  snprintf(out->timestamp, sizeof(out->timestamp), "20%02d-%02d-%02d %02d:%02d:%02d %c%02d:%02d", swapped(ts[0]),
           swapped(ts[1]), swapped(ts[2]), swapped(ts[3]), swapped(ts[4]), swapped(ts[5]), tz_neg ? '-' : '+',
           tz_q / 4, (tz_q % 4) * 15);

  sms_encoding_t enc = SMS_ENC_GSM7;
  if ((dcs & 0xC0) == 0x00) {
    int a = (dcs >> 2) & 0x03;
    enc = a == 1 ? SMS_ENC_8BIT : a == 2 ? SMS_ENC_UCS2 : SMS_ENC_GSM7;
  } else if ((dcs & 0xF0) == 0xE0) {
    enc = SMS_ENC_UCS2;
  } else if ((dcs & 0xF0) == 0xF0) {
    enc = (dcs & 0x04) ? SMS_ENC_8BIT : SMS_ENC_GSM7;
  }
  out->encoding = enc;

  int udl = p[i++];
  const uint8_t *ud = &p[i];
  size_t ud_bytes = enc == SMS_ENC_GSM7 ? (size_t)((udl * 7 + 7) / 8) : (size_t)udl;
  NEED(ud_bytes);

  size_t hdr_bytes = 0;
  if (udhi && ud_bytes > 0) {
    size_t udhl = ud[0];
    if (udhl + 1 > ud_bytes) return E_SHORT;
    size_t j = 1;
    while (j + 1 < udhl + 1) {
      uint8_t iei = ud[j], iel = ud[j + 1];
      const uint8_t *ie = &ud[j + 2];
      if (j + 2 + iel > udhl + 1) break;
      if (iei == 0x00 && iel == 3) {
        out->concat_ref = ie[0];
        out->concat_total = ie[1];
        out->concat_seq = ie[2];
      } else if (iei == 0x08 && iel == 4) {
        out->concat_ref = (ie[0] << 8) | ie[1];
        out->concat_total = ie[2];
        out->concat_seq = ie[3];
      }
      j += 2 + iel;
    }
    hdr_bytes = udhl + 1;
  }

  sbuf_t tb = {out->text, sizeof(out->text), 0};
  out->text[0] = 0;
  if (enc == SMS_ENC_GSM7) {
    size_t skip = (hdr_bytes * 8 + 6) / 7;  // header plus fill bits, in septets
    if ((size_t)udl > skip) gsm7_decode(ud, skip, (size_t)udl - skip, &tb);
  } else if (enc == SMS_ENC_UCS2) {
    for (size_t k = hdr_bytes; k + 1 < (size_t)udl; k += 2) {
      uint32_t u = ((uint32_t)ud[k] << 8) | ud[k + 1];
      if (u >= 0xD800 && u <= 0xDBFF && k + 3 < (size_t)udl) {
        uint32_t lo = ((uint32_t)ud[k + 2] << 8) | ud[k + 3];
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
          k += 2;
        }
      }
      put_cp(&tb, u);
    }
  } else {
    static const char HX[] = "0123456789ABCDEF";
    for (size_t k = hdr_bytes; k < (size_t)udl; k++) {
      put_byte(&tb, HX[ud[k] >> 4]);
      put_byte(&tb, HX[ud[k] & 0x0F]);
    }
  }
  return 0;
#undef NEED
}

// ---------- SMS-SUBMIT encoder ----------

// Maps a code point to the GSM 7-bit alphabet: returns the septet, with 0x100
// set when it needs the 0x1B escape, or -1 if it is not representable.
static int gsm7_of(uint32_t cp) {
  for (int i = 0; i < 128; i++)
    if (GSM7_BASIC[i] == cp) return i;
  static const uint8_t ext[] = {0x0A, 0x14, 0x28, 0x29, 0x2F, 0x3C, 0x3D, 0x3E, 0x40, 0x65};
  for (size_t i = 0; i < sizeof(ext); i++)
    if (gsm7_ext(ext[i]) == cp) return 0x100 | ext[i];
  return -1;
}

// UTF-8 -> code points. Returns the count or -3 on malformed input.
static int utf8_decode(const char *s, uint32_t *out, int cap) {
  int n = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    uint32_t cp;
    int extra;
    if (*p < 0x80) cp = *p, extra = 0;
    else if ((*p & 0xE0) == 0xC0) cp = *p & 0x1F, extra = 1;
    else if ((*p & 0xF0) == 0xE0) cp = *p & 0x0F, extra = 2;
    else if ((*p & 0xF8) == 0xF0) cp = *p & 0x07, extra = 3;
    else return -3;
    p++;
    for (int k = 0; k < extra; k++, p++) {
      if ((*p & 0xC0) != 0x80) return -3;
      cp = (cp << 6) | (*p & 0x3F);
    }
    if (n >= cap) return -2;
    out[n++] = cp;
  }
  return n;
}

static void hex_put(char *out, size_t *o, const uint8_t *b, size_t n) {
  static const char HX[] = "0123456789ABCDEF";
  for (size_t i = 0; i < n; i++) {
    out[(*o)++] = HX[b[i] >> 4];
    out[(*o)++] = HX[b[i] & 15];
  }
  out[*o] = 0;
}

// Destination address field: length (digits), type, swapped BCD.
static int encode_address(const char *dest, uint8_t *out) {
  char d[24];
  int n = 0, intl = 0;
  for (const char *p = dest; *p; p++) {
    if (*p == '+' && n == 0) intl = 1;
    else if (*p >= '0' && *p <= '9') {
      if (n >= 20) return -1;
      d[n++] = *p;
    } else if (*p != ' ' && *p != '-' && *p != '(' && *p != ')') return -1;
  }
  if (n < 3) return -1;
  int k = 0;
  out[k++] = (uint8_t)n;
  out[k++] = intl ? 0x91 : 0x81;
  for (int i = 0; i < n; i += 2) {
    uint8_t lo = (uint8_t)(d[i] - '0');
    uint8_t hi = (i + 1 < n) ? (uint8_t)(d[i + 1] - '0') : 0x0F;
    out[k++] = (uint8_t)(hi << 4 | lo);
  }
  return k;
}

int sms_pdu_encode_submit(const char *dest, const char *text, unsigned char ref, sms_submit_part_t *parts,
                          int max_parts) {
  static uint32_t cps[1200];
  int ncp = utf8_decode(text, cps, 1200);
  if (ncp < 0) return ncp;
  uint8_t addr[16];
  int al = encode_address(dest, addr);
  if (al < 0) return -1;

  // GSM 7-bit if possible.
  static uint16_t sept[2400];
  int ns = 0, gsm = 1;
  for (int i = 0; i < ncp && gsm; i++) {
    int g = gsm7_of(cps[i]);
    if (g < 0) gsm = 0;
    else if (g & 0x100) {
      sept[ns++] = 0x1B;
      sept[ns++] = (uint16_t)(g & 0x7F);
    } else sept[ns++] = (uint16_t)g;
  }
  // UCS2 code units otherwise.
  static uint16_t units[2400];
  int nu = 0;
  if (!gsm)
    for (int i = 0; i < ncp; i++) {
      if (cps[i] >= 0x10000) {
        uint32_t v = cps[i] - 0x10000;
        units[nu++] = (uint16_t)(0xD800 | (v >> 10));
        units[nu++] = (uint16_t)(0xDC00 | (v & 0x3FF));
      } else units[nu++] = (uint16_t)cps[i];
    }

  int total_len = gsm ? ns : nu;
  int single = gsm ? 160 : 70, per = gsm ? 153 : 67;
  int nparts;
  int starts[SMS_SUBMIT_MAX_PARTS + 1];
  if (total_len <= single) {
    nparts = 1;
    starts[0] = 0;
    starts[1] = total_len;
  } else {
    nparts = 0;
    int pos = 0;
    while (pos < total_len) {
      if (nparts >= max_parts || nparts >= SMS_SUBMIT_MAX_PARTS) return -2;
      int end = pos + per < total_len ? pos + per : total_len;
      // Never split an escape pair or a surrogate pair.
      if (end < total_len) {
        if (gsm && sept[end - 1] == 0x1B) end--;
        if (!gsm && units[end - 1] >= 0xD800 && units[end - 1] <= 0xDBFF) end--;
      }
      starts[nparts++] = pos;
      pos = end;
    }
    starts[nparts] = total_len;
  }
  if (nparts > max_parts) return -2;

  for (int p = 0; p < nparts; p++) {
    uint8_t b[200];
    int k = 0;
    int udh = nparts > 1;
    b[k++] = (uint8_t)(0x11 | (udh ? 0x40 : 0));  // SMS-SUBMIT, relative validity period
    b[k++] = 0x00;                                 // message reference (set by the modem)
    memcpy(b + k, addr, (size_t)al);
    k += al;
    b[k++] = 0x00;               // protocol identifier
    b[k++] = gsm ? 0x00 : 0x08;  // data coding scheme
    b[k++] = 0xAA;               // validity: 4 days
    int a = starts[p], z = starts[p + 1];
    uint8_t hdr[6] = {0x05, 0x00, 0x03, ref, (uint8_t)nparts, (uint8_t)(p + 1)};
    if (gsm) {
      int hdr_septets = udh ? 7 : 0;  // 6 header octets + 1 fill bit = 7 septets
      int udl = hdr_septets + (z - a);
      b[k++] = (uint8_t)udl;
      int ud_bytes = (udl * 7 + 7) / 8;
      uint8_t *ud = b + k;
      memset(ud, 0, (size_t)ud_bytes);
      if (udh) memcpy(ud, hdr, 6);
      for (int i = a; i < z; i++) {
        int bit = (hdr_septets + (i - a)) * 7;
        int byte = bit / 8, sh = bit % 8;
        ud[byte] |= (uint8_t)(sept[i] << sh);
        if (sh > 1) ud[byte + 1] |= (uint8_t)(sept[i] >> (8 - sh));
      }
      k += ud_bytes;
    } else {
      int udl = (udh ? 6 : 0) + 2 * (z - a);
      b[k++] = (uint8_t)udl;
      if (udh) {
        memcpy(b + k, hdr, 6);
        k += 6;
      }
      for (int i = a; i < z; i++) {
        b[k++] = (uint8_t)(units[i] >> 8);
        b[k++] = (uint8_t)(units[i] & 0xFF);
      }
    }
    size_t o = 0;
    parts[p].hex[0] = 0;
    hex_put(parts[p].hex, &o, (const uint8_t *)"\x00", 1);
    hex_put(parts[p].hex, &o, b, (size_t)k);
    parts[p].tpdu_len = k;
  }
  return nparts;
}
