/*
 * Copyright (c) 2015-2016 Sergi Granell (xerpi)
 */

#ifndef FTPVITA_PATH_H
#define FTPVITA_PATH_H

#include <stddef.h>

void ftpvita_path_from_command_arg(const char *cur_path, const char *args, char *out, size_t out_size);
void ftpvita_path_from_list_args(const char *cur_path, const char *args, char *out, size_t out_size);
void ftpvita_format_epsv_response(char *out, size_t out_size, unsigned short port);
void ftpvita_format_mdtm_response(char *out, size_t out_size,
	int year, int month, int day, int hour, int minute, int second);

#endif
