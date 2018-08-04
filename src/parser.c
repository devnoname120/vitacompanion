#include "parser.h"

#include <string.h>
#include <stdbool.h>

// Note: maybe use https://github.com/ryanflannery/str2argv
//       doesn't seem to support positional argument decision trees though


size_t parse_cmd(char *cmd, size_t cmd_size, char **arg_list, size_t arg_max) {
    size_t arg_count = 0;
    char *cur_arg = cmd;

    // load my stuff\n ==> load\0my\0stuff\0
    for (unsigned int i=0; i < cmd_size && arg_count < arg_max; i ++) {
        if (cmd[i] == ' ' || cmd[i] == '\n') {
            cmd[i] = '\0';
            arg_list[arg_count] = cur_arg;

            cur_arg = &(cmd[i+1]);
            arg_count += 1;
        }

        if (cmd[i] == '\n') {
            break;
        }
    }

    return arg_count;
}
