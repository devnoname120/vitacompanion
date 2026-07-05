#include "parser.h"

#include <string.h>
#include <stdbool.h>

// Note: maybe use https://github.com/ryanflannery/str2argv
//       doesn't seem to support positional argument decision trees though


size_t parse_cmd(char *cmd, size_t cmd_size, char **arg_list, size_t arg_max) {
    size_t arg_count = 0;
    bool in_arg = false;

    for (unsigned int i=0; i < cmd_size && arg_count < arg_max; i ++) {
        bool is_endline = cmd[i] == '\n' || cmd[i] == '\r';
        bool is_space = cmd[i] == ' ' || cmd[i] == '\t' || is_endline;

        if (is_space) {
            cmd[i] = '\0';
            in_arg = false;

            if (is_endline) {
                break;
            }
        } else if (!in_arg) {
            arg_list[arg_count] = &(cmd[i]);
            arg_count += 1;
            in_arg = true;
        }
    }

    return arg_count;
}
