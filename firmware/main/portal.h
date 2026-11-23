// Login-page engine: logs the board into the upstream Wi-Fi's login page using
// the SIM's number and the code that arrives by SMS. Two methods:
//   recipe - replays saved steps (/data/recipe.json), editable on the admin page;
//   form   - finds the phone and code fields in the page by itself.
// Every attempt is recorded (recorder.h).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

void portal_start(void);
// Asks for an attempt now, regardless of mode and back-off.
void portal_trigger(void);
void portal_status_json(cJSON *root);
void portal_summary(char *out, size_t cap);

// Phone number as it should be typed into a form (per the phone settings),
// and the bare local number. Returns false if no number is known.
bool portal_phone(char *formatted, size_t cap, char *local, size_t lcap);

// Recipe file helpers.
char *recipe_load(void);                 // malloc'd JSON text or NULL
bool recipe_save(const char *json, char *err, size_t errcap);
bool recipe_delete(void);
// Builds a recipe from a recording (mirror or auto). Returns malloc'd JSON or NULL.
char *recipe_from_recording(const char *rec_name, char *err, size_t errcap);
