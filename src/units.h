// units.h — Signal K values are SI; config (or Signal K's displayUnits) names the display unit.
#pragma once

#include <stdbool.h>

// True if `unit` is in the unit table ("" counts: shown as received).
bool units_known(const char *unit);

// Converts an SI value to `unit`. Returns false for an unknown unit (value left unchanged).
bool units_convert(const char *unit, double si, double *out);

// Text shown after the number, e.g. "degC" -> "°C".
const char *units_symbol(const char *unit);

// Unit key for Signal K metadata `units` + `displayUnits.targetUnit`, e.g. ("K", "F") -> "degF".
// NULL if not in the table or it doesn't convert from `si_units`.
const char *units_from_sk(const char *si_units, const char *target_unit);

// Decimals from a Signal K displayFormat such as "0.00" -> 2, "0" -> 0; -1 if not understood.
int units_decimals_from_format(const char *format);
