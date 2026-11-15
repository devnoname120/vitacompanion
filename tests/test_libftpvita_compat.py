import pathlib
import re
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor" / "libftpvita"


def compile_and_run(
    source,
    support_sources=("ftpvita_path.c",),
    source_root=VENDOR,
    include_root=VENDOR,
):
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)
        source_path = tmp / "test.c"
        exe_path = tmp / "test"
        source_path.write_text(textwrap.dedent(source))
        subprocess.run(
            [
                "cc",
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(include_root),
                str(source_path),
                *(str(source_root / name) for name in support_sources),
                "-o",
                str(exe_path),
            ],
            check=True,
            cwd=ROOT,
        )
        subprocess.run([str(exe_path)], check=True, cwd=ROOT)


class LibftpvitaCompatTests(unittest.TestCase):
    def test_command_server_accepts_lf_and_windows_crlf(self):
        compile_and_run(
            r"""
            #include "parser.h"

            #include <stdlib.h>
            #include <string.h>

            static void expect_command(char *input, size_t input_size,
                size_t expected_count, const char *first, const char *second)
            {
                char *args[4] = {0};
                size_t count = parse_cmd(input, input_size, args, 4);

                if (count != expected_count || strcmp(args[0], first) != 0)
                    exit(1);
                if (second && strcmp(args[1], second) != 0)
                    exit(2);
            }

            int main(void)
            {
                char lf[] = "reboot\n";
                char crlf[] = "launch TEST00001\r\n";

                expect_command(lf, sizeof(lf) - 1, 1, "reboot", NULL);
                expect_command(crlf, sizeof(crlf) - 1, 2,
                    "launch", "TEST00001");
                return 0;
            }
            """,
            support_sources=("parser.c",),
            source_root=ROOT / "src",
            include_root=ROOT / "src",
        )

    def test_vita_paths_are_normalized_for_file_commands(self):
        compile_and_run(
            r"""
            #include "ftpvita_path.h"

            #include <stdio.h>
            #include <stdlib.h>
            #include <string.h>

            static void expect_path(const char *cur_path, const char *arg, const char *expected)
            {
                char actual[256];
                ftpvita_path_from_command_arg(cur_path, arg, actual, sizeof(actual));
                if (strcmp(actual, expected) != 0) {
                    fprintf(stderr, "for arg %s expected %s, got %s\n", arg, expected, actual);
                    exit(1);
                }
            }

            int main(void)
            {
                expect_path("/", "/ux0:/data/bootstrap.self\r\n", "/ux0:/data/bootstrap.self");
                expect_path("/", "ux0:/data/bootstrap.self\r\n", "/ux0:/data/bootstrap.self");
                expect_path("/", "uma0:/media/file.bin\r\n", "/uma0:/media/file.bin");
                expect_path("/ux0:/data", "bootstrap.self\r\n", "/ux0:/data/bootstrap.self");
                expect_path("/ux0:/", "data/bootstrap.self\r\n", "/ux0:/data/bootstrap.self");
                return 0;
            }
            """
        )

    def test_list_paths_are_normalized_and_list_options_are_ignored(self):
        compile_and_run(
            r"""
            #include "ftpvita_path.h"

            #include <stdio.h>
            #include <stdlib.h>
            #include <string.h>

            static void expect_list_path(const char *cur_path, const char *arg, const char *expected)
            {
                char actual[256];
                ftpvita_path_from_list_args(cur_path, arg, actual, sizeof(actual));
                if (strcmp(actual, expected) != 0) {
                    fprintf(stderr, "for list arg %s expected %s, got %s\n", arg, expected, actual);
                    exit(1);
                }
            }

            int main(void)
            {
                expect_list_path("/ux0:/data", "", "/ux0:/data");
                expect_list_path("/", "ux0:/data/\r\n", "/ux0:/data/");
                expect_list_path("/", "/ux0:/data/\r\n", "/ux0:/data/");
                expect_list_path("/", "-la ux0:/data/\r\n", "/ux0:/data/");
                expect_list_path("/ux0:/data", "-a\r\n", "/ux0:/data");
                expect_list_path("/ux0:/", "data/\r\n", "/ux0:/data/");
                return 0;
            }
            """
        )

    def test_epsv_response_uses_extended_passive_mode_format(self):
        compile_and_run(
            r"""
            #include "ftpvita_path.h"

            #include <stdio.h>
            #include <stdlib.h>
            #include <string.h>

            int main(void)
            {
                char actual[128];
                ftpvita_format_epsv_response(actual, sizeof(actual), 65000);
                if (strcmp(actual, "229 Entering Extended Passive Mode (|||65000|)\r\n") != 0) {
                    fprintf(stderr, "unexpected EPSV reply: %s\n", actual);
                    exit(1);
                }
                return 0;
            }
            """
        )

    def test_mdtm_response_uses_ftp_timestamp_format(self):
        compile_and_run(
            r"""
            #include "ftpvita_path.h"

            #include <stdio.h>
            #include <stdlib.h>
            #include <string.h>

            int main(void)
            {
                char actual[128];
                ftpvita_format_mdtm_response(actual, sizeof(actual), 2026, 7, 4, 9, 5, 3);
                if (strcmp(actual, "213 20260704090503\r\n") != 0) {
                    fprintf(stderr, "unexpected MDTM reply: %s\n", actual);
                    exit(1);
                }
                return 0;
            }
            """
        )

    def test_restart_offset_parser_accepts_valid_offsets_and_rejects_invalid_input(self):
        compile_and_run(
            r"""
            #include "ftpvita_path.h"

            #include <limits.h>
            #include <stdio.h>
            #include <stdlib.h>

            static void expect_valid(const char *input, unsigned int expected)
            {
                unsigned int actual = 123;
                if (!ftpvita_parse_restart_offset(input, &actual) || actual != expected) {
                    fprintf(stderr, "expected valid offset %u for %s, got %u\n",
                        expected, input, actual);
                    exit(1);
                }
            }

            static void expect_invalid(const char *input)
            {
                unsigned int actual = 123;
                if (ftpvita_parse_restart_offset(input, &actual)) {
                    fprintf(stderr, "expected invalid offset for %s\n", input);
                    exit(1);
                }
            }

            int main(void)
            {
                expect_valid("0\r\n", 0);
                expect_valid("  42 \r\n", 42);
                expect_valid("2147483647\r\n", INT_MAX);
                expect_invalid("");
                expect_invalid("-1\r\n");
                expect_invalid("12x\r\n");
                expect_invalid("2147483648\r\n");
                return 0;
            }
            """
        )

    def test_active_mode_parsers_accept_valid_port_and_ipv4_eprt(self):
        compile_and_run(
            r"""
            #include "ftpvita_protocol.h"

            #include <stdio.h>
            #include <stdlib.h>
            #include <string.h>

            int main(void)
            {
                unsigned char ip[4];
                unsigned short port;
                unsigned int protocol;
                char address[64];

                if (!ftpvita_parse_port("192,168,1,44,195,80", ip, &port))
                    return 1;
                if (ip[0] != 192 || ip[1] != 168 || ip[2] != 1 ||
                    ip[3] != 44 || port != 50000)
                    return 2;
                if (!ftpvita_parse_eprt("|1|10.0.0.7|49152|", &protocol,
                    address, sizeof(address), &port))
                    return 3;
                if (protocol != 1 || strcmp(address, "10.0.0.7") != 0 ||
                    port != 49152)
                    return 4;

                if (ftpvita_parse_port("192,168,1,300,1,1", ip, &port))
                    return 5;
                if (ftpvita_parse_port("192,168,1,2,0,0", ip, &port))
                    return 6;
                if (ftpvita_parse_port("192,168,1,2,1", ip, &port))
                    return 7;
                if (ftpvita_parse_port("192,168,1,2,1,2,3", ip, &port))
                    return 8;
                if (ftpvita_parse_eprt("|1|10.0.0.7|0|", &protocol,
                    address, sizeof(address), &port))
                    return 9;
                if (ftpvita_parse_eprt("|1|10.0.0.7|70000|", &protocol,
                    address, sizeof(address), &port))
                    return 10;
                return 0;
            }
            """,
            support_sources=("ftpvita_protocol.c",),
        )

    def test_epsv_and_type_parameter_parsers_are_bounded(self):
        compile_and_run(
            r"""
            #include "ftpvita_protocol.h"

            #include <stdlib.h>

            int main(void)
            {
                ftpvita_epsv_request_t request;
                unsigned int unsupported = 0;
                char type;

                if (!ftpvita_parse_epsv("", &request, &unsupported) ||
                    request != FTPVITA_EPSV_DEFAULT)
                    return 1;
                if (!ftpvita_parse_epsv("1", &request, &unsupported) ||
                    request != FTPVITA_EPSV_IPV4)
                    return 2;
                if (!ftpvita_parse_epsv("all", &request, &unsupported) ||
                    request != FTPVITA_EPSV_ALL)
                    return 3;
                if (ftpvita_parse_epsv("2", &request, &unsupported) != -1 ||
                    unsupported != 2)
                    return 4;

                if (!ftpvita_parse_type("a n", &type) || type != 'A')
                    return 5;
                if (!ftpvita_parse_type("i", &type) || type != 'I')
                    return 6;
                if (ftpvita_parse_type("A THIS_ARGUMENT_IS_TOO_LONG", &type))
                    return 7;
                if (ftpvita_parse_type("L 8", &type))
                    return 8;
                return 0;
            }
            """,
            support_sources=("ftpvita_protocol.c",),
        )

    def test_u64_size_formatting_avoids_variadic_hardfp_mismatch(self):
        compile_and_run(
            r"""
            #include "ftpvita_protocol.h"

            #include <limits.h>
            #include <stdlib.h>
            #include <string.h>

            static void expect_value(unsigned long long value,
                const char *expected)
            {
                char actual[21];
                if (!ftpvita_format_u64_decimal(actual, sizeof(actual), value))
                    exit(1);
                if (strcmp(actual, expected) != 0)
                    exit(2);
            }

            int main(void)
            {
                char too_small[3];

                expect_value(0, "0");
                expect_value(1182, "1182");
                expect_value(4294967296ULL, "4294967296");
                expect_value(ULLONG_MAX, "18446744073709551615");
                if (ftpvita_format_u64_decimal(too_small,
                    sizeof(too_small), 100))
                    return 3;
                if (too_small[0] != '\0')
                    return 4;
                return 0;
            }
            """,
            support_sources=("ftpvita_protocol.c",),
        )

    def test_ascii_transfer_conversion_handles_split_crlf_sequences(self):
        compile_and_run(
            r"""
            #include "ftpvita_protocol.h"

            #include <stdlib.h>
            #include <string.h>

            int main(void)
            {
                static const unsigned char local[] = "a\nb\nc\rd";
                static const unsigned char wire[] = "a\r\nb\r\nc\r\0d";
                unsigned char encoded[64];
                unsigned char decoded[64];
                ftpvita_ascii_state_t state;
                size_t used = 0;
                size_t amount;

                ftpvita_ascii_state_init(&state);
                amount = ftpvita_ascii_encode(&state, local, 4,
                    encoded + used, sizeof(encoded) - used);
                if (amount == (size_t)-1)
                    return 1;
                used += amount;
                amount = ftpvita_ascii_encode(&state, local + 4,
                    sizeof(local) - 1 - 4, encoded + used,
                    sizeof(encoded) - used);
                if (amount == (size_t)-1)
                    return 2;
                used += amount;
                amount = ftpvita_ascii_finish_encode(&state, encoded + used,
                    sizeof(encoded) - used);
                if (amount == (size_t)-1)
                    return 3;
                used += amount;
                if (used != sizeof(wire) - 1 ||
                    memcmp(encoded, wire, used) != 0)
                    return 4;

                ftpvita_ascii_state_init(&state);
                used = ftpvita_ascii_decode(&state, wire, 2,
                    decoded, sizeof(decoded));
                if (used == (size_t)-1)
                    return 5;
                amount = ftpvita_ascii_decode(&state, wire + 2,
                    sizeof(wire) - 1 - 2, decoded + used,
                    sizeof(decoded) - used);
                if (amount == (size_t)-1)
                    return 6;
                used += amount;
                amount = ftpvita_ascii_finish_decode(&state, decoded + used,
                    sizeof(decoded) - used);
                if (amount == (size_t)-1)
                    return 7;
                used += amount;
                if (used != sizeof(local) - 1 ||
                    memcmp(decoded, local, used) != 0)
                    return 8;
                return 0;
            }
            """,
            support_sources=("ftpvita_protocol.c",),
        )

    def test_ftp_command_table_wires_compatibility_commands(self):
        source = (VENDOR / "ftpvita.c").read_text()
        self.assertIn("ftpvita_path_from_list_args", source)
        self.assertIn("add_entry(EPSV)", source)
        self.assertIn("add_entry(EPRT)", source)
        self.assertIn("add_entry(NLST)", source)
        self.assertIn("add_entry(MLST)", source)
        self.assertIn("add_entry(MLSD)", source)
        self.assertIn("add_entry(MDTM)", source)

    def test_commands_without_argument_separator_have_empty_args(self):
        source = (VENDOR / "ftpvita.c").read_text()
        self.assertIn('client->recv_cmd_args = "";', source)
        self.assertNotIn("client->recv_cmd_args = client->recv_buffer;", source)

    def test_send_all_handles_partial_writes_and_disconnects(self):
        compile_and_run(
            r"""
            #include "ftpvita_io.h"

            #include <stdlib.h>
            #include <string.h>

            static const char *expected;
            static unsigned int offset;
            static int fail_after;
            static int zero_after;

            static int mock_send(int socket, const void *buffer, unsigned int length, int flags)
            {
                unsigned int amount;
                (void)socket;
                (void)flags;

                if (fail_after >= 0 && (int)offset >= fail_after)
                    return -123;
                if (zero_after >= 0 && (int)offset >= zero_after)
                    return 0;

                amount = length > 2 ? 2 : length;
                if (memcmp(buffer, expected + offset, amount) != 0)
                    exit(10);
                offset += amount;
                return (int)amount;
            }

            int main(void)
            {
                expected = "abcdef";
                offset = 0;
                fail_after = -1;
                zero_after = -1;
                if (ftpvita_send_all(mock_send, 1, expected, 6, 0) != 6 || offset != 6)
                    return 1;

                offset = 0;
                fail_after = 2;
                if (ftpvita_send_all(mock_send, 1, expected, 6, 0) != -123 || offset != 2)
                    return 2;

                offset = 0;
                fail_after = -1;
                zero_after = 2;
                if (ftpvita_send_all(mock_send, 1, expected, 6, 0) >= 0 || offset != 2)
                    return 3;

                return 0;
            }
            """,
            support_sources=("ftpvita_io.c",),
        )

    def test_disconnect_paths_are_bounded_and_shutdown_does_not_wait_under_mutex(self):
        source = (VENDOR / "ftpvita.c").read_text()
        self.assertIn("SCE_NET_SO_SNDTIMEO", source)
        self.assertIn("SCE_NET_SO_RCVTIMEO", source)
        self.assertIn("SCE_NET_SO_LINGER", source)
        self.assertIn("client_send_data_msg(client, buffer) < 0", source)
        self.assertIn("client_consume_received_data", source)
        self.assertIn("recv_buffer_discarding", source)
        self.assertIn("Command line is too long.", source)
        self.assertIn("MAX_CLIENTS", source)
        self.assertIn("if (server_stopping)", source)
        self.assertIn("Server accept error:", source)

        shutdown_start = source.index("static void client_list_thread_end()")
        shutdown_end = source.index("static int client_thread", shutdown_start)
        shutdown = source[shutdown_start:shutdown_end]
        self.assertLess(
            shutdown.index("sceKernelUnlockMutex(client_list_mtx, 1)"),
            shutdown.index("sceKernelWaitThreadEnd(it->thid"),
        )

    def test_active_mode_replaces_prior_data_connection_and_remains_unrestricted(self):
        source = (VENDOR / "ftpvita.c").read_text()
        active_start = source.index("static int client_prepare_active_data_connection")
        active_end = source.index("static void cmd_PORT_func", active_start)
        active = source[active_start:active_end]
        self.assertIn("client_close_data_connection(client)", active)
        self.assertIn("client->data_sockaddr.sin_addr = *data_addr;", active)
        self.assertNotIn("client->addr", active)

        port_start = source.index("static void cmd_PORT_func")
        port_end = source.index("static void cmd_EPRT_func", port_start)
        port_handler = source[port_start:port_end]
        self.assertIn("ftpvita_parse_port", port_handler)
        self.assertNotIn("sscanf", port_handler)
        self.assertNotIn("sprintf", port_handler)

    def test_ftp_listener_waits_for_readiness_and_bounds_clients(self):
        source = (VENDOR / "ftpvita.c").read_text()
        init_start = source.index("int ftpvita_init")
        init_end = source.index("void ftpvita_fini", init_start)
        init = source[init_start:init_end]
        self.assertIn("server_start_state", init)
        self.assertIn("SERVER_START_TIMEOUT_MS", init)
        self.assertIn("if (server_start_state != 1)", init)
        self.assertIn("if (number_clients < MAX_CLIENTS)", source)
        self.assertIn('"421 Too many FTP clients."', source)

    def test_test_ports_are_build_time_overrides_with_canonical_defaults(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        ftp_source = (VENDOR / "ftpvita.c").read_text()
        cmd_source = (ROOT / "src" / "cmd.c").read_text()

        self.assertIn("VITACOMPANION_FTP_PORT 1337", cmake)
        self.assertIn("VITACOMPANION_CMD_PORT 1338", cmake)
        self.assertIn("VITACOMPANION_MODULE_NAME", cmake)
        self.assertIn("FTP_PORT=${VITACOMPANION_FTP_PORT}", cmake)
        self.assertIn("CMD_PORT=${VITACOMPANION_CMD_PORT}", cmake)
        self.assertIn("#ifndef FTP_PORT", ftp_source)
        self.assertIn("#ifndef CMD_PORT", cmd_source)

    def test_control_parser_handles_stream_framing_and_case_insensitive_commands(self):
        source = (VENDOR / "ftpvita.c").read_text()
        consume_start = source.index("static int client_consume_received_data")
        consume_end = source.index("static int client_thread", consume_start)
        consume = source[consume_start:consume_end]
        self.assertIn("client->recv_buffer_used", consume)
        self.assertIn("if (value == '\\n')", consume)
        self.assertIn("client_handle_command_line", consume)

        line_start = source.index("static int client_handle_command_line")
        line_end = source.index("static int client_consume_received_data", line_start)
        line_handler = source[line_start:line_end]
        self.assertIn("cursor[cmd_length] - ('a' - 'A')", line_handler)
        self.assertIn("args_length >= 4", source)

    def test_command_service_tracks_and_aborts_the_accepted_socket(self):
        source = (ROOT / "src" / "cmd.c").read_text()
        self.assertIn("loader_client_sockfd", source)
        self.assertIn("loader_start_state", source)
        self.assertIn("SCE_NET_SO_RCVTIMEO", source)
        shutdown_start = source.index("void cmd_end()")
        shutdown = source[shutdown_start:]
        self.assertLess(
            shutdown.index("sceNetSocketAbort(loader_client_sockfd"),
            shutdown.index("sceKernelWaitThreadEnd(loader_thid"),
        )

    def test_network_teardown_stops_command_service_before_ftp_network_term(self):
        source = (ROOT / "src" / "net.c").read_text()
        shutdown_start = source.index("void net_end()")
        shutdown_end = source.index("static void do_net_connected", shutdown_start)
        shutdown = source[shutdown_start:shutdown_end]
        self.assertLess(shutdown.index("cmd_end();"), shutdown.index("ftpvita_fini();"))

    def test_requested_destructive_upload_semantics_remain_unchanged(self):
        source = (VENDOR / "ftpvita.c").read_text()
        receive_start = source.index("static void receive_file")
        receive_end = source.index("static void cmd_STOR_func", receive_start)
        receive = source[receive_start:receive_end]
        self.assertIn("mode |= SCE_O_TRUNC;", receive)
        self.assertIn("sceIoRemove(path);", receive)

    def test_client_allocation_initializes_fields_explicitly(self):
        source = (VENDOR / "ftpvita.c").read_text()
        server_start = source.index("static int server_thread")
        server_end = source.index("int ftpvita_init", server_start)
        server = source[server_start:server_end]

        allocation_start = server.index(
            "ftpvita_client_info_t *client = malloc(sizeof(*client));"
        )
        initialization_start = server.index("client->num =", allocation_start)
        allocation = server[allocation_start:initialization_start]

        self.assertNotIn("memset(client, 0, sizeof(*client))", allocation)
        self.assertNotIn("socket_set_io_timeouts(client_sockfd", server)
        self.assertIn("client->data_sockaddr = (SceNetSockaddrIn){0};", server)
        self.assertIn("client->pasv_sockaddr = (SceNetSockaddrIn){0};", server)
        self.assertIn("client->n_recv = 0;", server)
        self.assertIn("client->recv_buffer[0] = '\\0';", server)
        self.assertIn('client->recv_cmd_args = "";', server)
        self.assertIn("client->rename_path[0] = '\\0';", server)
        self.assertIn("sceKernelStartThread(client_thid", server)

        rest_start = source.index("static void cmd_REST_func")
        rest_end = source.index("static void cmd_FEAT_func", rest_start)
        rest_handler = source[rest_start:rest_end]
        self.assertIn("ftpvita_parse_restart_offset", rest_handler)
        self.assertNotIn("sscanf", rest_handler)

    def test_ftp_protocol_identifies_as_vitacompanion(self):
        source = (VENDOR / "ftpvita.c").read_text()

        self.assertIn('"220 VitaCompanion Server ready."', source)
        self.assertIn(
            '"211 VitaCompanion ready; %d client%s connected."',
            source,
        )
        self.assertNotIn('"220 FTPVita Server ready."', source)
        self.assertNotIn(
            '"211 FTPVita ready; %d client%s connected."',
            source,
        )
        self.assertIn('"FTPVita_server_sock"', source)
        self.assertIn('"FTPVita_server_thread"', source)

    def test_public_extension_hooks_reject_null_inputs(self):
        source = (VENDOR / "ftpvita.c").read_text()
        self.assertIn("if (!devname)", source)
        self.assertIn("if (!cmd || !func)", source)
        self.assertIn("if (client && msg)", source)
        self.assertIn("if (client && str)", source)


if __name__ == "__main__":
    unittest.main()
