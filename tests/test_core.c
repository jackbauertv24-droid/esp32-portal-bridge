// Host tests for the pure-C helpers. The HTML snippets are generic shapes of
// login pages (they are NOT recordings of the real portal, which has not been
// seen yet); they check the parser's mechanics, not that it fits that portal.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html_form.h"
#include "textutil.h"
#include "urlutil.h"

static int failures = 0;
#define CHECK(label, cond)                 \
  do {                                     \
    if (!(cond)) {                         \
      printf("FAIL %s\n", label);          \
      failures++;                          \
    }                                      \
  } while (0)
#define CHECK_STR(label, got, want)                                                 \
  do {                                                                              \
    if (strcmp((got), (want))) {                                                    \
      printf("FAIL %s\n  got:  [%s]\n  want: [%s]\n", label, (got), (want));        \
      failures++;                                                                   \
    }                                                                               \
  } while (0)

static void test_phone(void) {
  char out[40];
  phone_local_part("+852 9123 4567", "852", out, sizeof out);
  CHECK_STR("local from +cc", out, "91234567");
  phone_local_part("0085291234567", "852", out, sizeof out);
  CHECK_STR("local from 00cc", out, "91234567");
  phone_local_part("85291234567", "852", out, sizeof out);
  CHECK_STR("local from cc", out, "91234567");
  phone_local_part("8521234", "852", out, sizeof out);
  CHECK_STR("short number keeps leading digits", out, "8521234");
  phone_format("852", "91234567", "plus_cc", out, sizeof out);
  CHECK_STR("fmt plus_cc", out, "+85291234567");
  phone_format("852", "91234567", "00cc", out, sizeof out);
  CHECK_STR("fmt 00cc", out, "0085291234567");
  phone_format("852", "91234567", "local", out, sizeof out);
  CHECK_STR("fmt local", out, "91234567");
}

static void test_phone_match(void) {
  CHECK("same with country code", phone_same("+85291234567", "91234567"));
  CHECK("same with spaces and 00", phone_same("00852 9123 4567", "+852-9123-4567"));
  CHECK("different", !phone_same("+85291234567", "+85291234568"));
  CHECK("too short never matches", !phone_same("123", "123"));
  CHECK("empty never matches", !phone_same("", ""));
  CHECK("in list", phone_in_list("+85291234567", "61234567, 91234567"));
  CHECK("in list semicolon", phone_in_list("91234567", "+85261234567;+85291234567"));
  CHECK("not in list", !phone_in_list("+85291234567", "61234567,51234567"));
  CHECK("empty list", !phone_in_list("+85291234567", ""));
}

static void test_otp(void) {
  char c[16];
  CHECK("otp found", otp_extract("Your code is 482913. Valid 5 min", 4, 8, c, sizeof c) && !strcmp(c, "482913"));
  CHECK("otp skips long numbers",
        otp_extract("Call 85291234567 then enter 7731", 4, 8, c, sizeof c) && !strcmp(c, "7731"));
  CHECK("otp none", !otp_extract("no digits 12 here", 4, 8, c, sizeof c));
  CHECK("otp chinese", otp_extract("\xE9\xA9\x97\xE8\xAD\x89\xE7\xA2\xBC\xEF\xBC\x9A""551177", 4, 8, c, sizeof c) &&
                           !strcmp(c, "551177"));
}

static void test_placeholders(void) {
  int n = 0;
  char *s = placeholder_replace("phone=%3Cphone%3E&x=<PHONE>&j={\"p\":\"\\u003cphone\\u003e\"}", "phone", "91234567", &n);
  CHECK_STR("placeholder variants", s, "phone=91234567&x=91234567&j={\"p\":\"91234567\"}");
  CHECK("placeholder count", n == 3);
  free(s);
  CHECK("placeholder present", placeholder_present("code=%3ccode%3e", "code"));
  CHECK("placeholder absent", !placeholder_present("code=1234", "code"));
}

static void test_url(void) {
  char o[512];
  url_resolve("https://portal.example.net/login/index.php?x=1", "verify.php", o, sizeof o);
  CHECK_STR("relative", o, "https://portal.example.net/login/verify.php");
  url_resolve("https://portal.example.net/login/index.php", "/auth?a=b", o, sizeof o);
  CHECK_STR("root-relative", o, "https://portal.example.net/auth?a=b");
  url_resolve("https://portal.example.net/a/b/c", "../d", o, sizeof o);
  CHECK_STR("dot-dot", o, "https://portal.example.net/a/d");
  url_resolve("http://portal.example.net/a/b", "//cdn.example.org/x.js", o, sizeof o);
  CHECK_STR("scheme-relative", o, "http://cdn.example.org/x.js");
  url_resolve("http://portal.example.net/a/b?q=1", "", o, sizeof o);
  CHECK_STR("empty action = same URL", o, "http://portal.example.net/a/b?q=1");
  url_resolve("http://portal.example.net/a/b", "?step=2", o, sizeof o);
  CHECK_STR("query only", o, "http://portal.example.net/a/b?step=2");
  url_resolve("http://x.example/", "HTTPS://Other.Example:8443/p", o, sizeof o);
  CHECK_STR("absolute", o, "https://other.example:8443/p");
  url_resolve("http://x.example/a/b/", "c/", o, sizeof o);
  CHECK_STR("trailing slash kept", o, "http://x.example/a/b/c/");
  url_resolve("http://x.example/a/b/c", "./", o, sizeof o);
  CHECK_STR("dot slash", o, "http://x.example/a/b/");
  url_parts_t u;
  CHECK("split", url_split("https://h.example:444/p?q", &u) == 0 && !strcmp(u.host, "h.example:444") &&
                     !strcmp(u.path, "/p?q"));
}

static void test_forms(void) {
  hf_form_t *f = calloc(HF_MAX_FORMS, sizeof(hf_form_t));
  const char *page1 =
      "<html><head><script>var a='<form>';</script></head><body>"
      "<!-- <form action=\"/old\"> -->"
      "<form id=f method=POST action=\"/portal/send_sms.php?lang=en&amp;v=2\">"
      "<input type=hidden name=csrf value=\"tok&quot;123\">"
      "<p>Mobile number</p><input name=\"msisdn\" maxlength=8>"
      "<input type=checkbox name=agree value=yes>"
      "<button type=submit name=go value=1>Get code</button>"
      "</form></body></html>";
  int n = hf_parse(page1, f, HF_MAX_FORMS);
  CHECK("page1 one form", n == 1);
  CHECK_STR("page1 action decoded", f[0].action, "/portal/send_sms.php?lang=en&v=2");
  CHECK_STR("page1 method", f[0].method, "post");
  CHECK("page1 inputs", f[0].n_inputs == 4);
  CHECK_STR("page1 hidden value", f[0].inputs[0].value, "tok\"123");
  CHECK("page1 phone field", hf_find_phone(&f[0]) == 1);
  CHECK("page1 no code field", hf_find_code(&f[0]) == -1);

  const char *page2 =
      "<form action=verify method=post><label for=c>\xE9\xA9\x97\xE8\xAD\x89\xE7\xA2\xBC</label>"
      "<input id=c name=f2 type=text><input type=hidden name=sid value=abc>"
      "<select name=lang><option value=zh>ZH<option value=en selected>EN</select>"
      "<textarea name=note>hi &amp; bye</textarea><input type=submit value=OK></form>";
  n = hf_parse(page2, f, HF_MAX_FORMS);
  CHECK("page2 one form", n == 1);
  CHECK("page2 code field via chinese label", hf_find_code(&f[0]) == 0);
  CHECK_STR("page2 select picks selected", hf_value_of(f, n, "lang"), "en");
  CHECK_STR("page2 textarea", hf_value_of(f, n, "note"), "hi & bye");

  const char *page3 =
      "<div><input type=tel id=ph placeholder='Phone number'><input autocomplete=one-time-code id=otp>"
      "<button onclick=send()>Send</button></div>";
  n = hf_parse(page3, f, HF_MAX_FORMS);
  CHECK("page3 pseudo form for script page", n == 1);
  CHECK("page3 phone", n == 1 && hf_find_phone(&f[0]) == 0);
  CHECK("page3 code", n == 1 && hf_find_code(&f[0]) == 1);

  const char *only_text = "<form action=x><input name=a><input type=submit></form>";
  n = hf_parse(only_text, f, HF_MAX_FORMS);
  CHECK("single text field is taken as phone", hf_find_phone(&f[0]) == 0);

  char r[256];
  CHECK("meta refresh", hf_find_redirect("<meta http-equiv=\"Refresh\" content=\"0; URL='http://p.example/login'\">", r,
                                         sizeof r) &&
                            !strcmp(r, "http://p.example/login"));
  CHECK("js redirect", hf_find_redirect("<script>window.location.href = \"https://p.example/x?y=1\";</script>", r,
                                        sizeof r) &&
                           !strcmp(r, "https://p.example/x?y=1"));
  CHECK("no redirect", !hf_find_redirect("<p>hello</p>", r, sizeof r));
  CHECK("garbage does not crash", hf_parse("<form <input <<<>> <select", f, HF_MAX_FORMS) >= 0);
  free(f);
}

int main(void) {
  test_phone();
  test_otp();
  test_phone_match();
  test_placeholders();
  test_url();
  test_forms();
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all core checks passed\n");
  return 0;
}
