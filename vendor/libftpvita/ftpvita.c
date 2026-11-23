/*
 * Copyright (c) 2015-2016 Sergi Granell (xerpi)
 */

#include "ftpvita.h"
#include "ftpvita_io.h"
#include "ftpvita_path.h"
#include "ftpvita_protocol.h"
#include "transfer_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/syslimits.h>

#include <psp2/kernel/threadmgr.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>

#include <psp2/rtc.h>

#define UNUSED(x) (void)(x)

#define NET_CTL_ERROR_NOT_TERMINATED ((int)0x80412102)

#ifndef FTP_PORT
#define FTP_PORT 1337
#endif

#if FTP_PORT < 1 || FTP_PORT > 65535
#error FTP_PORT must be between 1 and 65535
#endif
#define NET_INIT_SIZE (64 * 1024)
#define DEFAULT_FILE_BUF_SIZE (4 * 1024 * 1024)
#define DATA_SOCKET_TIMEOUT_US (15 * 1000 * 1000)
#define CONTROL_SOCKET_SEND_TIMEOUT_US (15 * 1000 * 1000)
#define SERVER_START_TIMEOUT_MS 5000
#define ASCII_INPUT_SIZE 4096
#define ASCII_OUTPUT_SIZE (ASCII_INPUT_SIZE * 2 + 2)

#ifndef FTPVITA_ENABLE_LOGGING
#define FTPVITA_ENABLE_LOGGING 1
#endif

#define FTP_DEFAULT_PATH   "/"

#define MAX_DEVICES 16
#define MAX_CUSTOM_COMMANDS 16
#define MAX_CLIENTS 8

/* PSVita paths are in the form:
 *     <device name>:<filename in device>
 * for example: cache0:/foo/bar
 * We will send Unix-like paths to the FTP client, like:
 *     /cache0:/foo/bar
 */

typedef struct {
	const char *cmd;
	cmd_dispatch_func func;
} cmd_dispatch_entry;

static struct {
	char name[PATH_MAX];
	int valid;
} device_list[MAX_DEVICES];

static struct {
	const char *cmd;
	cmd_dispatch_func func;
	int valid;
} custom_command_dispatchers[MAX_CUSTOM_COMMANDS];

static void *net_memory = NULL;
static int ftp_initialized = 0;
static unsigned int file_buf_size = DEFAULT_FILE_BUF_SIZE;
static SceNetInAddr vita_addr;
static SceUID server_thid = -1;
static int server_sockfd = -1;
static volatile int server_stopping = 0;
static volatile int server_start_state = 0;
static int number_clients = 0;
static unsigned int next_client_id = 0;
static ftpvita_client_info_t *client_list = NULL;
static SceUID client_list_mtx = -1;

static int netctl_init = -1;
static int net_init = -1;

#if FTPVITA_ENABLE_LOGGING == 1
static void (*info_log_cb)(const char *) = NULL;
static void (*debug_log_cb)(const char *) = NULL;

static void log_func(ftpvita_log_cb_t log_cb, const char *s, ...)
{
	if (log_cb) {
		char buf[256];
		va_list argptr;
		va_start(argptr, s);
		vsnprintf(buf, sizeof(buf), s, argptr);
		va_end(argptr);
		log_cb(buf);
	}
}

#define INFO(...) log_func(info_log_cb, __VA_ARGS__)
#define DEBUG(...) log_func(debug_log_cb, __VA_ARGS__)
#else
#define INFO(...) ((void)0)
#define DEBUG(...) ((void)0)
#endif

static void socket_set_io_timeouts(int socket, int timeout_us)
{
	sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO,
		&timeout_us, sizeof(timeout_us));
	sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO,
		&timeout_us, sizeof(timeout_us));
}

static void socket_set_send_timeout(int socket, int timeout_us)
{
	sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO,
		&timeout_us, sizeof(timeout_us));
}

static void socket_set_graceful_close(int socket)
{
	SceNetLinger linger = {1, 2};
	sceNetSetsockopt(socket, SCE_NET_SOL_SOCKET, SCE_NET_SO_LINGER,
		&linger, sizeof(linger));
}

static int client_send_ctrl_msg(ftpvita_client_info_t *client, const char *str)
{
	if (client->ctrl_send_failed)
		return -1;

	int ret = ftpvita_send_all(sceNetSend, client->ctrl_sockfd,
		str, (unsigned int)strlen(str), 0);
	if (ret < 0)
		client->ctrl_send_failed = 1;
	return ret;
}

static inline int client_send_data_msg(ftpvita_client_info_t *client, const char *str)
{
	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		return ftpvita_send_all(sceNetSend, client->data_sockfd,
			str, (unsigned int)strlen(str), 0);
	} else {
		return ftpvita_send_all(sceNetSend, client->pasv_sockfd,
			str, (unsigned int)strlen(str), 0);
	}
}

static inline int client_recv_data_raw(ftpvita_client_info_t *client, void *buf, unsigned int len)
{
	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		return sceNetRecv(client->data_sockfd, buf, len, 0);
	} else {
		return sceNetRecv(client->pasv_sockfd, buf, len, 0);
	}
}

static inline int client_send_data_raw(ftpvita_client_info_t *client, const void *buf, unsigned int len)
{
	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		return ftpvita_send_all(sceNetSend, client->data_sockfd, buf, len, 0);
	} else {
		return ftpvita_send_all(sceNetSend, client->pasv_sockfd, buf, len, 0);
	}
}

static inline const char *get_vita_path(const char *path)
{
	if (!path)
		return NULL;
	else if (path[0] == '/' && strlen(path) > 1)
		/* /cache0:/foo/bar -> cache0:/foo/bar */
		return &path[1];
	else if (strlen(path) > 0)
		return path;
	else
		return NULL;
}

static int file_exists(const char *path)
{
	SceIoStat stat;
	return (sceIoGetstat(path, &stat) >= 0);
}

static void cmd_NOOP_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "200 No operation ;)" FTPVITA_EOL);
}

static void cmd_USER_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "331 Username OK, need password b0ss." FTPVITA_EOL);
}

static void cmd_PASS_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "230 User logged in!" FTPVITA_EOL);
}

static void cmd_QUIT_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "221 Goodbye senpai :'(" FTPVITA_EOL);
}

static void cmd_SYST_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "215 UNIX Type: L8" FTPVITA_EOL);
}

static void client_close_data_connection(ftpvita_client_info_t *client);

static int client_prepare_passive_data_connection(ftpvita_client_info_t *client, SceNetSockaddrIn *picked)
{
	int ret;

	unsigned int namelen;

	if (client->data_con_type != FTP_DATA_CONNECTION_NONE)
		client_close_data_connection(client);

	/* Create data mode socket name */
	char data_socket_name[64];
	snprintf(data_socket_name, sizeof(data_socket_name),
		"FTPVita_client_%i_data_socket",
		client->num);

	/* Create the data socket */
	client->data_sockfd = sceNetSocket(data_socket_name,
		SCE_NET_AF_INET,
		SCE_NET_SOCK_STREAM,
		0);
	if (client->data_sockfd < 0)
		return client->data_sockfd;
	socket_set_io_timeouts(client->data_sockfd, DATA_SOCKET_TIMEOUT_US);

	DEBUG("PASV data socket fd: %d\n", client->data_sockfd);

	/* Fill the data socket address */
	client->data_sockaddr.sin_family = SCE_NET_AF_INET;
	client->data_sockaddr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
	/* Let the PSVita choose a port */
	client->data_sockaddr.sin_port = sceNetHtons(0);

	/* Bind the data socket address to the data socket */
	ret = sceNetBind(client->data_sockfd,
		(SceNetSockaddr *)&client->data_sockaddr,
		sizeof(client->data_sockaddr));
	DEBUG("sceNetBind(): 0x%08X\n", ret);
	if (ret < 0)
		goto error;

	/* Start listening */
	ret = sceNetListen(client->data_sockfd, 128);
	DEBUG("sceNetListen(): 0x%08X\n", ret);
	if (ret < 0)
		goto error;

	/* Get the port that the PSVita has chosen */
	namelen = sizeof(*picked);
	ret = sceNetGetsockname(client->data_sockfd, (SceNetSockaddr *)picked,
		&namelen);
	if (ret < 0)
		goto error;

	DEBUG("PASV mode port: 0x%04X\n", picked->sin_port);

	/* Set the data connection type to passive! */
	client->data_con_type = FTP_DATA_CONNECTION_PASSIVE;
	return 0;

error:
	sceNetSocketClose(client->data_sockfd);
	client->data_sockfd = -1;
	return ret;
}

static unsigned short passive_ftp_port(const SceNetSockaddrIn *picked)
{
	unsigned int port_hi = (picked->sin_port >> 0) & 0xFF;
	unsigned int port_lo = (picked->sin_port >> 8) & 0xFF;
	return (unsigned short)((port_hi << 8) | port_lo);
}

static void cmd_PASV_func(ftpvita_client_info_t *client)
{
	char cmd[512];
	SceNetSockaddrIn picked;
	unsigned short port;

	if (client->epsv_all) {
		client_send_ctrl_msg(client, "501 PASV disabled after EPSV ALL." FTPVITA_EOL);
		return;
	}

	if (client_prepare_passive_data_connection(client, &picked) < 0) {
		client_send_ctrl_msg(client, "425 Cannot open passive connection." FTPVITA_EOL);
		return;
	}

	port = passive_ftp_port(&picked);
	/* Build the command */
	snprintf(cmd, sizeof(cmd),
		"227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)" FTPVITA_EOL,
		(unsigned int)((vita_addr.s_addr >> 0) & 0xFF),
		(unsigned int)((vita_addr.s_addr >> 8) & 0xFF),
		(unsigned int)((vita_addr.s_addr >> 16) & 0xFF),
		(unsigned int)((vita_addr.s_addr >> 24) & 0xFF),
		(unsigned int)(port / 256),
		(unsigned int)(port % 256));

	client_send_ctrl_msg(client, cmd);
}

static void cmd_EPSV_func(ftpvita_client_info_t *client)
{
	char cmd[128];
	SceNetSockaddrIn picked;
	ftpvita_epsv_request_t request;
	unsigned int unsupported_protocol = 0;
	int parse_result;

	parse_result = ftpvita_parse_epsv(client->recv_cmd_args, &request,
		&unsupported_protocol);
	if (parse_result < 0) {
		client_send_ctrl_msg(client,
			"522 Network protocol not supported, use (1)." FTPVITA_EOL);
		return;
	}
	if (!parse_result) {
		client_send_ctrl_msg(client, "501 Invalid EPSV parameters." FTPVITA_EOL);
		return;
	}
	if (request == FTPVITA_EPSV_ALL) {
		if (client->data_con_type != FTP_DATA_CONNECTION_NONE)
			client_close_data_connection(client);
		client->epsv_all = 1;
		client_send_ctrl_msg(client, "200 EPSV ALL accepted." FTPVITA_EOL);
		return;
	}

	if (client_prepare_passive_data_connection(client, &picked) < 0) {
		client_send_ctrl_msg(client, "425 Cannot open passive connection." FTPVITA_EOL);
		return;
	}
	ftpvita_format_epsv_response(cmd, sizeof(cmd), passive_ftp_port(&picked));
	client_send_ctrl_msg(client, cmd);
}

static int client_prepare_active_data_connection(ftpvita_client_info_t *client,
	const SceNetInAddr *data_addr, unsigned short data_port)
{
	char data_socket_name[64];
	int ret;

	if (client->data_con_type != FTP_DATA_CONNECTION_NONE)
		client_close_data_connection(client);

	snprintf(data_socket_name, sizeof(data_socket_name),
		"FTPVita_client_%i_data_socket",
		client->num);

	client->data_sockfd = sceNetSocket(data_socket_name,
		SCE_NET_AF_INET,
		SCE_NET_SOCK_STREAM,
		0);
	if (client->data_sockfd < 0)
		return client->data_sockfd;
	socket_set_io_timeouts(client->data_sockfd, DATA_SOCKET_TIMEOUT_US);

	client->data_sockaddr = (SceNetSockaddrIn){0};
	client->data_sockaddr.sin_family = SCE_NET_AF_INET;
	client->data_sockaddr.sin_addr = *data_addr;
	client->data_sockaddr.sin_port = sceNetHtons(data_port);
	client->data_con_type = FTP_DATA_CONNECTION_ACTIVE;
	ret = 0;
	return ret;
}

static void cmd_PORT_func(ftpvita_client_info_t *client)
{
	unsigned char data_ip[4];
	unsigned short data_port;
	char ip_str[16];
	SceNetInAddr data_addr;

	if (client->epsv_all) {
		client_send_ctrl_msg(client, "501 PORT disabled after EPSV ALL." FTPVITA_EOL);
		return;
	}
	if (!ftpvita_parse_port(client->recv_cmd_args, data_ip, &data_port)) {
		client_send_ctrl_msg(client, "501 Invalid PORT parameters." FTPVITA_EOL);
		return;
	}

	snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
		(unsigned int)data_ip[0], (unsigned int)data_ip[1],
		(unsigned int)data_ip[2], (unsigned int)data_ip[3]);
	if (sceNetInetPton(SCE_NET_AF_INET, ip_str, &data_addr) != 1) {
		client_send_ctrl_msg(client, "501 Invalid PORT address." FTPVITA_EOL);
		return;
	}

	if (client_prepare_active_data_connection(client, &data_addr, data_port) < 0) {
		client_send_ctrl_msg(client, "425 Cannot open active connection." FTPVITA_EOL);
		return;
	}

	client_send_ctrl_msg(client, "200 PORT command successful." FTPVITA_EOL);
}

static void cmd_EPRT_func(ftpvita_client_info_t *client)
{
	unsigned int protocol;
	unsigned short data_port;
	char ip_str[64];
	SceNetInAddr data_addr;

	if (client->epsv_all) {
		client_send_ctrl_msg(client, "501 EPRT disabled after EPSV ALL." FTPVITA_EOL);
		return;
	}
	if (!ftpvita_parse_eprt(client->recv_cmd_args, &protocol, ip_str,
		sizeof(ip_str), &data_port)) {
		client_send_ctrl_msg(client, "501 Invalid EPRT parameters." FTPVITA_EOL);
		return;
	}
	if (protocol != 1) {
		client_send_ctrl_msg(client,
			"522 Network protocol not supported, use (1)." FTPVITA_EOL);
		return;
	}
	if (sceNetInetPton(SCE_NET_AF_INET, ip_str, &data_addr) != 1) {
		client_send_ctrl_msg(client, "501 Invalid EPRT address." FTPVITA_EOL);
		return;
	}
	if (client_prepare_active_data_connection(client, &data_addr, data_port) < 0) {
		client_send_ctrl_msg(client, "425 Cannot open active connection." FTPVITA_EOL);
		return;
	}

	client_send_ctrl_msg(client, "200 EPRT command successful." FTPVITA_EOL);
}

static int client_open_data_connection(ftpvita_client_info_t *client)
{
	int ret;

	unsigned int addrlen;

	if (client->data_con_type == FTP_DATA_CONNECTION_ACTIVE) {
		/* Connect to the client using the data socket */
		ret = sceNetConnect(client->data_sockfd,
			(SceNetSockaddr *)&client->data_sockaddr,
			sizeof(client->data_sockaddr));

		DEBUG("sceNetConnect(): 0x%08X\n", ret);
		if (ret >= 0)
			socket_set_graceful_close(client->data_sockfd);
		return ret;
	} else if (client->data_con_type == FTP_DATA_CONNECTION_PASSIVE) {
		/* Listen to the client using the data socket */
		addrlen = sizeof(client->pasv_sockaddr);
		client->pasv_sockfd = sceNetAccept(client->data_sockfd,
			(SceNetSockaddr *)&client->pasv_sockaddr,
			&addrlen);
		DEBUG("PASV client fd: 0x%08X\n", client->pasv_sockfd);
		if (client->pasv_sockfd >= 0) {
			socket_set_io_timeouts(client->pasv_sockfd, DATA_SOCKET_TIMEOUT_US);
			socket_set_graceful_close(client->pasv_sockfd);
		}
		return client->pasv_sockfd;
	}

	return SCE_NET_ERROR_ENOTCONN;
}

static void client_close_data_connection(ftpvita_client_info_t *client)
{
	sceNetSocketClose(client->data_sockfd);
	/* In passive mode we have to close the client pasv socket too */
	if (client->data_con_type == FTP_DATA_CONNECTION_PASSIVE) {
		sceNetSocketClose(client->pasv_sockfd);
	}
	client->data_sockfd = -1;
	client->pasv_sockfd = -1;
	client->data_con_type = FTP_DATA_CONNECTION_NONE;
}

static int gen_list_format(char *out, size_t out_size, int dir,
	const SceIoStat *stat, const char *filename)
{
	static const char num_to_month[][4] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};

	char yt[6];
	SceDateTime cdt;
	sceRtcGetCurrentClockLocalTime(&cdt);

	if  (cdt.year == stat->st_mtime.year) {
		snprintf(yt, sizeof(yt), "%02u:%02u",
			(unsigned int)stat->st_mtime.hour % 24,
			(unsigned int)stat->st_mtime.minute % 60);
	}
	else {
		snprintf(yt, sizeof(yt), "%04d", stat->st_mtime.year);
	}

	return snprintf(out, out_size,
		"%c%s 1 vita vita %u %s %-2d %s %.400s" FTPVITA_EOL,
		dir ? 'd' : '-',
		dir ? "rwxr-xr-x" : "rw-r--r--",
		(unsigned int) stat->st_size,
		num_to_month[stat->st_mtime.month<=0?0:(stat->st_mtime.month-1)%12],
		stat->st_mtime.day,
		yt,
		filename);
}

static void send_LIST(ftpvita_client_info_t *client, const char *path)
{
	int i;
	char buffer[512];
	SceUID dir;
	SceIoDirent dirent;
	SceIoStat stat;
	char *devname;
	int send_devices = 0;
	int transfer_ok = 1;

	/* "/" path is a special case, if we are here we have
	 * to send the list of devices (aka mountpoints). */
	if (strcmp(path, "/") == 0) {
		send_devices = 1;
	}

	if (!send_devices) {
		dir = sceIoDopen(get_vita_path(path));
		if (dir < 0) {
			client_send_ctrl_msg(client, "550 Invalid directory." FTPVITA_EOL);
			return;
		}
	}

	client_send_ctrl_msg(client, "150 Opening ASCII mode data transfer for LIST." FTPVITA_EOL);

	if (client_open_data_connection(client) < 0) {
		if (!send_devices)
			sceIoDclose(dir);
		if (client->data_con_type != FTP_DATA_CONNECTION_NONE)
			client_close_data_connection(client);
		client_send_ctrl_msg(client, "425 Cannot open data connection." FTPVITA_EOL);
		return;
	}

	if (send_devices) {
		for (i = 0; i < MAX_DEVICES; i++) {
			if (device_list[i].valid) {
				devname = device_list[i].name;
				if (sceIoGetstat(devname, &stat) >= 0) {
					gen_list_format(buffer, sizeof(buffer), 1, &stat, devname);
					if (client_send_data_msg(client, buffer) < 0) {
						transfer_ok = 0;
						break;
					}
				}
			}
		}
	} else {
		memset(&dirent, 0, sizeof(dirent));

		while (sceIoDread(dir, &dirent) > 0) {
			gen_list_format(buffer, sizeof(buffer), SCE_S_ISDIR(dirent.d_stat.st_mode),
				&dirent.d_stat, dirent.d_name);
			if (client_send_data_msg(client, buffer) < 0) {
				transfer_ok = 0;
				break;
			}
			memset(&dirent, 0, sizeof(dirent));
			memset(buffer, 0, sizeof(buffer));
		}

		sceIoDclose(dir);
	}

	DEBUG("Done sending LIST\n");

	client_close_data_connection(client);
	if (transfer_ok)
		client_send_ctrl_msg(client, "226 Transfer complete." FTPVITA_EOL);
	else
		client_send_ctrl_msg(client, "426 Connection closed; transfer aborted." FTPVITA_EOL);
}

static void cmd_LIST_func(ftpvita_client_info_t *client)
{
	char list_path[PATH_MAX];

	ftpvita_path_from_list_args(client->cur_path, client->recv_cmd_args, list_path, sizeof(list_path));
	send_LIST(client, list_path);
}

static void send_NLST(ftpvita_client_info_t *client, const char *path)
{
	int i;
	char buffer[PATH_MAX + 4];
	SceUID dir;
	SceIoDirent dirent;
	SceIoStat stat;
	char *devname;
	int send_devices = 0;
	int transfer_ok = 1;

	if (strcmp(path, "/") == 0) {
		send_devices = 1;
	}

	if (!send_devices) {
		dir = sceIoDopen(get_vita_path(path));
		if (dir < 0) {
			client_send_ctrl_msg(client, "550 Invalid directory." FTPVITA_EOL);
			return;
		}
	}

	client_send_ctrl_msg(client, "150 Opening ASCII mode data transfer for NLST." FTPVITA_EOL);

	if (client_open_data_connection(client) < 0) {
		if (!send_devices)
			sceIoDclose(dir);
		if (client->data_con_type != FTP_DATA_CONNECTION_NONE)
			client_close_data_connection(client);
		client_send_ctrl_msg(client, "425 Cannot open data connection." FTPVITA_EOL);
		return;
	}

	if (send_devices) {
		for (i = 0; i < MAX_DEVICES; i++) {
			if (device_list[i].valid) {
				devname = device_list[i].name;
				if (sceIoGetstat(devname, &stat) >= 0) {
					snprintf(buffer, sizeof(buffer), "%.1020s" FTPVITA_EOL, devname);
					if (client_send_data_msg(client, buffer) < 0) {
						transfer_ok = 0;
						break;
					}
				}
			}
		}
	} else {
		memset(&dirent, 0, sizeof(dirent));

		while (sceIoDread(dir, &dirent) > 0) {
			snprintf(buffer, sizeof(buffer), "%.1020s" FTPVITA_EOL,
				dirent.d_name);
			if (client_send_data_msg(client, buffer) < 0) {
				transfer_ok = 0;
				break;
			}
			memset(&dirent, 0, sizeof(dirent));
		}

		sceIoDclose(dir);
	}

	DEBUG("Done sending NLST\n");

	client_close_data_connection(client);
	if (transfer_ok)
		client_send_ctrl_msg(client, "226 Transfer complete." FTPVITA_EOL);
	else
		client_send_ctrl_msg(client, "426 Connection closed; transfer aborted." FTPVITA_EOL);
}

static void cmd_NLST_func(ftpvita_client_info_t *client)
{
	char list_path[PATH_MAX];

	ftpvita_path_from_list_args(client->cur_path, client->recv_cmd_args, list_path, sizeof(list_path));
	send_NLST(client, list_path);
}

static int gen_mlsx_format(char *out, size_t out_size,
	const SceIoStat *stat, const char *filename)
{
	const char *type = SCE_S_ISDIR(stat->st_mode) ? "dir" : "file";
	char size_string[21];

	if (SCE_S_ISDIR(stat->st_mode)) {
		return snprintf(out, out_size,
			"type=%s;modify=%04d%02d%02d%02d%02d%02d; %.1023s" FTPVITA_EOL,
			type, stat->st_mtime.year, stat->st_mtime.month,
			stat->st_mtime.day, stat->st_mtime.hour,
			stat->st_mtime.minute, stat->st_mtime.second, filename);
	}

	if (!ftpvita_format_u64_decimal(size_string, sizeof(size_string),
		(unsigned long long)stat->st_size))
		return -1;
	return snprintf(out, out_size,
		"type=%s;size=%s;modify=%04d%02d%02d%02d%02d%02d; %.1023s" FTPVITA_EOL,
		type, size_string, stat->st_mtime.year, stat->st_mtime.month,
		stat->st_mtime.day, stat->st_mtime.hour,
		stat->st_mtime.minute, stat->st_mtime.second, filename);
}

static void send_MLSD(ftpvita_client_info_t *client, const char *path)
{
	int i;
	char buffer[PATH_MAX + 128];
	SceUID dir = -1;
	SceIoDirent dirent;
	SceIoStat stat;
	int send_devices = strcmp(path, "/") == 0;
	int transfer_ok = 1;

	if (!send_devices) {
		dir = sceIoDopen(get_vita_path(path));
		if (dir < 0) {
			client_send_ctrl_msg(client, "550 Invalid directory." FTPVITA_EOL);
			return;
		}
	}

	client_send_ctrl_msg(client, "150 Opening data connection for MLSD." FTPVITA_EOL);
	if (client_open_data_connection(client) < 0) {
		if (dir >= 0)
			sceIoDclose(dir);
		if (client->data_con_type != FTP_DATA_CONNECTION_NONE)
			client_close_data_connection(client);
		client_send_ctrl_msg(client, "425 Cannot open data connection." FTPVITA_EOL);
		return;
	}

	if (send_devices) {
		for (i = 0; i < MAX_DEVICES; i++) {
			if (device_list[i].valid &&
				sceIoGetstat(device_list[i].name, &stat) >= 0) {
				gen_mlsx_format(buffer, sizeof(buffer), &stat,
					device_list[i].name);
				if (client_send_data_msg(client, buffer) < 0) {
					transfer_ok = 0;
					break;
				}
			}
		}
	} else {
		memset(&dirent, 0, sizeof(dirent));
		while (sceIoDread(dir, &dirent) > 0) {
			gen_mlsx_format(buffer, sizeof(buffer), &dirent.d_stat,
				dirent.d_name);
			if (client_send_data_msg(client, buffer) < 0) {
				transfer_ok = 0;
				break;
			}
			memset(&dirent, 0, sizeof(dirent));
		}
		sceIoDclose(dir);
	}

	client_close_data_connection(client);
	client_send_ctrl_msg(client, transfer_ok ?
		"226 MLSD completed." FTPVITA_EOL :
		"426 Connection closed; transfer aborted." FTPVITA_EOL);
}

static void cmd_MLSD_func(ftpvita_client_info_t *client)
{
	char list_path[PATH_MAX];

	ftpvita_path_from_command_arg(client->cur_path, client->recv_cmd_args,
		list_path, sizeof(list_path));
	send_MLSD(client, list_path);
}

static void cmd_MLST_func(ftpvita_client_info_t *client)
{
	char path[PATH_MAX];
	char facts[PATH_MAX + 128];
	SceIoStat stat;

	ftpvita_path_from_command_arg(client->cur_path, client->recv_cmd_args,
		path, sizeof(path));

	if (strcmp(path, "/") == 0) {
		memset(&stat, 0, sizeof(stat));
		stat.st_mode = SCE_S_IFDIR;
	} else if (sceIoGetstat(get_vita_path(path), &stat) < 0) {
		client_send_ctrl_msg(client, "550 Path not found." FTPVITA_EOL);
		return;
	}

	client_send_ctrl_msg(client, "250-Listing follows." FTPVITA_EOL);
	facts[0] = ' ';
	gen_mlsx_format(facts + 1, sizeof(facts) - 1, &stat, path);
	client_send_ctrl_msg(client, facts);
	client_send_ctrl_msg(client, "250 End." FTPVITA_EOL);
}

static void cmd_PWD_func(ftpvita_client_info_t *client)
{
	char msg[PATH_MAX + 64];
	snprintf(msg, sizeof(msg), "257 \"%s\" is the current directory." FTPVITA_EOL, client->cur_path);
	client_send_ctrl_msg(client, msg);
}

static int path_is_at_root(const char *path)
{
	return strrchr(path, '/') == (path + strlen(path) - 1);
}

static void dir_up(char *path)
{
	char *pch;
	size_t len_in = strlen(path);
	if (len_in == 1) {
		strcpy(path, "/");
		return;
	}
	if (path_is_at_root(path)) { /* Case root of the device (/foo0:/) */
		strcpy(path, "/");
	} else {
		pch = strrchr(path, '/');
		size_t s = len_in - (size_t)(pch - path);
		memset(pch, '\0', s);
		/* If the path is like: /foo: add slash */
		if (strrchr(path, '/') == path)
			strcat(path, "/");
	}
}

static int command_has_argument(ftpvita_client_info_t *client);

static void cmd_CWD_func(ftpvita_client_info_t *client)
{
	char tmp_path[PATH_MAX];
	SceUID pd;

	if (!command_has_argument(client)) {
		client_send_ctrl_msg(client, "501 CWD requires a pathname." FTPVITA_EOL);
		return;
	}

	if (ftpvita_command_is(client->recv_cmd_args, "..")) {
		dir_up(client->cur_path);
		client_send_ctrl_msg(client,
			"250 Requested file action okay, completed." FTPVITA_EOL);
		return;
	}

	ftpvita_path_from_command_arg(client->cur_path, client->recv_cmd_args,
		tmp_path, sizeof(tmp_path));
	if (strcmp(tmp_path, "/") != 0) {
		pd = sceIoDopen(get_vita_path(tmp_path));
		if (pd < 0) {
			client_send_ctrl_msg(client, "550 Invalid directory." FTPVITA_EOL);
			return;
		}
		sceIoDclose(pd);
	}

	snprintf(client->cur_path, sizeof(client->cur_path), "%s", tmp_path);
	client_send_ctrl_msg(client,
		"250 Requested file action okay, completed." FTPVITA_EOL);
}

static void cmd_TYPE_func(ftpvita_client_info_t *client)
{
	char data_type;

	if (!ftpvita_parse_type(client->recv_cmd_args, &data_type)) {
		client_send_ctrl_msg(client, "504 Unsupported TYPE parameters." FTPVITA_EOL);
		return;
	}

	client->transfer_type = data_type == 'A' ?
		FTP_TRANSFER_TYPE_ASCII : FTP_TRANSFER_TYPE_IMAGE;
	client_send_ctrl_msg(client, data_type == 'A' ?
		"200 Type set to A." FTPVITA_EOL :
		"200 Type set to I." FTPVITA_EOL);
}

static void cmd_MODE_func(ftpvita_client_info_t *client)
{
	if (ftpvita_command_is(client->recv_cmd_args, "S"))
		client_send_ctrl_msg(client, "200 Mode set to S." FTPVITA_EOL);
	else
		client_send_ctrl_msg(client, "504 Only stream mode is supported." FTPVITA_EOL);
}

static void cmd_STRU_func(ftpvita_client_info_t *client)
{
	if (ftpvita_command_is(client->recv_cmd_args, "F"))
		client_send_ctrl_msg(client, "200 Structure set to F." FTPVITA_EOL);
	else
		client_send_ctrl_msg(client, "504 Only file structure is supported." FTPVITA_EOL);
}

static void cmd_CDUP_func(ftpvita_client_info_t *client)
{
	dir_up(client->cur_path);
	client_send_ctrl_msg(client, "200 Command okay." FTPVITA_EOL);
}

static int file_write_all(SceUID fd, const unsigned char *buffer,
	unsigned int length)
{
	unsigned int written = 0;

	while (written < length) {
		int result = sceIoWrite(fd, buffer + written, length - written);
		if (result <= 0)
			return result < 0 ? result : (int)SCE_NET_ERROR_EIO;
		written += (unsigned int)result;
	}

	return (int)written;
}

static void send_file(ftpvita_client_info_t *client, const char *path)
{
	unsigned char *buffer = NULL;
	unsigned char *ascii_buffer = NULL;
	unsigned int transfer_slot = FTPVITA_TRANSFER_INVALID_SLOT;
	SceUID fd;
	int bytes_read;
	int pool_result = FTPVITA_TRANSFER_OK;
	int transfer_ok = 1;
	int uses_pool = client->transfer_type != FTP_TRANSFER_TYPE_ASCII;
	unsigned int restore_point = client->restore_point;
	ftpvita_ascii_state_t ascii_state;

	DEBUG("Opening: %s\n", path);
	client->restore_point = 0;
	if (uses_pool) {
		pool_result = ftpvita_transfer_pool_acquire(path,
			FTPVITA_TRANSFER_READ, (void **)&buffer, &transfer_slot);
		if (pool_result != FTPVITA_TRANSFER_OK) {
			if (client->data_con_type != FTP_DATA_CONNECTION_NONE)
				client_close_data_connection(client);
			if (pool_result == FTPVITA_TRANSFER_PATH_BUSY)
				client_send_ctrl_msg(client, "450 File busy." FTPVITA_EOL);
			else if (pool_result == FTPVITA_TRANSFER_NO_SLOT)
				client_send_ctrl_msg(client,
					"450 Transfer slot unavailable; retry later." FTPVITA_EOL);
			else
				client_send_ctrl_msg(client,
					"451 Transfer admission failed." FTPVITA_EOL);
			return;
		}
	}

	if ((fd = sceIoOpen(path, SCE_O_RDONLY, 0777)) >= 0) {

		if (sceIoLseek32(fd, (long)restore_point, SCE_SEEK_SET) < 0) {
			sceIoClose(fd);
			if (uses_pool)
				ftpvita_transfer_pool_release(transfer_slot);
			client_send_ctrl_msg(client, "550 Invalid restart offset." FTPVITA_EOL);
			return;
		}

		if (client->transfer_type == FTP_TRANSFER_TYPE_ASCII) {
			buffer = malloc(ASCII_INPUT_SIZE);
			if (buffer == NULL) {
				sceIoClose(fd);
				client_send_ctrl_msg(client,
					"550 Could not allocate memory." FTPVITA_EOL);
				return;
			}
			ascii_buffer = malloc(ASCII_OUTPUT_SIZE);
			if (ascii_buffer == NULL) {
				sceIoClose(fd);
				free(buffer);
				client_send_ctrl_msg(client, "550 Could not allocate memory." FTPVITA_EOL);
				return;
			}
			ftpvita_ascii_state_init(&ascii_state);
		}

		if (client_open_data_connection(client) < 0) {
			sceIoClose(fd);
			if (uses_pool)
				ftpvita_transfer_pool_release(transfer_slot);
			else {
				free(buffer);
				free(ascii_buffer);
			}
			if (client->data_con_type != FTP_DATA_CONNECTION_NONE)
				client_close_data_connection(client);
			client_send_ctrl_msg(client, "425 Cannot open data connection." FTPVITA_EOL);
			return;
		}
		client_send_ctrl_msg(client,
			client->transfer_type == FTP_TRANSFER_TYPE_ASCII ?
			"150 Opening ASCII mode data transfer." FTPVITA_EOL :
			"150 Opening Image mode data transfer." FTPVITA_EOL);

		while ((bytes_read = sceIoRead(fd, buffer,
			client->transfer_type == FTP_TRANSFER_TYPE_ASCII ?
			ASCII_INPUT_SIZE : file_buf_size)) > 0) {
			const unsigned char *send_buffer = buffer;
			size_t send_size = (size_t)bytes_read;

			if (client->transfer_type == FTP_TRANSFER_TYPE_ASCII) {
				send_size = ftpvita_ascii_encode(&ascii_state, buffer,
					(size_t)bytes_read, ascii_buffer, ASCII_OUTPUT_SIZE);
				if (send_size == (size_t)-1) {
					transfer_ok = 0;
					break;
				}
				send_buffer = ascii_buffer;
			}

			if (client_send_data_raw(client, send_buffer,
				(unsigned int)send_size) != (int)send_size) {
				transfer_ok = 0;
				break;
			}
		}
		if (bytes_read < 0)
			transfer_ok = 0;
		if (transfer_ok && client->transfer_type == FTP_TRANSFER_TYPE_ASCII) {
			size_t send_size = ftpvita_ascii_finish_encode(&ascii_state,
				ascii_buffer, ASCII_OUTPUT_SIZE);
			if (send_size == (size_t)-1 ||
				client_send_data_raw(client, ascii_buffer,
				(unsigned int)send_size) != (int)send_size)
				transfer_ok = 0;
		}

		sceIoClose(fd);
		if (uses_pool)
			ftpvita_transfer_pool_release(transfer_slot);
		else {
			free(buffer);
			free(ascii_buffer);
		}
		client_close_data_connection(client);
		if (transfer_ok)
			client_send_ctrl_msg(client, "226 Transfer completed." FTPVITA_EOL);
		else
			client_send_ctrl_msg(client, "426 Connection closed; transfer aborted." FTPVITA_EOL);

	} else {
		if (uses_pool)
			ftpvita_transfer_pool_release(transfer_slot);
		client_send_ctrl_msg(client, "550 File not found." FTPVITA_EOL);
	}
}

/* This function generates an FTP full-path with the input path (relative or absolute)
 * from RETR, STOR, DELE, RMD, MKD, RNFR and RNTO commands */
static void gen_ftp_fullpath(ftpvita_client_info_t *client, char *path, size_t path_size)
{
	ftpvita_path_from_command_arg(client->cur_path, client->recv_cmd_args, path, path_size);
}

static int command_has_argument(ftpvita_client_info_t *client)
{
	const char *arg = client->recv_cmd_args;

	while (*arg == ' ' || *arg == '\t')
		arg++;
	return *arg != '\0';
}

static void cmd_RETR_func(ftpvita_client_info_t *client)
{
	char dest_path[PATH_MAX];
	if (!command_has_argument(client)) {
		client->restore_point = 0;
		client_send_ctrl_msg(client, "501 RETR requires a pathname." FTPVITA_EOL);
		return;
	}
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));
	send_file(client, get_vita_path(dest_path));
}

static void receive_file(ftpvita_client_info_t *client, const char *path,
	int append)
{
	unsigned char *buffer = NULL;
	unsigned char *ascii_buffer = NULL;
	unsigned int transfer_slot = FTPVITA_TRANSFER_INVALID_SLOT;
	SceUID fd;
	int bytes_recv;
	int pool_result = FTPVITA_TRANSFER_OK;
	int uses_pool = client->transfer_type != FTP_TRANSFER_TYPE_ASCII;
	unsigned int restore_point = client->restore_point;
	ftpvita_ascii_state_t ascii_state;

	DEBUG("Opening: %s\n", path);
	client->restore_point = 0;
	if (uses_pool) {
		pool_result = ftpvita_transfer_pool_acquire(path,
			FTPVITA_TRANSFER_WRITE, (void **)&buffer, &transfer_slot);
		if (pool_result != FTPVITA_TRANSFER_OK) {
			if (client->data_con_type != FTP_DATA_CONNECTION_NONE)
				client_close_data_connection(client);
			if (pool_result == FTPVITA_TRANSFER_PATH_BUSY)
				client_send_ctrl_msg(client, "450 File busy." FTPVITA_EOL);
			else if (pool_result == FTPVITA_TRANSFER_NO_SLOT)
				client_send_ctrl_msg(client,
					"450 Transfer slot unavailable; retry later." FTPVITA_EOL);
			else
				client_send_ctrl_msg(client,
					"451 Transfer admission failed." FTPVITA_EOL);
			return;
		}
	}

	int mode = SCE_O_CREAT | SCE_O_RDWR;
	if (append)
		mode |= SCE_O_APPEND;
	else if (restore_point == 0)
		mode |= SCE_O_TRUNC;

	if ((fd = sceIoOpen(path, mode, 0777)) >= 0) {
		if (!append && restore_point > 0 &&
			sceIoLseek32(fd, (long)restore_point, SCE_SEEK_SET) < 0) {
			sceIoClose(fd);
			if (uses_pool)
				ftpvita_transfer_pool_release(transfer_slot);
			client_send_ctrl_msg(client, "550 Invalid restart offset." FTPVITA_EOL);
			return;
		}

		if (client->transfer_type == FTP_TRANSFER_TYPE_ASCII) {
			buffer = malloc(ASCII_INPUT_SIZE);
			if (buffer == NULL) {
				sceIoClose(fd);
				client_send_ctrl_msg(client,
					"550 Could not allocate memory." FTPVITA_EOL);
				return;
			}
			ascii_buffer = malloc(ASCII_OUTPUT_SIZE);
			if (ascii_buffer == NULL) {
				sceIoClose(fd);
				free(buffer);
				client_send_ctrl_msg(client, "550 Could not allocate memory." FTPVITA_EOL);
				return;
			}
			ftpvita_ascii_state_init(&ascii_state);
		}

		if (client_open_data_connection(client) < 0) {
			sceIoClose(fd);
			if (uses_pool)
				ftpvita_transfer_pool_release(transfer_slot);
			else {
				free(buffer);
				free(ascii_buffer);
			}
			if (client->data_con_type != FTP_DATA_CONNECTION_NONE)
				client_close_data_connection(client);
			sceIoRemove(path);
			client_send_ctrl_msg(client, "425 Cannot open data connection." FTPVITA_EOL);
			return;
		}
		client_send_ctrl_msg(client,
			client->transfer_type == FTP_TRANSFER_TYPE_ASCII ?
			"150 Opening ASCII mode data transfer." FTPVITA_EOL :
			"150 Opening Image mode data transfer." FTPVITA_EOL);

		while ((bytes_recv = client_recv_data_raw(client, buffer,
			client->transfer_type == FTP_TRANSFER_TYPE_ASCII ?
			ASCII_INPUT_SIZE : file_buf_size)) > 0) {
			const unsigned char *write_buffer = buffer;
			size_t write_size = (size_t)bytes_recv;

			if (client->transfer_type == FTP_TRANSFER_TYPE_ASCII) {
				write_size = ftpvita_ascii_decode(&ascii_state, buffer,
					(size_t)bytes_recv, ascii_buffer, ASCII_OUTPUT_SIZE);
				if (write_size == (size_t)-1) {
					bytes_recv = SCE_NET_ERROR_EIO;
					break;
				}
				write_buffer = ascii_buffer;
			}

			if (file_write_all(fd, write_buffer, (unsigned int)write_size) !=
				(int)write_size) {
				bytes_recv = SCE_NET_ERROR_EIO;
				break;
			}
		}
		if (bytes_recv == 0 &&
			client->transfer_type == FTP_TRANSFER_TYPE_ASCII) {
			size_t write_size = ftpvita_ascii_finish_decode(&ascii_state,
				ascii_buffer, ASCII_OUTPUT_SIZE);
			if (write_size == (size_t)-1 ||
				file_write_all(fd, ascii_buffer, (unsigned int)write_size) !=
				(int)write_size)
				bytes_recv = SCE_NET_ERROR_EIO;
		}

		sceIoClose(fd);
		if (uses_pool)
			ftpvita_transfer_pool_release(transfer_slot);
		else {
			free(buffer);
			free(ascii_buffer);
		}
		if (bytes_recv == 0) {
			client_send_ctrl_msg(client, "226 Transfer completed." FTPVITA_EOL);
		} else {
			sceIoRemove(path);
			client_send_ctrl_msg(client, "426 Connection closed; transfer aborted." FTPVITA_EOL);
		}
		client_close_data_connection(client);

	} else {
		if (uses_pool)
			ftpvita_transfer_pool_release(transfer_slot);
		client_send_ctrl_msg(client, "550 File not found." FTPVITA_EOL);
	}
}

static void cmd_STOR_func(ftpvita_client_info_t *client)
{
	char dest_path[PATH_MAX];
	if (!command_has_argument(client)) {
		client->restore_point = 0;
		client_send_ctrl_msg(client, "501 STOR requires a pathname." FTPVITA_EOL);
		return;
	}
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));
	receive_file(client, get_vita_path(dest_path), 0);
}

static void delete_file(ftpvita_client_info_t *client, const char *path)
{
	DEBUG("Deleting: %s\n", path);

	if (sceIoRemove(path) >= 0) {
		client_send_ctrl_msg(client, "250 File deleted." FTPVITA_EOL);
	} else {
		client_send_ctrl_msg(client, "550 Could not delete the file." FTPVITA_EOL);
	}
}

static void cmd_DELE_func(ftpvita_client_info_t *client)
{
	char dest_path[PATH_MAX];
	if (!command_has_argument(client)) {
		client_send_ctrl_msg(client, "501 DELE requires a pathname." FTPVITA_EOL);
		return;
	}
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));
	delete_file(client, get_vita_path(dest_path));
}

static void delete_dir(ftpvita_client_info_t *client, const char *path)
{
	int ret;
	DEBUG("Deleting: %s\n", path);
	ret = sceIoRmdir(path);
	if (ret >= 0) {
		client_send_ctrl_msg(client, "250 Directory deleted." FTPVITA_EOL);
	} else if (ret == (int)0x8001005A) { /* DIRECTORY_IS_NOT_EMPTY */
		client_send_ctrl_msg(client, "550 Directory is not empty." FTPVITA_EOL);
	} else {
		client_send_ctrl_msg(client, "550 Could not delete the directory." FTPVITA_EOL);
	}
}

static void cmd_RMD_func(ftpvita_client_info_t *client)
{
	char dest_path[PATH_MAX];
	if (!command_has_argument(client)) {
		client_send_ctrl_msg(client, "501 RMD requires a pathname." FTPVITA_EOL);
		return;
	}
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));
	delete_dir(client, get_vita_path(dest_path));
}

static void cmd_MKD_func(ftpvita_client_info_t *client)
{
	char dest_path[PATH_MAX];
	char response[PATH_MAX + 64];
	if (!command_has_argument(client)) {
		client_send_ctrl_msg(client, "501 MKD requires a pathname." FTPVITA_EOL);
		return;
	}
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));

	DEBUG("Creating: %s\n", get_vita_path(dest_path));
	if (sceIoMkdir(get_vita_path(dest_path), 0777) >= 0) {
		snprintf(response, sizeof(response), "257 \"%s\" created." FTPVITA_EOL,
			dest_path);
		client_send_ctrl_msg(client, response);
	} else {
		client_send_ctrl_msg(client, "550 Could not create the directory." FTPVITA_EOL);
	}
}

static void cmd_RNFR_func(ftpvita_client_info_t *client)
{
	char path_src[PATH_MAX];
	const char *vita_path_src;
	client->rename_path[0] = '\0';
	if (!command_has_argument(client)) {
		client_send_ctrl_msg(client, "501 RNFR requires a pathname." FTPVITA_EOL);
		return;
	}
	/* Get the origin filename */
	gen_ftp_fullpath(client, path_src, sizeof(path_src));
	vita_path_src = get_vita_path(path_src);

	/* Check if the file exists */
	if (!file_exists(vita_path_src)) {
		client_send_ctrl_msg(client, "550 The file doesn't exist." FTPVITA_EOL);
		return;
	}
	/* The file to be renamed is the received path */
	strcpy(client->rename_path, vita_path_src);
	client_send_ctrl_msg(client, "350 I need the destination name b0ss." FTPVITA_EOL);
}

static void cmd_RNTO_func(ftpvita_client_info_t *client)
{
	char path_dst[PATH_MAX];
	const char *vita_path_dst;
	if (client->rename_path[0] == '\0') {
		client_send_ctrl_msg(client, "503 RNFR required before RNTO." FTPVITA_EOL);
		return;
	}
	if (!command_has_argument(client)) {
		client->rename_path[0] = '\0';
		client_send_ctrl_msg(client, "501 RNTO requires a pathname." FTPVITA_EOL);
		return;
	}
	/* Get the destination filename */
	gen_ftp_fullpath(client, path_dst,sizeof(path_dst));
	vita_path_dst = get_vita_path(path_dst);

	DEBUG("Renaming: %s to %s\n", client->rename_path, vita_path_dst);

	if (sceIoRename(client->rename_path, vita_path_dst) < 0) {
		client->rename_path[0] = '\0';
		client_send_ctrl_msg(client, "550 Error renaming the file." FTPVITA_EOL);
		return;
	}

	client->rename_path[0] = '\0';
	client_send_ctrl_msg(client, "250 Rename completed." FTPVITA_EOL);
}

static void cmd_SIZE_func(ftpvita_client_info_t *client)
{
	SceIoStat stat;
	char path[PATH_MAX];
	char cmd[64];
	char size_string[21];
	unsigned long long transfer_size;
	if (!command_has_argument(client)) {
		client_send_ctrl_msg(client, "501 SIZE requires a pathname." FTPVITA_EOL);
		return;
	}
	/* Get the filename to retrieve its size */
	gen_ftp_fullpath(client, path, sizeof(path));

	/* Check if the file exists */
	if (sceIoGetstat(get_vita_path(path), &stat) < 0) {
		client_send_ctrl_msg(client, "550 The file doesn't exist." FTPVITA_EOL);
		return;
	}
	if (SCE_S_ISDIR(stat.st_mode)) {
		client_send_ctrl_msg(client, "550 SIZE requires a regular file." FTPVITA_EOL);
		return;
	}

	transfer_size = (unsigned long long)stat.st_size;
	if (client->transfer_type == FTP_TRANSFER_TYPE_ASCII) {
		unsigned char *buffer;
		SceUID fd;
		int bytes_read;
		int pending_cr = 0;

		fd = sceIoOpen(get_vita_path(path), SCE_O_RDONLY, 0777);
		buffer = malloc(ASCII_INPUT_SIZE);
		if (fd < 0 || !buffer) {
			if (fd >= 0)
				sceIoClose(fd);
			free(buffer);
			client_send_ctrl_msg(client,
				"451 Could not calculate ASCII transfer size." FTPVITA_EOL);
			return;
		}

		transfer_size = 0;
		while ((bytes_read = sceIoRead(fd, buffer, ASCII_INPUT_SIZE)) > 0) {
			int i;
			for (i = 0; i < bytes_read; i++) {
				unsigned char value = buffer[i];
				if (pending_cr) {
					transfer_size += 2;
					pending_cr = 0;
					if (value == '\n')
						continue;
				}
				if (value == '\r')
					pending_cr = 1;
				else
					transfer_size += value == '\n' ? 2 : 1;
			}
		}
		if (pending_cr)
			transfer_size += 2;
		sceIoClose(fd);
		free(buffer);
		if (bytes_read < 0) {
			client_send_ctrl_msg(client,
				"451 Could not calculate ASCII transfer size." FTPVITA_EOL);
			return;
		}
	}
	/* Send the size of the file */
	if (!ftpvita_format_u64_decimal(size_string, sizeof(size_string),
		transfer_size)) {
		client_send_ctrl_msg(client,
			"451 Could not format transfer size." FTPVITA_EOL);
		return;
	}
	snprintf(cmd, sizeof(cmd), "213 %s" FTPVITA_EOL, size_string);
	client_send_ctrl_msg(client, cmd);
}

static void cmd_MDTM_func(ftpvita_client_info_t *client)
{
	SceIoStat stat;
	char path[PATH_MAX];
	char cmd[64];

	if (!command_has_argument(client)) {
		client_send_ctrl_msg(client, "501 MDTM requires a pathname." FTPVITA_EOL);
		return;
	}
	gen_ftp_fullpath(client, path, sizeof(path));

	if (sceIoGetstat(get_vita_path(path), &stat) < 0) {
		client_send_ctrl_msg(client, "550 The file doesn't exist." FTPVITA_EOL);
		return;
	}

	ftpvita_format_mdtm_response(cmd, sizeof(cmd),
		stat.st_mtime.year,
		stat.st_mtime.month,
		stat.st_mtime.day,
		stat.st_mtime.hour,
		stat.st_mtime.minute,
		stat.st_mtime.second);
	client_send_ctrl_msg(client, cmd);
}

static void cmd_REST_func(ftpvita_client_info_t *client)
{
	char cmd[64];
	unsigned int restore_point;

	client->restore_point = 0;
	if (!ftpvita_parse_restart_offset(client->recv_cmd_args, &restore_point)) {
		client_send_ctrl_msg(client, "501 Invalid restart offset." FTPVITA_EOL);
		return;
	}

	client->restore_point = restore_point;
	snprintf(cmd, sizeof(cmd), "350 Resuming at %u" FTPVITA_EOL, client->restore_point);
	client_send_ctrl_msg(client, cmd);
}

static void cmd_FEAT_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client, "211-Extensions supported:" FTPVITA_EOL);
	client_send_ctrl_msg(client, " EPRT" FTPVITA_EOL);
	client_send_ctrl_msg(client, " EPSV" FTPVITA_EOL);
	client_send_ctrl_msg(client, " MLST type*;size*;modify*;" FTPVITA_EOL);
	client_send_ctrl_msg(client, " MDTM" FTPVITA_EOL);
	client_send_ctrl_msg(client, " REST STREAM" FTPVITA_EOL);
	client_send_ctrl_msg(client, " SIZE" FTPVITA_EOL);
	client_send_ctrl_msg(client, " UTF8" FTPVITA_EOL);
	client_send_ctrl_msg(client, "211 End" FTPVITA_EOL);
}

static void cmd_OPTS_func(ftpvita_client_info_t *client)
{
	const char *args = client->recv_cmd_args;
	size_t args_length;

	while (*args == ' ' || *args == '\t')
		args++;
	args_length = strlen(args);
	if (ftpvita_command_is(args, "UTF8") ||
		ftpvita_command_is(args, "UTF8 ON")) {
		client_send_ctrl_msg(client, "200 UTF8 enabled." FTPVITA_EOL);
	} else if (args_length >= 4 &&
		(args[0] == 'M' || args[0] == 'm') &&
		(args[1] == 'L' || args[1] == 'l') &&
		(args[2] == 'S' || args[2] == 's') &&
		(args[3] == 'T' || args[3] == 't') &&
		(args[4] == '\0' || args[4] == ' ' || args[4] == '\t')) {
		client_send_ctrl_msg(client,
			"200 MLST options accepted; type;size;modify." FTPVITA_EOL);
	} else {
		client_send_ctrl_msg(client, "501 Unsupported OPTS parameters." FTPVITA_EOL);
	}
}

static void cmd_APPE_func(ftpvita_client_info_t *client)
{
	char dest_path[PATH_MAX];
	client->restore_point = 0;
	if (!command_has_argument(client)) {
		client_send_ctrl_msg(client, "501 APPE requires a pathname." FTPVITA_EOL);
		return;
	}
	gen_ftp_fullpath(client, dest_path, sizeof(dest_path));
	receive_file(client, get_vita_path(dest_path), 1);
}

static void cmd_ABOR_func(ftpvita_client_info_t *client)
{
	if (client->data_con_type != FTP_DATA_CONNECTION_NONE) {
		client_close_data_connection(client);
		client_send_ctrl_msg(client, "226 Data connection closed." FTPVITA_EOL);
	} else {
		client_send_ctrl_msg(client,
			"225 No data connection is currently open." FTPVITA_EOL);
	}
}

static void cmd_ALLO_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client,
		"202 ALLO is unnecessary for this server." FTPVITA_EOL);
}

static void cmd_HELP_func(ftpvita_client_info_t *client)
{
	client_send_ctrl_msg(client,
		"214 Commands: USER PASS QUIT SYST NOOP FEAT OPTS HELP STAT "
		"PWD CWD CDUP TYPE MODE STRU PORT EPRT PASV EPSV ABOR "
		"LIST NLST MLST MLSD RETR STOR APPE REST SIZE MDTM "
		"DELE RMD MKD RNFR RNTO ALLO" FTPVITA_EOL);
}

static void cmd_STAT_func(ftpvita_client_info_t *client)
{
	char path[PATH_MAX];
	char response[128];
	SceIoStat stat;

	if (!command_has_argument(client)) {
		int clients;
		sceKernelLockMutex(client_list_mtx, 1, NULL);
		clients = number_clients;
		sceKernelUnlockMutex(client_list_mtx, 1);
		snprintf(response, sizeof(response),
			"211 VitaCompanion ready; %d client%s connected." FTPVITA_EOL,
			clients, clients == 1 ? "" : "s");
		client_send_ctrl_msg(client, response);
		return;
	}

	gen_ftp_fullpath(client, path, sizeof(path));
	if (sceIoGetstat(get_vita_path(path), &stat) < 0) {
		client_send_ctrl_msg(client, "550 Path not found." FTPVITA_EOL);
		return;
	}
	if (SCE_S_ISDIR(stat.st_mode)) {
		client_send_ctrl_msg(client, "212 Directory status is available." FTPVITA_EOL);
	} else {
		char size_string[21];
		if (!ftpvita_format_u64_decimal(size_string, sizeof(size_string),
			(unsigned long long)stat.st_size)) {
			client_send_ctrl_msg(client,
				"451 Could not format file size." FTPVITA_EOL);
			return;
		}
		snprintf(response, sizeof(response), "213 %s" FTPVITA_EOL,
			size_string);
		client_send_ctrl_msg(client, response);
	}
}

#define add_entry(name) {#name, cmd_##name##_func}
static const cmd_dispatch_entry cmd_dispatch_table[] = {
	add_entry(NOOP),
	add_entry(USER),
	add_entry(PASS),
	add_entry(QUIT),
	add_entry(SYST),
	add_entry(PASV),
	add_entry(EPSV),
	add_entry(PORT),
	add_entry(EPRT),
	add_entry(LIST),
	add_entry(NLST),
	add_entry(MLST),
	add_entry(MLSD),
	add_entry(PWD),
	add_entry(CWD),
	add_entry(TYPE),
	add_entry(MODE),
	add_entry(STRU),
	add_entry(CDUP),
	add_entry(RETR),
	add_entry(STOR),
	add_entry(DELE),
	add_entry(RMD),
	add_entry(MKD),
	add_entry(RNFR),
	add_entry(RNTO),
	add_entry(SIZE),
	add_entry(MDTM),
	add_entry(REST),
	add_entry(FEAT),
	add_entry(OPTS),
	add_entry(APPE),
	add_entry(ABOR),
	add_entry(ALLO),
	add_entry(HELP),
	add_entry(STAT),
	{NULL, NULL}
};

static cmd_dispatch_func get_dispatch_func(const char *cmd)
{
	int i;
	for(i = 0; cmd_dispatch_table[i].cmd && cmd_dispatch_table[i].func; i++) {
		if (strcmp(cmd, cmd_dispatch_table[i].cmd) == 0) {
			return cmd_dispatch_table[i].func;
		}
	}
	// Check for custom commands
	for(i = 0; i < MAX_CUSTOM_COMMANDS; i++) {
		if (custom_command_dispatchers[i].valid) {
			if (strcmp(cmd, custom_command_dispatchers[i].cmd) == 0) {
				return custom_command_dispatchers[i].func;
			}
		}
	}
	return NULL;
}

static int client_list_add(ftpvita_client_info_t *client)
{
	int added = 0;

	/* Add the client at the front of the client list */
	sceKernelLockMutex(client_list_mtx, 1, NULL);

	if (number_clients < MAX_CLIENTS) {
		if (client_list == NULL) { /* List is empty */
			client_list = client;
			client->prev = NULL;
			client->next = NULL;
		} else {
			client->next = client_list;
			client_list->prev = client;
			client->prev = NULL;
			client_list = client;
		}
		client->restore_point = 0;
		client->listed = 1;
		client->cleanup_by_server = 0;
		number_clients++;
		added = 1;
	}

	sceKernelUnlockMutex(client_list_mtx, 1);
	return added;
}

static int client_list_delete(ftpvita_client_info_t *client)
{
	int client_owns_cleanup = 1;

	/* Remove the client from the client list */
	sceKernelLockMutex(client_list_mtx, 1, NULL);

	if (client->cleanup_by_server) {
		client_owns_cleanup = 0;
	} else if (client->listed) {
		if (client->prev) {
			client->prev->next = client->next;
		}
		if (client->next) {
			client->next->prev = client->prev;
		}
		if (client == client_list) {
			client_list = client->next;
		}

		client->listed = 0;
		number_clients--;
	}

	sceKernelUnlockMutex(client_list_mtx, 1);
	return client_owns_cleanup;
}

static void client_list_thread_end()
{
	ftpvita_client_info_t *clients, *it, *next;
	const int socket_abort_flags = SCE_NET_SOCKET_ABORT_FLAG_RCV_PRESERVATION |
				SCE_NET_SOCKET_ABORT_FLAG_SND_PRESERVATION;

	sceKernelLockMutex(client_list_mtx, 1, NULL);

	clients = client_list;
	client_list = NULL;
	number_clients = 0;
	for (it = clients; it; it = it->next) {
		it->listed = 0;
		it->cleanup_by_server = 1;
	}

	sceKernelUnlockMutex(client_list_mtx, 1);

	/* Abort every client before waiting so one stuck worker cannot delay
	 * cancellation of all of the others. */
	for (it = clients; it; it = it->next) {
		sceNetSocketAbort(it->ctrl_sockfd, socket_abort_flags);

		/* If there's an open data connection, abort it */
		if (it->data_con_type != FTP_DATA_CONNECTION_NONE) {
			sceNetSocketAbort(it->data_sockfd, socket_abort_flags);
			if (it->data_con_type == FTP_DATA_CONNECTION_PASSIVE) {
				sceNetSocketAbort(it->pasv_sockfd, socket_abort_flags);
			}
		}
	}

	/* Wait for each client thread and release its server-owned state. */
	for (it = clients; it; it = next) {
		next = it->next;
		/* Wait until the client threads ends */
		sceKernelWaitThreadEnd(it->thid, NULL, NULL);
		free(it);
	}
}

static int client_handle_command_line(ftpvita_client_info_t *client,
	char *line)
{
	char cmd[16];
	cmd_dispatch_func dispatch_func;
	char *cursor = line;
	char *args;
	size_t cmd_length = 0;

	while (*cursor == ' ' || *cursor == '\t')
		cursor++;
	while (cursor[cmd_length] && cursor[cmd_length] != ' ' &&
		cursor[cmd_length] != '\t') {
		if (cmd_length + 1 >= sizeof(cmd)) {
			client_send_ctrl_msg(client, "500 Command name is too long." FTPVITA_EOL);
			return 0;
		}
		cmd[cmd_length] = cursor[cmd_length] >= 'a' &&
			cursor[cmd_length] <= 'z' ?
			(char)(cursor[cmd_length] - ('a' - 'A')) :
			cursor[cmd_length];
		cmd_length++;
	}
	cmd[cmd_length] = '\0';

	if (cmd_length == 0) {
		client_send_ctrl_msg(client, "500 Empty command." FTPVITA_EOL);
		return 0;
	}

	args = cursor + cmd_length;
	while (*args == ' ' || *args == '\t')
		args++;
	client->recv_cmd_args = args;

	INFO("\t%i> %s\n", client->num, line);
	sceKernelDelayThread(1 * 1000);

	dispatch_func = get_dispatch_func(cmd);
	if (!dispatch_func) {
		client_send_ctrl_msg(client,
			"502 Sorry, command not implemented. :(" FTPVITA_EOL);
		return 0;
	}

	dispatch_func(client);
	return dispatch_func == cmd_QUIT_func;
}

static int client_consume_received_data(ftpvita_client_info_t *client,
	const unsigned char *data, size_t data_size)
{
	size_t i;

	for (i = 0; i < data_size; i++) {
		unsigned char value = data[i];

		if (client->recv_buffer_discarding) {
			if (value == '\n')
				client->recv_buffer_discarding = 0;
			continue;
		}

		if (value == '\0') {
			client->recv_buffer_used = 0;
			client->recv_buffer_discarding = 1;
			client_send_ctrl_msg(client,
				"500 NUL is not valid in an FTP command." FTPVITA_EOL);
			continue;
		}

		if (value == '\n') {
			if (client->recv_buffer_used > 0 &&
				client->recv_buffer[client->recv_buffer_used - 1] == '\r')
				client->recv_buffer_used--;
			client->recv_buffer[client->recv_buffer_used] = '\0';
			if (client_handle_command_line(client, client->recv_buffer))
				return 1;
			client->recv_buffer_used = 0;
			if (client->ctrl_send_failed)
				return 1;
			continue;
		}

		if (client->recv_buffer_used + 1 >= sizeof(client->recv_buffer)) {
			client->recv_buffer_used = 0;
			client->recv_buffer_discarding = 1;
			client_send_ctrl_msg(client, "500 Command line is too long." FTPVITA_EOL);
			continue;
		}

		client->recv_buffer[client->recv_buffer_used++] = (char)value;
	}

	return 0;
}

static int client_thread(SceSize args, void *argp)
{
	unsigned char recv_chunk[512];
	ftpvita_client_info_t *client = *(ftpvita_client_info_t **)argp;
	int client_owns_cleanup;
	int quit = 0;

	UNUSED(args);
	DEBUG("Client thread %i started!\n", client->num);

	if (client_send_ctrl_msg(client, "220 VitaCompanion Server ready." FTPVITA_EOL) < 0)
		goto cleanup;

	while (1) {
		client->n_recv = sceNetRecv(client->ctrl_sockfd, recv_chunk,
			sizeof(recv_chunk), 0);
		if (client->n_recv > 0) {
			DEBUG("Received %i bytes from client number %i:\n",
				client->n_recv, client->num);
			quit = client_consume_received_data(client, recv_chunk,
				(size_t)client->n_recv);
			if (quit)
				break;
		} else if (client->n_recv == 0) {
			/* Value 0 means connection closed by the remote peer */
			INFO("Connection closed by the client %i.\n", client->num);
			break;
		} else if (client->n_recv == (int)SCE_NET_ERROR_EINTR) {
			/* Socket aborted (ftpvita_fini() called) */
			INFO("Client %i socket aborted.\n", client->num);
			break;
		} else {
			/* Other errors */
			INFO("Client %i socket error: 0x%08X\n", client->num, client->n_recv);
			break;
		}
	}

cleanup:
	client_owns_cleanup = client_list_delete(client);

	/* Close the client's socket */
	sceNetSocketClose(client->ctrl_sockfd);

	/* If there's an open data connection, close it */
	if (client->data_con_type != FTP_DATA_CONNECTION_NONE) {
		sceNetSocketClose(client->data_sockfd);
		if (client->data_con_type == FTP_DATA_CONNECTION_PASSIVE) {
			sceNetSocketClose(client->pasv_sockfd);
		}
	}

	DEBUG("Client thread %i exiting!\n", client->num);

	if (client_owns_cleanup)
		free(client);

	sceKernelExitDeleteThread(0);
	return 0;
}

static int server_thread(SceSize args, void *argp)
{
	int ret;
	UNUSED(ret);
	UNUSED(args);
	UNUSED(argp);

	SceNetSockaddrIn serveraddr;

	DEBUG("Server thread started!\n");
	memset(&serveraddr, 0, sizeof(serveraddr));

	/* Create server socket */
	server_sockfd = sceNetSocket("FTPVita_server_sock",
		SCE_NET_AF_INET,
		SCE_NET_SOCK_STREAM,
		0);

	DEBUG("Server socket fd: %d\n", server_sockfd);
	if (server_sockfd < 0)
		goto exit;

	/* Fill the server's address */
	serveraddr.sin_family = SCE_NET_AF_INET;
	serveraddr.sin_addr.s_addr = sceNetHtonl(SCE_NET_INADDR_ANY);
	serveraddr.sin_port = sceNetHtons(FTP_PORT);

	/* Bind the server's address to the socket */
	ret = sceNetBind(server_sockfd, (SceNetSockaddr *)&serveraddr, sizeof(serveraddr));
	DEBUG("sceNetBind(): 0x%08X\n", ret);
	if (ret < 0)
		goto close_and_exit;

	/* Start listening */
	ret = sceNetListen(server_sockfd, 128);
	DEBUG("sceNetListen(): 0x%08X\n", ret);
	if (ret < 0)
		goto close_and_exit;
	server_start_state = 1;

	while (1) {
		/* Accept clients */
		SceNetSockaddrIn clientaddr = {0};
		int client_sockfd;
		unsigned int addrlen = sizeof(clientaddr);

		DEBUG("Waiting for incoming connections...\n");

		client_sockfd = sceNetAccept(server_sockfd, (SceNetSockaddr *)&clientaddr, &addrlen);
		if (client_sockfd >= 0) {
			DEBUG("New connection, client fd: 0x%08X\n", client_sockfd);
			socket_set_send_timeout(client_sockfd,
				CONTROL_SOCKET_SEND_TIMEOUT_US);

			/* Get the client's IP address */
			char remote_ip[16];
			sceNetInetNtop(SCE_NET_AF_INET,
				&clientaddr.sin_addr.s_addr,
				remote_ip,
				sizeof(remote_ip));

			unsigned int client_id = next_client_id++;
			INFO("Client %i connected, IP: %s port: %i\n",
				client_id, remote_ip, clientaddr.sin_port);

			/* Create a new thread for the client */
			char client_thread_name[64];
			snprintf(client_thread_name, sizeof(client_thread_name),
				"FTPVita_client_%u_thread",
				client_id);

			SceUID client_thid = sceKernelCreateThread(
				client_thread_name, client_thread,
				0x10000100, 0x10000, 0, 0, NULL);

			DEBUG("Client %i thread UID: 0x%08X\n", client_id, client_thid);
			if (client_thid < 0) {
				sceNetSocketClose(client_sockfd);
				continue;
			}

			/* Allocate the ftpvita_client_info_t struct for the new client */
			ftpvita_client_info_t *client = malloc(sizeof(*client));
			if (client == NULL) {
				sceKernelDeleteThread(client_thid);
				sceNetSocketClose(client_sockfd);
				continue;
			}
			client->num = (int)client_id;
			client->thid = client_thid;
			client->ctrl_sockfd = client_sockfd;
			client->data_sockfd = -1;
			client->data_con_type = FTP_DATA_CONNECTION_NONE;
			client->data_sockaddr = (SceNetSockaddrIn){0};
			client->pasv_sockaddr = (SceNetSockaddrIn){0};
			client->pasv_sockfd = -1;
			client->n_recv = 0;
			client->recv_buffer[0] = '\0';
			client->recv_buffer_used = 0;
			client->recv_buffer_discarding = 0;
			client->recv_cmd_args = "";
			client->ctrl_send_failed = 0;
			strcpy(client->cur_path, FTP_DEFAULT_PATH);
			client->rename_path[0] = '\0';
			client->restore_point = 0;
			client->transfer_type = FTP_TRANSFER_TYPE_ASCII;
			client->epsv_all = 0;
			client->listed = 0;
			client->cleanup_by_server = 0;
			client->next = NULL;
			client->prev = NULL;
			memcpy(&client->addr, &clientaddr, sizeof(client->addr));

			/* Add the new client to the client list */
			if (!client_list_add(client)) {
				ftpvita_send_all(sceNetSend, client_sockfd,
					"421 Too many FTP clients." FTPVITA_EOL,
					(unsigned int)strlen(
					"421 Too many FTP clients." FTPVITA_EOL), 0);
				sceKernelDeleteThread(client_thid);
				sceNetSocketClose(client_sockfd);
				free(client);
				continue;
			}

			/* Start the client thread */
			ret = sceKernelStartThread(client_thid, sizeof(client), &client);
			if (ret < 0) {
				client_list_delete(client);
				sceKernelDeleteThread(client_thid);
				sceNetSocketClose(client_sockfd);
				free(client);
			}
		} else {
			if (server_stopping) {
				DEBUG("Server socket closed, 0x%08X\n", client_sockfd);
				break;
			}

			/* Resource pressure and interrupted calls can fail accept
			 * transiently. Keep serving once the condition clears. */
			INFO("Server accept error: 0x%08X; retrying.\n", client_sockfd);
			sceKernelDelayThread(100 * 1000);
		}
	}

	goto exit;

close_and_exit:
	sceNetSocketClose(server_sockfd);
	server_sockfd = -1;

exit:
	if (server_start_state == 0)
		server_start_state = -1;
	DEBUG("Server thread exiting!\n");

	sceKernelExitDeleteThread(0);
	return 0;
}

static int ftpvita_init_internal(char *vita_ip, unsigned short int *vita_port,
	int existing_network)
{
	int ret;
	int i;
	int waited_ms;
	SceNetInitParam initparam;
	SceNetCtlInfo info;

	if (ftp_initialized || !vita_ip || !vita_port) {
		return -1;
	}

	if (existing_network)
		goto get_ip_address;

	/* Init Net */
	ret = sceNetShowNetstat();
	if (ret == 0) {
		DEBUG("Net is already initialized.\n");
		net_init = -1;
	} else if (ret == (int)SCE_NET_ERROR_ENOTINIT) {
		net_memory = malloc(NET_INIT_SIZE);
		if (!net_memory) {
			ret = -1;
			goto error_netinit;
		}

		initparam.memory = net_memory;
		initparam.size = NET_INIT_SIZE;
		initparam.flags = 0;

		ret = net_init = sceNetInit(&initparam);
		DEBUG("sceNetInit(): 0x%08X\n", net_init);
		if (net_init < 0)
			goto error_netinit;
	} else {
		INFO("Net error: 0x%08X\n", ret);
		goto error_netstat;
	}

	/* Init NetCtl */
	ret = netctl_init = sceNetCtlInit();
	DEBUG("sceNetCtlInit(): 0x%08X\n", netctl_init);
	if (netctl_init < 0 && netctl_init != NET_CTL_ERROR_NOT_TERMINATED)
		goto error_netctlinit;

get_ip_address:
	/* Get IP address */
	ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
	DEBUG("sceNetCtlInetGetInfo(): 0x%08X\n", ret);
	if (ret < 0)
		goto error_netctlgetinfo;

	/* Return data */
	strcpy(vita_ip, info.ip_address);
	*vita_port = FTP_PORT;

	/* Save the IP of PSVita to a global variable */
	ret = sceNetInetPton(SCE_NET_AF_INET, info.ip_address, &vita_addr);
	if (ret != 1) {
		ret = -1;
		goto error_netctlgetinfo;
	}

	ret = ftpvita_transfer_pool_init(file_buf_size);
	if (ret < 0)
		goto error_transfer_pool;

	/* Create the client list mutex */
	client_list_mtx = sceKernelCreateMutex("FTPVita_client_list_mutex", 0, 0, NULL);
	DEBUG("Client list mutex UID: 0x%08X\n", client_list_mtx);
	if (client_list_mtx < 0) {
		ret = client_list_mtx;
		goto error_server_resources;
	}

	/* Create server thread */
	server_thid = sceKernelCreateThread("FTPVita_server_thread",
		server_thread, 0x10000100, 0x10000, 0, 0, NULL);
	DEBUG("Server thread UID: 0x%08X\n", server_thid);
	if (server_thid < 0) {
		ret = server_thid;
		goto error_server_resources;
	}

	/* Init device list */
	for (i = 0; i < MAX_DEVICES; i++) {
		device_list[i].valid = 0;
	}

	for (i = 0; i < MAX_CUSTOM_COMMANDS; i++) {
		custom_command_dispatchers[i].valid = 0;
	}

	/* Start the server thread */
	server_stopping = 0;
	server_start_state = 0;
	server_sockfd = -1;
	ret = sceKernelStartThread(server_thid, 0, NULL);
	if (ret < 0) {
		sceKernelDeleteThread(server_thid);
		server_thid = -1;
		goto error_server_resources;
	}

	for (waited_ms = 0;
		waited_ms < SERVER_START_TIMEOUT_MS && server_start_state == 0;
		waited_ms += 10)
		sceKernelDelayThread(10 * 1000);

	if (server_start_state != 1) {
		ret = server_start_state < 0 ? server_start_state : -1;
		server_stopping = 1;
		if (server_sockfd >= 0)
			sceNetSocketClose(server_sockfd);
		sceKernelWaitThreadEnd(server_thid, NULL, NULL);
		server_thid = -1;
		server_sockfd = -1;
		goto error_server_resources;
	}

	ftp_initialized = 1;

	return 0;

error_server_resources:
	if (client_list_mtx >= 0) {
		sceKernelDeleteMutex(client_list_mtx);
		client_list_mtx = -1;
	}
	ftpvita_transfer_pool_fini();
error_transfer_pool:
error_netctlgetinfo:
	if (netctl_init == 0) {
		sceNetCtlTerm();
		netctl_init = -1;
	}
error_netctlinit:
	if (net_init == 0) {
		sceNetTerm();
		net_init = -1;
	}
error_netinit:
	if (net_memory) {
		free(net_memory);
		net_memory = NULL;
	}
error_netstat:
	return ret;
}

int ftpvita_init(char *vita_ip, unsigned short int *vita_port)
{
	return ftpvita_init_internal(vita_ip, vita_port, 0);
}

int ftpvita_init_existing_network(char *vita_ip, unsigned short int *vita_port)
{
	return ftpvita_init_internal(vita_ip, vita_port, 1);
}

void ftpvita_fini()
{
	if (ftp_initialized) {
		ftp_initialized = 0;
		/* In order to "stop" the blocking sceNetAccept,
		 * we have to close the server socket; this way
		 * the accept call will return an error */
		server_stopping = 1;
		if (server_sockfd >= 0)
			sceNetSocketClose(server_sockfd);

		/* Wait until the server threads ends */
		if (server_thid >= 0)
			sceKernelWaitThreadEnd(server_thid, NULL, NULL);
		server_thid = -1;
		server_sockfd = -1;

		/* To close the clients we have to do the same:
		 * we have to iterate over all the clients
		 * and shutdown their sockets */
		if (client_list_mtx >= 0)
			client_list_thread_end();

		/* Delete the client list mutex */
		if (client_list_mtx >= 0)
			sceKernelDeleteMutex(client_list_mtx);
		client_list_mtx = -1;

		client_list = NULL;
		number_clients = 0;
		ftpvita_transfer_pool_fini();

		if (netctl_init == 0)
			sceNetCtlTerm();
		if (net_init == 0)
			sceNetTerm();
		if (net_memory)
			free(net_memory);

		netctl_init = -1;
		net_init = -1;
		net_memory = NULL;
		server_start_state = 0;
	}
}

int ftpvita_is_initialized()
{
	return ftp_initialized;
}

int ftpvita_add_device(const char *devname)
{
	int i;
	if (!devname)
		return 0;
	for (i = 0; i < MAX_DEVICES; i++) {
		if (!device_list[i].valid) {
			snprintf(device_list[i].name, sizeof(device_list[i].name),
				"%s", devname);
			device_list[i].valid = 1;
			return 1;
		}
	}
	return 0;
}

int ftpvita_del_device(const char *devname)
{
	int i;
	if (!devname)
		return 0;
	for (i = 0; i < MAX_DEVICES; i++) {
		if (device_list[i].valid &&
			strcmp(devname, device_list[i].name) == 0) {
			device_list[i].valid = 0;
			return 1;
		}
	}
	return 0;
}

void ftpvita_set_info_log_cb(ftpvita_log_cb_t cb)
{
#if FTPVITA_ENABLE_LOGGING == 1
	info_log_cb = cb;
#else
	(void)cb;
#endif
}

void ftpvita_set_debug_log_cb(ftpvita_log_cb_t cb)
{
#if FTPVITA_ENABLE_LOGGING == 1
	debug_log_cb = cb;
#else
	(void)cb;
#endif
}

void ftpvita_set_file_buf_size(unsigned int size)
{
	if (size > 0 && !ftp_initialized)
		file_buf_size = size;
}

int ftpvita_ext_add_custom_command(const char *cmd, cmd_dispatch_func func)
{
	int i;
	if (!cmd || !func)
		return 0;
	for (i = 0; i < MAX_CUSTOM_COMMANDS; i++) {
		if (!custom_command_dispatchers[i].valid) {
			custom_command_dispatchers[i].cmd = cmd;
			custom_command_dispatchers[i].func = func;
			custom_command_dispatchers[i].valid = 1;
			return 1;
		}
	}
	return 0;
}

int ftpvita_ext_del_custom_command(const char *cmd)
{
	int i;
	if (!cmd)
		return 0;
	for (i = 0; i < MAX_CUSTOM_COMMANDS; i++) {
		if (custom_command_dispatchers[i].valid &&
			custom_command_dispatchers[i].cmd &&
			strcmp(cmd, custom_command_dispatchers[i].cmd) == 0) {
			custom_command_dispatchers[i].valid = 0;
			return 1;
		}
	}
	return 0;
}

void ftpvita_ext_client_send_ctrl_msg(ftpvita_client_info_t *client, const char *msg)
{
	if (client && msg)
		client_send_ctrl_msg(client, msg);
}

void ftpvita_ext_client_send_data_msg(ftpvita_client_info_t *client, const char *str)
{
	if (client && str)
		client_send_data_msg(client, str);
}
