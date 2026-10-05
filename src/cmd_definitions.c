#include "cmd_definitions.h"
#include "input.h"
#include "nosleep.h"
#include "parser.h"
#include "promote.h"
#include "reboot.h"
#include "version.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>

#define COUNT_OF(arr) (sizeof(arr) / sizeof(arr[0]))
#define CMD_RESPONSE_MAX 2048

static bool validate_press(char **arg_list, size_t arg_count,
    char *res_msg);
static bool validate_release(char **arg_list, size_t arg_count,
    char *res_msg);
static bool validate_wait(char **arg_list, size_t arg_count,
    char *res_msg);
static bool append_help_line(char **cursor, size_t *remaining,
    size_t command_width, const char *command,
    const char *description);

const cmd_definition cmd_definitions[] = {
    {.name = "help", .description = "Display this help screen", .min_arg_count = 0, .max_arg_count = 0, .validator = NULL, .executor = &cmd_help},
    {.name = "launch", .description = "Launch an app by Title ID", .min_arg_count = 1, .max_arg_count = 1, .validator = NULL, .executor = &cmd_launch},
    {.name = "nosleep", .description = "Control automatic suspend prevention", .min_arg_count = 1, .max_arg_count = 1, .validator = NULL, .executor = &cmd_nosleep},
    {.name = "press", .description = "Press or position a synthetic input", .min_arg_count = 1, .max_arg_count = 4, .validator = &validate_press, .executor = &cmd_press},
    {.name = "promote", .description = "Promote an extracted app directory", .min_arg_count = 1, .max_arg_count = 1, .validator = NULL, .executor = &cmd_promote},
    {.name = "quit", .description = "Quit an app by Title ID, or all apps", .min_arg_count = 1, .max_arg_count = 1, .validator = NULL, .executor = &cmd_quit},
    {.name = "reboot", .description = "Reboot the console", .min_arg_count = 0, .max_arg_count = 0, .validator = NULL, .executor = &cmd_reboot},
    {.name = "release", .description = "Release a synthetic input", .min_arg_count = 1, .max_arg_count = 2, .validator = &validate_release, .executor = &cmd_release},
    {.name = "screen", .description = "Turn the screen on or off", .min_arg_count = 1, .max_arg_count = 1, .validator = NULL, .executor = &cmd_screen},
    {.name = "version", .description = "Display the Vita Companion version", .min_arg_count = 0, .max_arg_count = 0, .validator = NULL, .executor = &cmd_version},
    {.name = "wait", .description = "Wait for a duration such as 100ms or 3s", .min_arg_count = 1, .max_arg_count = 1, .validator = &validate_wait, .executor = &cmd_wait}
};

extern volatile int run;
extern volatile int net_connected;

const cmd_definition *cmd_get_definition(char *cmd_name) {
  for (unsigned int i = 0; i < COUNT_OF(cmd_definitions); i++) {
    if (!strcmp(cmd_name, cmd_definitions[i].name)) {
      return &(cmd_definitions[i]);
    }
  }

  return NULL;
}

void cmd_help(char **arg_list, size_t arg_count, char *res_msg) {
  char *cursor = res_msg;
  size_t remaining = CMD_RESPONSE_MAX;
  size_t longest_cmd = strlen("Command");

  (void)arg_list;
  (void)arg_count;
  res_msg[0] = '\0';

  for (size_t i = 0; i < COUNT_OF(cmd_definitions); ++i) {
    size_t cmd_length = strlen(cmd_definitions[i].name);

    if (cmd_length > longest_cmd) {
      longest_cmd = cmd_length;
    }
  }

  if (!append_help_line(&cursor, &remaining, longest_cmd,
      "Command", "Description"))
    return;

  for (size_t i = 0; i < COUNT_OF(cmd_definitions); ++i) {
    if (!append_help_line(&cursor, &remaining, longest_cmd,
        cmd_definitions[i].name, cmd_definitions[i].description))
      return;
  }
}

static bool append_help_line(char **cursor, size_t *remaining,
    size_t command_width, const char *command,
    const char *description)
{
  size_t command_length = strlen(command);
  size_t description_length = strlen(description);
  size_t padding = command_width > command_length ?
    command_width - command_length : 0;
  size_t line_length = command_length + padding + 2 +
    description_length + 1;

  if (line_length >= *remaining)
    return false;

  memcpy(*cursor, command, command_length);
  *cursor += command_length;
  memset(*cursor, ' ', padding);
  *cursor += padding;
  memcpy(*cursor, "\t\t", 2);
  *cursor += 2;
  memcpy(*cursor, description, description_length);
  *cursor += description_length;
  *(*cursor)++ = '\n';
  **cursor = '\0';
  *remaining -= line_length;
  return true;
}


void cmd_quit(char **arg_list, size_t arg_count, char *res_msg) {
  if (!strcmp(arg_list[1], "all")) {
    sceAppMgrDestroyOtherApp();
    strcpy(res_msg, "Apps quit.\n");
  } else if (sceAppMgrDestroyAppByName(arg_list[1]) < 0) {
    strcpy(res_msg, "Error: cannot quit the app. Is the TITLEID correct?\n");
  } else {
    strcpy(res_msg, "Quit.\n");
  }
}

void cmd_nosleep(char **arg_list, size_t arg_count, char *res_msg) {
  char *state = arg_list[1];

  if (!strcmp(state, "on")) {
    nosleep_set_enabled(true);
    strcpy(res_msg, "No-sleep enabled.\n");
  } else if (!strcmp(state, "off")) {
    nosleep_set_enabled(false);
    strcpy(res_msg, "No-sleep disabled.\n");
  } else if (!strcmp(state, "status")) {
    strcpy(res_msg, nosleep_is_enabled() ? "No-sleep enabled.\n" : "No-sleep disabled.\n");
  } else {
    strcpy(res_msg, "Error: param should be 'on', 'off' or 'status'\n");
  }
}

void cmd_launch(char **arg_list, size_t arg_count, char *res_msg) {
  char uri[32];

  snprintf(uri, 32, "psgm:play?titleid=%s", arg_list[1]);

  if (sceAppMgrLaunchAppByUri(0x20000, uri) < 0) {
    strcpy(res_msg, "Error: cannot launch the app. Is the TITLEID correct?\n");
  } else {
    strcpy(res_msg, "Launched.\n");
  }
}

void cmd_reboot(char **arg_list, size_t arg_count, char *res_msg) {
  (void)arg_list;
  (void)arg_count;

  reboot_request();
  strcpy(res_msg, "Rebooting...\n");
}

void cmd_promote(char **arg_list, size_t arg_count, char *res_msg) {
  int result;

  (void)arg_count;
  result = promote_directory(arg_list[1]);
  switch (result) {
    case 0:
      strcpy(res_msg, "Promoted.\n");
      break;
    case VITACOMPANION_PROMOTE_ERROR_NOT_DIRECTORY:
      strcpy(res_msg, "Error: Only directory paths are supported; VPK files are not supported.\n");
      break;
    case VITACOMPANION_PROMOTE_ERROR_PATH_TOO_LONG:
      strcpy(res_msg, "Error: Directory path is too long.\n");
      break;
    case VITACOMPANION_PROMOTE_ERROR_INVALID_LAYOUT:
      strcpy(res_msg, "Error: Directory does not contain a promotable Vita application.\n");
      break;
    case VITACOMPANION_PROMOTE_ERROR_INVALID_SFO:
      strcpy(res_msg, "Error: sce_sys/param.sfo is malformed.\n");
      break;
    case VITACOMPANION_PROMOTE_ERROR_INVALID_TITLE_ID:
      strcpy(res_msg, "Error: TITLE_ID must contain exactly nine uppercase characters.\n");
      break;
    case VITACOMPANION_PROMOTE_ERROR_NO_MEMORY:
      strcpy(res_msg, "Error: Not enough memory to prepare the directory.\n");
      break;
    case VITACOMPANION_PROMOTE_ERROR_INCOMPLETE_IO:
      strcpy(res_msg, "Error: Could not completely read or write promotion metadata.\n");
      break;
    default:
      snprintf(res_msg, CMD_RESPONSE_MAX,
        "Error: Could not promote directory (0x%08X).\n",
        (uint32_t)result);
      break;
  }
}

void cmd_screen(char **arg_list, size_t arg_count, char *res_msg) {
  char *state = arg_list[1];

  if (!strcmp(state, "on")) {
    scePowerRequestDisplayOn();
    strcpy(res_msg, "Turning display on...\n");
  } else if (!strcmp(state, "off")) {
    scePowerRequestDisplayOff();
    strcpy(res_msg, "Turning display off...\n");
  } else {
    strcpy(res_msg, "Error: param should be 'on' or 'off'\n");
  }
}

static bool validate_press(char **arg_list, size_t arg_count,
    char *res_msg)
{
  vitacompanion_input_action action;
  int result = vitacompanion_parse_press(arg_list, arg_count, &action);

  if (result < 0) {
    strcpy(res_msg, vitacompanion_input_parse_error(result));
    return false;
  }
  return true;
}

static bool validate_release(char **arg_list, size_t arg_count,
    char *res_msg)
{
  vitacompanion_input_action action;
  int result = vitacompanion_parse_release(arg_list, arg_count, &action);

  if (result < 0) {
    strcpy(res_msg, vitacompanion_input_parse_error(result));
    return false;
  }
  return true;
}

static bool validate_wait(char **arg_list, size_t arg_count,
    char *res_msg)
{
  uint32_t duration_ms;

  (void)arg_count;
  if (!parse_wait_duration_ms(arg_list[1], &duration_ms)) {
    strcpy(res_msg, "Error: Invalid duration.\n");
    return false;
  }
  return true;
}

void cmd_press(char **arg_list, size_t arg_count, char *res_msg)
{
  vitacompanion_input_action action;

  if (!input_is_ready()) {
    strcpy(res_msg, "Error: Input simulation is unavailable.\n");
    return;
  }

  if (vitacompanion_parse_press(arg_list, arg_count, &action) < 0 ||
      input_apply(&action) < 0) {
    strcpy(res_msg, "Error: Could not apply synthetic input.\n");
    return;
  }

  strcpy(res_msg, "Input pressed.\n");
}

void cmd_release(char **arg_list, size_t arg_count, char *res_msg)
{
  vitacompanion_input_action action;

  if (!input_is_ready()) {
    strcpy(res_msg, "Error: Input simulation is unavailable.\n");
    return;
  }

  if (vitacompanion_parse_release(arg_list, arg_count, &action) < 0 ||
      input_apply(&action) < 0) {
    strcpy(res_msg, "Error: Could not release synthetic input.\n");
    return;
  }

  strcpy(res_msg, "Input released.\n");
}

void cmd_wait(char **arg_list, size_t arg_count, char *res_msg)
{
  uint32_t duration_ms;

  (void)arg_count;
  if (!parse_wait_duration_ms(arg_list[1], &duration_ms)) {
    strcpy(res_msg, "Error: Invalid duration.\n");
    return;
  }

  while (duration_ms > 0 && run && net_connected) {
    uint32_t delay_ms = duration_ms > 50 ? 50 : duration_ms;
    sceKernelDelayThread(delay_ms * 1000);
    duration_ms -= delay_ms;
  }

  if (run && net_connected)
    strcpy(res_msg, "Waited.\n");
}

void cmd_version(char **arg_list, size_t arg_count, char *res_msg)
{
  (void)arg_list;
  (void)arg_count;

  if (version_format(res_msg, CMD_RESPONSE_MAX) < 0)
    strcpy(res_msg, "Error: Could not read module version.\n");
}
