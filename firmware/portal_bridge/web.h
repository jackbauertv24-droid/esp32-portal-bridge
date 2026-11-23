#pragma once
#include <Arduino.h>

void web_begin();
void web_loop();
// Plain-text diagnostics with identifiers masked, safe to attach to a public issue.
String build_report();
