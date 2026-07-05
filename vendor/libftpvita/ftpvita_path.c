/*
 * Copyright (c) 2015-2016 Sergi Granell (xerpi)
 */

#include "ftpvita_path.h"

#include <stdio.h>
#include <string.h>

#define FTPVITA_EOL "\r\n"

static int ascii_is_space(char c)
{
	return c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v';
}

static int ascii_is_alnum(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static void copy_string(char *out, size_t out_size, const char *src)
{
	if (out_size == 0)
		return;

	if (!src)
		src = "";

	snprintf(out, out_size, "%s", src);
}

static void copy_arg_line(const char *args, char *out, size_t out_size)
{
	size_t i = 0;

	if (out_size == 0)
		return;

	if (!args) {
		out[0] = '\0';
		return;
	}

	while (args[i] && args[i] != '\r' && args[i] != '\n' && args[i] != '\t' && i + 1 < out_size) {
		out[i] = args[i];
		i++;
	}
	out[i] = '\0';
}

static char *trim_ascii_space(char *s)
{
	char *end;

	while (*s && ascii_is_space(*s))
		s++;

	end = s + strlen(s);
	while (end > s && ascii_is_space(end[-1]))
		*--end = '\0';

	return s;
}

static int is_vita_device_path(const char *path)
{
	const char *colon;
	const char *p;

	if (!path)
		return 0;

	colon = strchr(path, ':');
	if (!colon || colon == path || colon[1] != '/')
		return 0;

	for (p = path; p < colon; p++) {
		if (!ascii_is_alnum(*p))
			return 0;
	}

	return 1;
}

static void join_relative_path(const char *cur_path, const char *arg, char *out, size_t out_size)
{
	size_t len;

	if (!cur_path || cur_path[0] == '\0')
		cur_path = "/";

	len = strlen(cur_path);
	if (strcmp(cur_path, "/") == 0) {
		snprintf(out, out_size, "/%s", arg);
	} else if (len > 0 && cur_path[len - 1] == '/') {
		snprintf(out, out_size, "%s%s", cur_path, arg);
	} else {
		snprintf(out, out_size, "%s/%s", cur_path, arg);
	}
}

void ftpvita_path_from_command_arg(const char *cur_path, const char *args, char *out, size_t out_size)
{
	char arg[1024];
	char *path;

	copy_arg_line(args, arg, sizeof(arg));
	path = trim_ascii_space(arg);

	if (path[0] == '\0') {
		copy_string(out, out_size, cur_path && cur_path[0] ? cur_path : "/");
	} else if (path[0] == '/') {
		copy_string(out, out_size, path);
	} else if (is_vita_device_path(path)) {
		snprintf(out, out_size, "/%s", path);
	} else {
		join_relative_path(cur_path, path, out, out_size);
	}
}

void ftpvita_path_from_list_args(const char *cur_path, const char *args, char *out, size_t out_size)
{
	char arg[1024];
	char *path;
	char *next;

	copy_arg_line(args, arg, sizeof(arg));
	path = trim_ascii_space(arg);

	while (path[0] == '-' && path[1] != '\0') {
		next = path;
		while (*next && !ascii_is_space(*next))
			next++;
		while (*next && ascii_is_space(*next))
			next++;
		path = next;
	}

	ftpvita_path_from_command_arg(cur_path, path, out, out_size);
}

void ftpvita_format_epsv_response(char *out, size_t out_size, unsigned short port)
{
	snprintf(out, out_size, "229 Entering Extended Passive Mode (|||%u|)" FTPVITA_EOL, (unsigned int)port);
}
