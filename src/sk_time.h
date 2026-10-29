// sk_time.h — Signal K timestamps ("2026-10-28T03:28:56.339343Z") to milliseconds since 1970.
// Used to judge staleness by when the server got a value, not when the display did:
// a fresh subscription replays the last value, which may be minutes old.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// UTC only ("Z" or "+00:00"); any fraction length. False if it can't be read.
bool sk_time_parse(const char *s, int64_t *ms_out);

// "2026-10-28T19:00:00Z" (whole seconds), for requests that take a time.
void sk_time_format_iso(int64_t ms, char *buf, int len);

// Clock time of `ms` shifted by `offset_min`: "6:14 PM" when hour12, else "18:14".
void sk_time_format_clock(int64_t ms, int offset_min, bool hour12, char *buf, int len);

// Signal K environment.time.timezoneOffset, "(-)hhmm" as a number (-700, 530, -930), to minutes.
// False if it isn't a valid offset (minutes over 59, more than 14 h).
bool sk_time_offset_minutes(double hhmm, int *minutes);
