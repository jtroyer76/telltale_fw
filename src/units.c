#include "units.h"

#include <math.h>
#include <string.h>

typedef struct {
    const char *unit;
    const char *si;       // Signal K base unit it converts from
    const char *symbol;
    double scale;
    double offset;        // display = si * scale + offset
} unit_t;

static const unit_t s_units[] = {
    { "",      "",      "",    1.0,                0.0     },
    { "ratio", "ratio", "",    1.0,                0.0     },
    { "%",     "ratio", "%",   100.0,              0.0     },
    { "V",     "V",     "V",   1.0,                0.0     },
    { "A",     "A",     "A",   1.0,                0.0     },
    { "W",     "W",     "W",   1.0,                0.0     },
    { "Ah",    "C",     "Ah",  1.0 / 3600.0,       0.0     },
    { "Wh",    "J",     "Wh",  1.0 / 3600.0,       0.0     },
    { "K",     "K",     "K",   1.0,                0.0     },
    { "degC",  "K",     "°C",  1.0,                -273.15 },
    { "degF",  "K",     "°F",  1.8,                -459.67 },
    { "deg",   "rad",   "°",   180.0 / M_PI,       0.0     },
    { "m/s",   "m/s",   "m/s", 1.0,                0.0     },
    { "kn",    "m/s",   "kn",  3600.0 / 1852.0,    0.0     },
    { "m",     "m",     "m",   1.0,                0.0     },
    { "ft",    "m",     "ft",  1.0 / 0.3048,       0.0     },
    { "nm",    "m",     "nm",  1.0 / 1852.0,       0.0     },
    { "s",     "s",     "s",   1.0,                0.0     },
    { "min",   "s",     "min", 1.0 / 60.0,         0.0     },
    { "h",     "s",     "h",   1.0 / 3600.0,       0.0     },
    { "Pa",    "Pa",    "Pa",  1.0,                0.0     },
    { "hPa",   "Pa",    "hPa", 0.01,               0.0     },
};

// Signal K displayUnits.targetUnit names (as Signal K 2.26.0 sends them) -> unit keys above.
// Names not listed fall back to the unit key of the same name, if any.
static const struct { const char *target, *unit; } s_sk_targets[] = {
    { "F", "degF" }, { "C", "degC" }, { "foot", "ft" }, { "percent", "%" }, { "hour", "h" },
    { "degree", "deg" },
};

static const unit_t *find(const char *unit)
{
    if (!unit) unit = "";
    for (size_t i = 0; i < sizeof(s_units) / sizeof(s_units[0]); i++) {
        if (strcmp(s_units[i].unit, unit) == 0) return &s_units[i];
    }
    return NULL;
}

bool units_known(const char *unit) { return find(unit) != NULL; }

bool units_convert(const char *unit, double si, double *out)
{
    const unit_t *u = find(unit);
    if (!u) {
        *out = si;
        return false;
    }
    *out = si * u->scale + u->offset;
    return true;
}

const char *units_symbol(const char *unit)
{
    const unit_t *u = find(unit);
    return u ? u->symbol : (unit ? unit : "");
}

const char *units_from_sk(const char *si_units, const char *target_unit)
{
    if (!si_units || !target_unit) return NULL;
    const char *key = target_unit;
    for (size_t i = 0; i < sizeof(s_sk_targets) / sizeof(s_sk_targets[0]); i++) {
        if (strcmp(s_sk_targets[i].target, target_unit) == 0) key = s_sk_targets[i].unit;
    }
    const unit_t *u = find(key);
    // Only when it converts from the unit the server says the data is in.
    return (u && u->unit[0] && strcmp(u->si, si_units) == 0) ? u->unit : NULL;
}

int units_decimals_from_format(const char *format)
{
    if (!format) return -1;
    const char *dot = strchr(format, '.');
    if (!dot) return (format[0] == '0') ? 0 : -1;
    int n = 0;
    for (const char *c = dot + 1; *c == '0'; c++) n++;
    return n;
}
