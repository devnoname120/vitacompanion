#pragma once

#include <string.h>

typedef void cmd_executor(char **arg_list, size_t arg_count, char *res_msg);

typedef struct {
    char  *name;
    char  *description;
    size_t arg_count;
    cmd_executor *executor;
} cmd_definition;

const cmd_definition *cmd_get_definition(char *cmd_name);
void cmd_help(char **arg_list, size_t arg_count, char *res_msg);
void cmd_destroy(char **arg_list, size_t arg_count, char *res_msg);
void cmd_launch(char **arg_list, size_t arg_count, char *res_msg);
void cmd_reboot(char **arg_list, size_t arg_count, char *res_msg);
void cmd_screen(char **arg_list, size_t arg_count, char *res_msg);