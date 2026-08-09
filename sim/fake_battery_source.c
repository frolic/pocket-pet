#include "../app/battery_source.h"

/* Preset states the debug rail cycles through to exercise the indicator. */
static const struct { int percent; bool charging; } presets[] = {
    {100, false}, {76, false}, {45, false}, {18, false}, {62, true},
};
static int preset_index;

int battery_source_percent(void)
{
    return presets[preset_index].percent;
}

bool battery_source_charging(void)
{
    return presets[preset_index].charging;
}

void fake_battery_source_cycle(void)
{
    preset_index = (preset_index + 1) % 5;
}
