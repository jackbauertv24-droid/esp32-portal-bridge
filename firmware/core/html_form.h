// Minimal HTML scanner for captive-portal pages: finds <form> elements and their
// fields, meta-refresh and script redirects, and guesses which field takes the
// phone number and which takes the SMS code. Tolerant of malformed HTML; never
// allocates. Plain C, unit-tested on the host.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HF_MAX_FORMS 6
#define HF_MAX_INPUTS 24

typedef struct {
  char tag[10];       // input / select / textarea / button
  char type[16];      // lower-case input type ("text" if absent)
  char name[64];
  char id[64];
  char value[256];    // entity-decoded; for select: selected (or first) option
  char placeholder[96];
  char hint[128];     // label text / aria-label / autocomplete, lower-cased
  int checked;
} hf_input_t;

typedef struct {
  char action[512];   // raw attribute, entity-decoded ("" = same URL)
  char method[8];     // "get" or "post"
  char enctype[48];
  int n_inputs;
  hf_input_t inputs[HF_MAX_INPUTS];
} hf_form_t;

// Parses up to `max` forms. Inputs outside any <form> are collected into an
// extra pseudo-form with action "" when no real form exists (pages that submit
// with JavaScript). Returns the number of forms written.
int hf_parse(const char *html, hf_form_t *forms, int max);

// Scores how likely an input is the phone-number field (0 = not at all).
int hf_phone_score(const hf_input_t *in);
// Scores how likely an input is the SMS-code field (0 = not at all).
int hf_code_score(const hf_input_t *in);

// Index of the best phone / code field in a form, or -1.
int hf_find_phone(const hf_form_t *f);
int hf_find_code(const hf_form_t *f);

// Value of the first input named `name` in any form, or NULL.
const char *hf_value_of(const hf_form_t *forms, int n, const char *name);

// Extracts a redirect target from <meta http-equiv="refresh" content="0;url=...">
// or from a script assignment such as location.href='...'. Returns 1 if found.
int hf_find_redirect(const char *html, char *out, size_t cap);

// Is the input something a person types into (not hidden/submit/checkbox...)?
int hf_is_text_entry(const hf_input_t *in);

#ifdef __cplusplus
}
#endif
