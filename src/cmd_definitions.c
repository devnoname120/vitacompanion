#include "cmd_definitions.h"
#include "input.h"
#include "nosleep.h"
#include "parser.h"
#include "version.h"
#include "vpk_install.h"
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

const cmd_definition cmd_definitions[] = {
    {.name = "help", .description = "Display this help screen", .min_arg_count = 0, .max_arg_count = 0, .validator = NULL, .executor = &cmd_help},
    {.name = "install", .description = "Install a VPK file from the Vita's storage", .min_arg_count = 1, .max_arg_count = 1, .validator = NULL, .executor = &cmd_install},
    {.name = "launch", .description = "Launch an app by Title ID", .min_arg_count = 1, .max_arg_count = 1, .validator = NULL, .executor = &cmd_launch},
    {.name = "nosleep", .description = "Control automatic suspend prevention", .min_arg_count = 1, .max_arg_count = 1, .validator = NULL, .executor = &cmd_nosleep},
    {.name = "press", .description = "Press or position a synthetic input", .min_arg_count = 1, .max_arg_count = 4, .validator = &validate_press, .executor = &cmd_press},
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
  int longest_cmd = 0;
  size_t used = 0;

  for (size_t i = 0; i < COUNT_OF(cmd_definitions); ++i) {
    int cmd_length = (int)strlen(cmd_definitions[i].name);

    if (cmd_length > longest_cmd) {
      longest_cmd = cmd_length;
    }
  }

  used = (size_t)snprintf(res_msg, CMD_RESPONSE_MAX, "%-*s\t\t%s\n",
    longest_cmd, "Command", "Description");
  if (used >= CMD_RESPONSE_MAX)
    return;

  for (size_t i = 0; i < COUNT_OF(cmd_definitions); ++i) {
    int written = snprintf(res_msg + used, CMD_RESPONSE_MAX - used,
      "%-*s\t\t%s\n", longest_cmd, cmd_definitions[i].name,
      cmd_definitions[i].description);
    if (written < 0 || (size_t)written >= CMD_RESPONSE_MAX - used)
      return;
    used += (size_t)written;
  }
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

void cmd_install(char **arg_list, size_t arg_count, char *res_msg) {
  char title_id[VPK_TITLE_ID_LENGTH + 1] = {0};
  const char *path = arg_list[1];
  int result;

  (void)arg_count;

  /* Accept the FTP form /ux0:/... as well as ux0:/... */
  if (path[0] == '/')
    path++;

  result = vpk_install(path, title_id);
  if (result < 0)
    vpk_install_format_error(result, res_msg, CMD_RESPONSE_MAX);
  else
    snprintf(res_msg, CMD_RESPONSE_MAX, "Installed %s.\n", title_id);
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
  scePowerRequestColdReset();
  strcpy(res_msg, "Rebooting...\n");
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
