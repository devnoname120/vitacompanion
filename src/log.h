#pragma once

#include "main.h"

#if ENABLE_LOGGING == 1
void LOG(const char *str, ...);
void LOG_CALLBACK(const char *str);
#else
#define LOG(...) ((void)0)
#endif
