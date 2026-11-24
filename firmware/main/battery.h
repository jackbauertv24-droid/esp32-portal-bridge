#pragma once
#include <stdbool.h>

#include "cJSON.h"

// Probes the MAX17048 gauge on both candidate pin pairs once at boot.
void battery_init(void);
void battery_status_json(cJSON *root);
// Releases / re-acquires the I2C pins (15/16), which the camera also uses.
void battery_suspend(void);
void battery_resume(void);
