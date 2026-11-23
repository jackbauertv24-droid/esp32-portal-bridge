#pragma once
#include <Arduino.h>

// Appends `s` to `out` as a quoted JSON string.
inline void json_str(String &out, const char *s) {
  out += '"';
  for (const char *p = s; *p; p++) {
    unsigned char c = (unsigned char)*p;
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += (char)c;
        }
    }
  }
  out += '"';
}
inline void json_str(String &out, const String &s) { json_str(out, s.c_str()); }

// Escapes text for safe inclusion in HTML.
inline String html_escape(const String &s) {
  String o;
  o.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}
