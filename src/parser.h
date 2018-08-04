#pragma once

#include <string.h>

size_t parse_cmd(char *cmd, size_t cmd_size, char **arg_list, size_t arg_max);