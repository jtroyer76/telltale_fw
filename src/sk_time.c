#include "sk_time.h"

#include <stdio.h>
#include <string.h>

static bool digits(const char *s, int n, int *out)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return true;
}

// Days from 1970-01-01 to y-m-d (proleptic Gregorian; Howard Hinnant's days_from_civil).
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool sk_time_parse(const char *s, int64_t *ms_out)
{
    int y, mo, d, h, mi, sec;
    if (!s || strlen(s) < 20) return false;
    if (!digits(s, 4, &y) || s[4] != '-' || !digits(s + 5, 2, &mo) || s[7] != '-' ||
        !digits(s + 8, 2, &d) || s[10] != 'T' || !digits(s + 11, 2, &h) || s[13] != ':' ||
        !digits(s + 14, 2, &mi) || s[16] != ':' || !digits(s + 17, 2, &sec)) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return false;

    const char *p = s + 19;
    int ms = 0;
    if (*p == '.') {
        p++;
        int n = 0;
        while (*p >= '0' && *p <= '9') {
            if (n < 3) ms = ms * 10 + (*p - '0');
            n++;
            p++;
        }
        if (n == 0) return false;
        for (; n < 3; n++) ms *= 10;
    }
    if (strcmp(p, "Z") != 0 && strcmp(p, "+00:00") != 0) return false;

    *ms_out = ((days_from_civil(y, mo, d) * 24 + h) * 60 + mi) * 60000LL + sec * 1000LL + ms;
    return true;
}

// Inverse of days_from_civil (Howard Hinnant's civil_from_days).
static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

void sk_time_format_iso(int64_t ms, char *buf, int len)
{
    int64_t secs = floor_div(ms, 1000);
    int64_t days = floor_div(secs, 86400);
    int sod = (int)(secs - days * 86400);
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, m, d, sod / 3600, sod / 60 % 60, sod % 60);
}

void sk_time_format_clock(int64_t ms, int offset_min, bool hour12, char *buf, int len)
{
    int64_t mins = floor_div(ms, 60000) + offset_min;
    int mod = (int)(mins - floor_div(mins, 1440) * 1440);   // minute of the local day
    int h = mod / 60, mi = mod % 60;
    if (hour12) {
        snprintf(buf, len, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, mi, h < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, len, "%02d:%02d", h, mi);
    }
}

bool sk_time_offset_minutes(double hhmm, int *minutes)
{
    if (hhmm != (double)(long)hhmm) return false;
    long v = (long)hhmm;
    long a = v < 0 ? -v : v;
    if (a % 100 > 59 || a / 100 > 14) return false;
    *minutes = (int)((v < 0 ? -1 : 1) * (a / 100 * 60 + a % 100));
    return true;
}
