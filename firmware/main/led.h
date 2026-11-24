#pragma once
// Status LED: blue booting, yellow setup pending, red no internet,
// cyan logging in to a login page, green online.
void led_start(void);
// Shows a fixed colour for `ms` milliseconds (hardware test).
void led_test(int r, int g, int b, int ms);
