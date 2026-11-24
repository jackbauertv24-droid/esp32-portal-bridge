// Host tests for core/callparse.c. Notification lines follow SIMCom formats
// (VOICE CALL, +CLIP) plus the DTMF spellings seen on related modules.
#include <stdio.h>
#include <string.h>

#include "callparse.h"

static int failures = 0;
#define CHECK(label, cond)        \
  do {                            \
    if (!(cond)) {                \
      printf("FAIL %s\n", label); \
      failures++;                 \
    }                             \
  } while (0)

static kp_event_t feed_str(keypad_t *k, const char *s) {
  kp_event_t e = KP_NONE;
  for (; *s; s++) e = keypad_feed(k, *s);
  return e;
}

int main(void) {
  char a[32];
  CHECK("ring", call_urc_parse("RING", a, sizeof a) == CU_RING);
  CHECK("clip", call_urc_parse("+CLIP: \"+85291234567\",145,,,,0", a, sizeof a) == CU_CLIP && !strcmp(a, "+85291234567"));
  CHECK("clip hidden", call_urc_parse("+CLIP: \"\",128,,,,1", a, sizeof a) == CU_CLIP && !a[0]);
  CHECK("begin", call_urc_parse("VOICE CALL: BEGIN", a, sizeof a) == CU_BEGIN);
  CHECK("end", call_urc_parse("VOICE CALL: END: 000012", a, sizeof a) == CU_END);
  CHECK("no carrier", call_urc_parse("NO CARRIER", a, sizeof a) == CU_END);
  CHECK("missed", call_urc_parse("MISSED_CALL: 10:15AM 91234567", a, sizeof a) == CU_MISSED);
  CHECK("recording full", call_urc_parse("+CREC: file full", a, sizeof a) == CU_REC_FULL);
  CHECK("recording reply is not a notification", call_urc_parse("+CREC: 2", a, sizeof a) == CU_NONE);
  CHECK("rxdtmf", call_urc_parse("+RXDTMF: 5", a, sizeof a) == CU_DTMF && a[0] == '5');
  CHECK("dtmf no space", call_urc_parse("+DTMF:#", a, sizeof a) == CU_DTMF && a[0] == '#');
  CHECK("dtmf quoted", call_urc_parse("+RXDTMF: \"*\"", a, sizeof a) == CU_DTMF && a[0] == '*');
  CHECK("tonedet ascii", call_urc_parse("+QTONEDET: 53", a, sizeof a) == CU_DTMF && a[0] == '5');
  CHECK("tonedet star", call_urc_parse("+QTONEDET: 42", a, sizeof a) == CU_DTMF && a[0] == '*');
  CHECK("dtmf with duration", call_urc_parse("+RXDTMF: 7,100", a, sizeof a) == CU_DTMF && a[0] == '7');
  CHECK("not a key", call_urc_parse("+CSQ: 20,99", a, sizeof a) == CU_NONE);
  CHECK("odd dtmf flagged", call_urc_parse("+DTMF: X9", a, sizeof a) == CU_NONE && call_urc_maybe_dtmf("+DTMF: X9"));
  CHECK("normal line not flagged", !call_urc_maybe_dtmf("+CSQ: 20,99") && !call_urc_maybe_dtmf("+RXDTMF: 5"));

  keypad_t k;
  keypad_init(&k, "123456", "*1", "*0");
  CHECK("partial pin", feed_str(&k, "12345") == KP_NONE && !k.unlocked);
  CHECK("pin ok", keypad_feed(&k, '6') == KP_PIN_OK && k.unlocked);
  CHECK("unmute", feed_str(&k, "*1") == KP_UNMUTE);
  CHECK("mute", feed_str(&k, "*0") == KP_MUTE);
  CHECK("noise then unmute", feed_str(&k, "99#*1") == KP_UNMUTE);
  CHECK("long noise then mute", feed_str(&k, "12345678901234567890*0") == KP_MUTE);

  keypad_init(&k, "123456", "*1", "*0");
  CHECK("wrong pin", feed_str(&k, "654321") == KP_PIN_BAD && k.bad_tries == 1 && !k.unlocked);
  CHECK("star restarts entry", feed_str(&k, "12*123456") == KP_PIN_OK);
  keypad_init(&k, "123456", "*1", "*0");
  CHECK("mute keys before unlock do nothing", feed_str(&k, "*1*0") == KP_NONE && !k.unlocked);
  keypad_init(&k, "", "*1", "*0");
  CHECK("no pin set never unlocks", keypad_feed(&k, '1') == KP_PIN_BAD && !k.unlocked);

  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("call: all passed\n");
  return 0;
}
