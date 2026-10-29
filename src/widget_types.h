// widget_types.h — the widget registry's config side (docs/SPEC.md §5.4, §6.3): type names,
// which bindings each takes, and the descriptors served at GET /api/widgets. The drawing side
// is in widgets.c. Adding a type: an entry here and its create/update in widgets.c.
#pragma once

#include <stdbool.h>

typedef struct {
    const char *name;
    bool takes_value;        // has a bind.value
    bool value_required;
    bool takes_range;        // min / max (ring, bar)
    bool takes_orientation;  // orientation (bar)
    bool takes_states;       // states map (state)
    bool takes_notifications; // bind.notifications (alert)
    bool takes_tide;         // api, hours (tide)
    bool takes_anchor;       // bind.position/anchor/radius/heading, trail_minutes (anchor)
    const char *descriptor;  // JSON object: props, bindings, types, defaults
} widget_type_t;

extern const widget_type_t widget_types[];
extern const int widget_type_count;

// Index into widget_types, or -1.
int widget_type_find(const char *name);
