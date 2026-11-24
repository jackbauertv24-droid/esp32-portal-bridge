// Decoder for SMS-DELIVER PDUs as printed by the modem in PDU mode (AT+CMGF=0).
//
// Plain C with no Arduino dependency so it can be unit-tested on a PC
// (see tests/test_sms_pdu.c). PDU mode is used instead of text mode because
// text mode loses information: non-Latin text, multipart (concatenated)
// messages and alphanumeric senders are only reliably recoverable from the PDU.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMS_SENDER_MAX 48
#define SMS_TEXT_MAX 1024  // UTF-8 bytes; one part is at most 160 GSM chars or 70 UCS2 chars

typedef enum {
  SMS_ENC_GSM7 = 0,
  SMS_ENC_8BIT = 1,
  SMS_ENC_UCS2 = 2,
} sms_encoding_t;

typedef struct {
  char sender[SMS_SENDER_MAX];  // "+85291234567", "12345" or an alphanumeric name
  char timestamp[48];           // "2026-11-23 13:45:07 +08:00" (service-centre time)
  char text[SMS_TEXT_MAX];      // UTF-8, NUL terminated; 8-bit data is hex encoded
  sms_encoding_t encoding;
  int concat_ref;    // reference shared by all parts of a multipart message, -1 if single
  int concat_total;  // number of parts, 1 if single
  int concat_seq;    // this part's number, starting at 1
} sms_t;

// Decodes one PDU given as a hex string (with the leading SMSC field, which is
// how +CMGL / +CMGR print it). Returns 0 on success or a negative error code.
int sms_pdu_decode(const char *hex, sms_t *out);

// Short human-readable reason for a negative return value.
const char *sms_pdu_error(int code);

// ---------- sending (SMS-SUBMIT) ----------

#define SMS_SUBMIT_MAX_PARTS 8

typedef struct {
  char hex[400];  // PDU with an empty SMSC field ("00"), ready for AT+CMGS
  int tpdu_len;   // octets after the SMSC field, the length AT+CMGS expects
} sms_submit_part_t;

// Encodes UTF-8 `text` to `dest` ("+85291234567" or a local number). Uses the
// GSM 7-bit alphabet when every character fits, otherwise UCS2; long texts are
// split into concatenated parts with reference `ref`. Returns the number of
// parts, or a negative error (-1 bad number, -2 text too long, -3 bad UTF-8).
int sms_pdu_encode_submit(const char *dest, const char *text, unsigned char ref, sms_submit_part_t *parts,
                          int max_parts);

#ifdef __cplusplus
}
#endif
