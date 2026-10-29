#include "widget_types.h"

#include <string.h>

// Descriptors are what an editor builds its forms from; config_parse enforces the same rules.
#define BINDING_VALUE \
    "\"value\":{\"required\":true,\"props\":{" \
        "\"path\":{\"type\":\"string\",\"required\":true}," \
        "\"unit\":{\"type\":\"unit\",\"default\":\"from Signal K displayUnits, else SI\"}," \
        "\"decimals\":{\"type\":\"integer\",\"min\":0,\"max\":6,\"default\":\"from Signal K displayFormat, else 1\"}," \
        "\"stale_after_s\":{\"type\":\"integer\",\"min\":1,\"default\":60}," \
        "\"zones\":{\"type\":\"zones\",\"default\":\"from Signal K metadata\"}}}"

#define PROP_LABEL "\"label\":{\"type\":\"string\",\"default\":\"Signal K displayName, else last path element\"}"
#define PROP_RANGE     "\"min\":{\"type\":\"number\",\"default\":0,\"note\":\"SI, like the data\"},"     "\"max\":{\"type\":\"number\",\"required\":true,\"note\":\"SI, like the data (ratio 1 = 100 %)\"}"

#define PROP_AT    "\"at\":{\"type\":\"cell\",\"required\":true,\"props\":{\"col\":{\"type\":\"integer\",\"min\":0},\"row\":{\"type\":\"integer\",\"min\":0},\"w\":{\"type\":\"integer\",\"min\":1,\"default\":1},\"h\":{\"type\":\"integer\",\"min\":1,\"default\":1}}}"

const widget_type_t widget_types[] = {
    {
        .name = "value", .takes_value = true, .value_required = true,
        .descriptor = "{\"type\":\"value\",\"shows\":\"Number or text with label and unit; colour from zones\","
                      "\"props\":{" PROP_AT "," PROP_LABEL "},"
                      "\"bindings\":{" BINDING_VALUE "}}",
    },
    {
        .name = "ring", .takes_value = true, .value_required = true, .takes_range = true,
        .descriptor = "{\"type\":\"ring\",\"shows\":\"Arc gauge from min to max with the value in the middle; colour from zones\","
                      "\"props\":{" PROP_AT "," PROP_LABEL "," PROP_RANGE "},"
                      "\"bindings\":{" BINDING_VALUE "}}",
    },
    {
        .name = "bar", .takes_value = true, .value_required = true, .takes_range = true, .takes_orientation = true,
        .descriptor = "{\"type\":\"bar\",\"shows\":\"Bar from min to max with the value; colour from zones\","
                      "\"props\":{" PROP_AT "," PROP_LABEL "," PROP_RANGE ","
                      "\"orientation\":{\"type\":\"enum\",\"values\":[\"horizontal\",\"vertical\"],"
                      "\"default\":\"from the cell: vertical if taller than wide\"}},"
                      "\"bindings\":{" BINDING_VALUE "}}",
    },
    {
        .name = "state", .takes_value = true, .value_required = true, .takes_states = true,
        .descriptor = "{\"type\":\"state\",\"shows\":\"Text and colour for an enum, e.g. charger state; unmapped values shown as received\","
                      "\"props\":{" PROP_AT "," PROP_LABEL ","
                      "\"states\":{\"type\":\"array\",\"max\":8,\"items\":{"
                      "\"value\":{\"type\":\"string\",\"required\":true,\"note\":\"numbers as written, e.g. 0; booleans true/false\"},"
                      "\"text\":{\"type\":\"string\",\"default\":\"the value\"},"
                      "\"state\":{\"type\":\"zone_state\",\"default\":\"normal\"}}}},"
                      "\"bindings\":{" BINDING_VALUE "}}",
    },
    {
        .name = "alert", .takes_notifications = true,
        .descriptor = "{\"type\":\"alert\",\"shows\":\"The most severe active Signal K notification (alert or worse) under a prefix, "
                      "coloured by severity, with a count of others; says the state is unknown when Signal K is not connected\","
                      "\"props\":{" PROP_AT ",\"label\":{\"type\":\"string\",\"default\":\"Alerts\"}},"
                      "\"bindings\":{\"notifications\":{\"required\":false,\"props\":{"
                      "\"prefix\":{\"type\":\"string\",\"default\":\"all notifications\","
                      "\"note\":\"path under notifications., e.g. electrical.batteries\"}}}}}",
    },
    {
        .name = "tide", .takes_value = true, .value_required = true, .takes_tide = true,
        .descriptor = "{\"type\":\"tide\",\"shows\":\"Predicted tide curve for the next hours with every high and low labelled "
                      "(local time from the clock config), the lowest low in the window called out, and the height now from the binding\","
                      "\"props\":{" PROP_AT "," PROP_LABEL ","
                      "\"api\":{\"type\":\"string\",\"required\":true,\"note\":\"tides API base on the Signal K server, "
                      "e.g. /signalk/v2/api/tides/stations/vessel/default (signalk-tides); /timeline and /extremes are fetched every 30 min\"},"
                      "\"hours\":{\"type\":\"integer\",\"min\":6,\"max\":48,\"default\":18}},"
                      "\"bindings\":{" BINDING_VALUE "}}",
    },
    {
        .name = "anchor", .takes_value = true, .value_required = true, .takes_anchor = true, .takes_notifications = true,
        .descriptor = "{\"type\":\"anchor\",\"shows\":\"North-up plot around the anchor: alarm circle, anchor, the boat (pointing along "
                      "its heading) and its swing trail; distance now / circle radius on top; red, with the alarm message, while a "
                      "notification under the prefix is active. Centred on the boat while no anchor is set\","
                      "\"props\":{" PROP_AT "," PROP_LABEL ","
                      "\"trail_minutes\":{\"type\":\"integer\",\"min\":10,\"max\":720,\"default\":240},"
                      "\"history\":{\"type\":\"string\",\"note\":\"Signal K History API values endpoint, e.g. /signalk/v2/api/history/values: "
                      "the trail is filled from it when the widget is built, so it survives restarts; absent = trail from boot\"}},"
                      "\"bindings\":{" BINDING_VALUE ","
                      "\"position\":{\"required\":true,\"props\":{\"path\":{\"type\":\"string\",\"note\":\"e.g. navigation.position\"}}},"
                      "\"anchor\":{\"required\":true,\"props\":{\"path\":{\"type\":\"string\",\"note\":\"e.g. navigation.anchor.position\"}}},"
                      "\"radius\":{\"required\":true,\"props\":{\"path\":{\"type\":\"string\",\"note\":\"e.g. navigation.anchor.maxRadius (m)\"}}},"
                      "\"heading\":{\"required\":false,\"props\":{\"path\":{\"type\":\"string\",\"note\":\"e.g. navigation.headingTrue\"}}},"
                      "\"notifications\":{\"required\":false,\"props\":{\"prefix\":{\"type\":\"string\",\"note\":\"e.g. navigation.anchor\"}}}}}",
    },
    {
        .name = "label", .takes_value = false,
        .descriptor = "{\"type\":\"label\",\"shows\":\"Static text\","
                      "\"props\":{" PROP_AT ",\"label\":{\"type\":\"string\",\"required\":true}},"
                      "\"bindings\":{}}",
    },
};

const int widget_type_count = sizeof(widget_types) / sizeof(widget_types[0]);

int widget_type_find(const char *name)
{
    for (int i = 0; i < widget_type_count; i++) {
        if (strcmp(widget_types[i].name, name) == 0) return i;
    }
    return -1;
}
