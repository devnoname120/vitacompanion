#pragma once

#include <stdbool.h>

void nosleep_start(void);
void nosleep_end(void);
void nosleep_set_enabled(bool enabled);
bool nosleep_is_enabled(void);
