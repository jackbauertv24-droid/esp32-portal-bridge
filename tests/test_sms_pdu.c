// Host-side tests for the PDU decoder: gcc -I firmware/core tests/test_sms_pdu.c firmware/core/sms_pdu.c
//
// The vectors were not hand-assembled: apart from the first (the widely used
// developershome.com example) they were produced by the independent `node-pdu`
// library and parsed back by it, so they check the decoder against a second
// implementation rather than against my own reading of the spec.
#include <stdio.h>
#include <string.h>

#include "sms_pdu.h"

static int failures = 0;

#define CHECK_STR(label, got, want)                                              \
  do {                                                                           \
    if (strcmp((got), (want)) != 0) {                                            \
      printf("FAIL %s\n  got:  [%s]\n  want: [%s]\n", label, (got), (want));     \
      failures++;                                                                \
    }                                                                            \
  } while (0)
#define CHECK_INT(label, got, want)                                              \
  do {                                                                           \
    if ((got) != (want)) {                                                       \
      printf("FAIL %s: got %d want %d\n", label, (int)(got), (int)(want));       \
      failures++;                                                                \
    }                                                                            \
  } while (0)

static void decode(const char *label, const char *hex, sms_t *m) {
  int rc = sms_pdu_decode(hex, m);
  if (rc != 0) {
    printf("FAIL %s: decode returned %d (%s)\n", label, rc, sms_pdu_error(rc));
    failures++;
  }
}

int main(void) {
  sms_t m;

  decode("classic", "07911326040000F0040B911346610089F60000208062917314080CC8F71D14969741F977FD07", &m);
  CHECK_STR("classic sender", m.sender, "+31641600986");
  CHECK_STR("classic text", m.text, "How are you?");
  CHECK_INT("classic single", m.concat_total, 1);
  if (strncmp(m.timestamp, "2002-08-26 19:37:41", 19) != 0) {
    printf("FAIL classic timestamp [%s]\n", m.timestamp);
    failures++;
  }

  decode("gsm7 escapes",
         "00000B915892214365F700006221305071350A1DC337B90CA2E164B9D80CB441BDD79B1468C3C36F7CA04D6F4301", &m);
  CHECK_STR("gsm7 sender", m.sender, "+85291234567");
  CHECK_STR("gsm7 text", m.text, "Code 482913 {ok} [x] ~^");
  CHECK_INT("gsm7 enc", m.encoding, SMS_ENC_GSM7);

  decode("alnum sender", "000012D1D7B4310D7DCBE9613600006221305071350A12D9775D0E7A52A1A0F41C54ABC562B71B", &m);
  CHECK_STR("alnum sender", m.sender, "WiFiPortal");
  CHECK_STR("alnum text", m.text, "Your OTP is 551177");

  decode("ucs2 chinese+emoji",
         "00000B915892214365F700086221305071840A2060A876849A578B4978BC662F00200037003300390031003000320020D83DDE00",
         &m);
  CHECK_STR("ucs2 text", m.text, "\xE6\x82\xA8\xE7\x9A\x84\xE9\xA9\x97\xE8\xAD\x89\xE7\xA2\xBC\xE6\x98\xAF 739102 \xF0\x9F\x98\x80");
  CHECK_INT("ucs2 enc", m.encoding, SMS_ENC_UCS2);

  decode("ucs2 latin + euro",
         "00000B915892214365F700086221305071840A4E0059006F0075007200200063006F006400650020006900730020003400380032003900"
         "310033002E002000560061006C00690064002000350020006D0069006E0020007B006F006B007D002020AC",
         &m);
  CHECK_STR("ucs2 euro text", m.text, "Your code is 482913. Valid 5 min {ok} \xE2\x82\xAC");

  decode("multipart 1/2",
         "004005812143F500006221305071840A8D060804F81D0201C16030180C0683C16030180C0683C16030180C0683C16030180C0683C16030"
         "180C0683C16030180C0683C16030180C0683C16030180C0683C16030180C0683C16030180C0683C16030180C0683C16030180C0683C160"
         "30180C0683C16030180C0683C16030180C0683C16030180C0683C160301804",
         &m);
  CHECK_STR("multipart sender", m.sender, "12345");
  CHECK_INT("multipart total", m.concat_total, 2);
  CHECK_INT("multipart seq 1", m.concat_seq, 1);
  CHECK_INT("multipart ref", m.concat_ref, 0xF81D);
  char want1[200];
  memset(want1, 'A', 133);
  want1[133] = 0;
  CHECK_STR("multipart part1 text", m.text, want1);

  decode("multipart 2/2",
         "004005812143F500006221305071840A62060804F81D0202C16030180C0683C16030180C068341D0F84D2E836231996C3603098542A150"
         "28140A8542A15028140A8542A15028140A8542A15028140A8542A15028140A8542A15028140A8542A15028140A854221",
         &m);
  CHECK_INT("multipart seq 2", m.concat_seq, 2);
  char want2[200];
  memset(want2, 'A', 17);
  strcpy(want2 + 17, " code 112233 ");
  memset(want2 + 30, 'B', 60);
  want2[90] = 0;
  CHECK_STR("multipart part2 text", m.text, want2);

  CHECK_INT("bad hex rejected", sms_pdu_decode("ZZ", &m) < 0, 1);
  CHECK_INT("truncated rejected", sms_pdu_decode("00000B9158922143", &m) < 0, 1);

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all sms_pdu checks passed\n");
  return 0;
}
