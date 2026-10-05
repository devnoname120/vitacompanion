#pragma once

#include <stdbool.h>
#include <string.h>

typedef void cmd_executor(char **arg_list, size_t arg_count, char *res_msg);
typedef bool cmd_validator(char **arg_list, size_t arg_count, char *res_msg);

typedef struct {
    char  *name;
    char  *description;
    size_t min_arg_count;
    size_t max_arg_count;
    cmd_validator *validator;
    cmd_executor *executor;
} cmd_definition;

const cmd_definition *cmd_get_definition(char *cmd_name);
void cmd_help(char **arg_list, size_t arg_count, char *res_msg);
void cmd_launch(char **arg_list, size_t arg_count, char *res_msg);
void cmd_nosleep(char **arg_list, size_t arg_count, char *res_msg);
void cmd_quit(char **arg_list, size_t arg_count, char *res_msg);
void cmd_reboot(char **arg_list, size_t arg_count, char *res_msg);
void cmd_screen(char **arg_list, size_t arg_count, char *res_msg);
void cmd_screenshot(char **arg_list, size_t arg_count, char *res_msg);
void cmd_version(char **arg_list, size_t arg_count, char *res_msg);
void cmd_wait(char **arg_list, size_t arg_count, char *res_msg);
void cmd_press(char **arg_list, size_t arg_count, char *res_msg);
void cmd_release(char **arg_list, size_t arg_count, char *res_msg);
