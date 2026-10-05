#include "touch_patch.h"

#define FRONT_TOUCH_REPORTS 6
#define REAR_TOUCH_REPORTS 4
#define SYNTHETIC_TOUCH_ID_BASE 0x70

unsigned int vitacompanion_patch_touch_data(unsigned int port,
    SceTouchData *data, unsigned int count,
    const vitacompanion_touch_point points[VITACOMPANION_TOUCH_SLOTS])
{
    unsigned int maximum_reports;
    unsigned int buffer_index;
    unsigned int injected = 0;
    int slot;

    if (port > VITACOMPANION_TOUCH_REAR || !data || !points)
        return 0;

    maximum_reports = port == VITACOMPANION_TOUCH_FRONT
        ? FRONT_TOUCH_REPORTS : REAR_TOUCH_REPORTS;

    for (buffer_index = 0; buffer_index < count; ++buffer_index)
    {
        SceTouchData *current = &data[buffer_index];

        for (slot = 0; slot < VITACOMPANION_TOUCH_SLOTS; ++slot)
        {
            SceTouchReport *report;

            if (!points[slot].active ||
                current->reportNum >= maximum_reports)
                continue;

            report = &current->report[current->reportNum++];
            report->id = (uint8_t)(SYNTHETIC_TOUCH_ID_BASE + slot);
            report->force = 0x80;
            report->x = (int16_t)points[slot].x;
            report->y = (int16_t)points[slot].y;
            report->reserved[0] = 0;
            report->reserved[1] = 0;
            report->reserved[2] = 0;
            report->reserved[3] = 0;
            report->reserved[4] = 0;
            report->reserved[5] = 0;
            report->reserved[6] = 0;
            report->reserved[7] = 0;
            report->info = 0;
            ++injected;
        }
    }

    return injected;
}
