/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2012 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <locale.h>
#include <string.h>
#include <sys/timex.h>
#include <sys/utsname.h>

#include "sd-bus.h"

#include "bus-util.h"
#include "bus-error.h"
#include "util.h"
#include "spawn-polkit-agent.h"
#include "build.h"
#include "hwclock.h"
#include "strv.h"
#include "sd-id128.h"
#include "virt.h"
#include "fileio.h"

static bool arg_ask_password = true;
static BusTransport arg_transport = BUS_TRANSPORT_LOCAL;
static char *arg_host = NULL;
static bool arg_transient = false;
static bool arg_pretty = false;
static bool arg_static = false;

static void polkit_agent_open_if_enabled(void) {

        /* Open the polkit agent as a child process if necessary */
        if (!arg_ask_password)
                return;

        if (arg_transport != BUS_TRANSPORT_LOCAL)
                return;

        polkit_agent_open();
}

typedef struct StatusInfo {
        char *hostname;
        char *static_hostname;
        char *pretty_hostname;
        char *icon_name;
        char *chassis;
} StatusInfo;

static void print_status_info(StatusInfo *i) {
        sd_id128_t mid, bid;
        int r;
        const char *id = NULL;
        _cleanup_free_ char *pretty_name = NULL, *cpe_name = NULL;
        struct utsname u;

        assert(i);

        printf("   Static hostname: %s\n",
               strna(i->static_hostname));

        if (!isempty(i->pretty_hostname) &&
            !streq_ptr(i->pretty_hostname, i->static_hostname))
                printf("   Pretty hostname: %s\n",
                       strna(i->pretty_hostname));

        if (!isempty(i->hostname) &&
            !streq_ptr(i->hostname, i->static_hostname))
                printf("Transient hostname: %s\n",
                       strna(i->hostname));

        printf("         Icon name: %s\n"
               "           Chassis: %s\n",
               strna(i->icon_name),
               strna(i->chassis));

        r = sd_id128_get_machine(&mid);
        if (r >= 0)
                printf("        Machine ID: " SD_ID128_FORMAT_STR "\n", SD_ID128_FORMAT_VAL(mid));

        r = sd_id128_get_boot(&bid);
        if (r >= 0)
                printf("           Boot ID: " SD_ID128_FORMAT_STR "\n", SD_ID128_FORMAT_VAL(bid));

        if (detect_virtualization(&id) > 0)
                printf("    Virtualization: %s\n", id);

        r = parse_env_file("/etc/os-release", NEWLINE,
                           "PRETTY_NAME", &pretty_name,
                           "CPE_NAME", &cpe_name,
                           NULL);
        if (r < 0)
                log_warning("Failed to read /etc/os-release: %s", strerror(-r));

        if (!isempty(pretty_name))
                printf("  Operating System: %s\n", pretty_name);

        if (!isempty(cpe_name))
                printf("       CPE OS Name: %s\n", cpe_name);

        assert_se(uname(&u) >= 0);
        printf("            Kernel: %s %s\n"
               "      Architecture: %s\n", u.sysname, u.release, u.machine);

}

static int show_one_name(sd_bus *bus, const char* attr) {
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        const char *s;
        int r;

        r = sd_bus_get_property(
                        bus,
                        "org.freedesktop.hostname1",
                        "/org/freedesktop/hostname1",
                        "org.freedesktop.hostname1",
                        attr,
                        &error, &reply, "s");
        if (r < 0) {
                log_error("Could not get property: %s", bus_error_message(&error, -r));
                return r;
        }

        r = sd_bus_message_read(reply, "s", &s);
        if (r < 0)
                return bus_log_parse_error(r);

        printf("%s\n", s);

        return 0;
}

static int show_all_names(sd_bus *bus) {
        StatusInfo info = {};
        static const struct bus_properties_map map[]  = {
                { "Hostname",       "s", NULL, offsetof(StatusInfo, hostname) },
                { "StaticHostname", "s", NULL, offsetof(StatusInfo, static_hostname) },
                { "PrettyHostname", "s", NULL, offsetof(StatusInfo, pretty_hostname) },
                { "IconName",       "s", NULL, offsetof(StatusInfo, icon_name) },
                { "Chassis",        "s", NULL, offsetof(StatusInfo, chassis) },
                {}
        };
        int r;

        r = bus_map_all_properties(bus,
                                   "org.freedesktop.hostname1",
                                   "/org/freedesktop/hostname1",
                                   map,
                                   &info);
        if (r < 0)
                goto fail;

        print_status_info(&info);

fail:
        free(info.hostname);
        free(info.static_hostname);
        free(info.pretty_hostname);
        free(info.icon_name);
        free(info.chassis);
        return 0;
}

static int show_status(sd_bus *bus, char **args, unsigned n) {
        assert(args);

        if (arg_pretty || arg_static || arg_transient) {
                const char *attr;

                if (!!arg_static + !!arg_pretty + !!arg_transient > 1) {
                        log_error("Cannot query more than one name type at a time");
                        return -EINVAL;
                }

                attr = arg_pretty ? "PrettyHostname" :
                        arg_static ? "StaticHostname" : "Hostname";

                return show_one_name(bus, attr);
        } else
                return show_all_names(bus);
}

static int set_simple_string(sd_bus *bus, const char *method, const char *value) {
        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        int r = 0;

        polkit_agent_open_if_enabled();

        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.hostname1",
                        "/org/freedesktop/hostname1",
                        "org.freedesktop.hostname1",
                        method,
                        &error, NULL,
                        "sb", value, arg_ask_password);
        if (r < 0)
                log_error("Could not set property: %s", bus_error_message(&error, -r));
        return r;
}

static int set_hostname(sd_bus *bus, char **args, unsigned n) {
        _cleanup_free_ char *h = NULL;
        const char *hostname = args[1];
        int r;

        assert(args);
        assert(n == 2);

        if (!arg_pretty && !arg_static && !arg_transient)
                arg_pretty = arg_static = arg_transient = true;

        if (arg_pretty) {
                const char *p;

                /* If the passed hostname is already valid, then
                 * assume the user doesn't know anything about pretty
                 * hostnames, so let's unset the pretty hostname, and
                 * just set the passed hostname as static/dynamic
                 * hostname. */

                h = strdup(hostname);
                if (!h)
                        return log_oom();

                hostname_cleanup(h, true);

                if (arg_static && streq(h, hostname))
                        p = "";
                else {
                        p = hostname;
                        hostname = h;
                }

                r = set_simple_string(bus, "SetPrettyHostname", p);
                if (r < 0)
                        return r;
        }

        if (arg_static) {
                r = set_simple_string(bus, "SetStaticHostname", hostname);
                if (r < 0)
                        return r;
        }

        if (arg_transient) {
                r = set_simple_string(bus, "SetHostname", hostname);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int set_icon_name(sd_bus *bus, char **args, unsigned n) {
        assert(args);
        assert(n == 2);

        return set_simple_string(bus, "SetIconName", args[1]);
}

static int set_chassis(sd_bus *bus, char **args, unsigned n) {
        assert(args);
        assert(n == 2);

        return set_simple_string(bus, "SetChasis", args[1]);
}

static int help(void) {

        printf("%s [OPTIONS...] COMMAND ...\n\n"
               "Query or change system hostname.\n\n"
               "  -h --help              Show this help\n"
               "     --version           Show package version\n"
               "     --no-ask-password   Do not prompt for password\n"
               "  -H --host=[USER@]HOST  Operate on remote host\n"
               "  -M --machine=CONTAINER Operate on local container\n"
               "     --transient         Only set transient hostname\n"
               "     --static            Only set static hostname\n"
               "     --pretty            Only set pretty hostname\n\n"
               "Commands:\n"
               "  status                 Show current hostname settings\n"
               "  set-hostname NAME      Set system hostname\n"
               "  set-icon-name NAME     Set icon name for host\n"
               "  set-chassis NAME       Set chassis type for host\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_NO_ASK_PASSWORD,
                ARG_TRANSIENT,
                ARG_STATIC,
                ARG_PRETTY
        };

        static const struct option options[] = {
                { "help",            no_argument,       NULL, 'h'                 },
                { "version",         no_argument,       NULL, ARG_VERSION         },
                { "transient",       no_argument,       NULL, ARG_TRANSIENT       },
                { "static",          no_argument,       NULL, ARG_STATIC          },
                { "pretty",          no_argument,       NULL, ARG_PRETTY          },
                { "host",            required_argument, NULL, 'H'                 },
                { "machine",         required_argument, NULL, 'M'                 },
                { "no-ask-password", no_argument,       NULL, ARG_NO_ASK_PASSWORD },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hH:M:", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        puts(PACKAGE_STRING);
                        puts(SYSTEMD_FEATURES);
                        return 0;

                case 'H':
                        arg_transport = BUS_TRANSPORT_REMOTE;
                        arg_host = optarg;
                        break;

                case 'M':
                        arg_transport = BUS_TRANSPORT_CONTAINER;
                        arg_host = optarg;
                        break;

                case ARG_TRANSIENT:
                        arg_transient = true;
                        break;

                case ARG_PRETTY:
                        arg_pretty = true;
                        break;

                case ARG_STATIC:
                        arg_static = true;
                        break;

                case ARG_NO_ASK_PASSWORD:
                        arg_ask_password = false;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }
        }

        return 1;
}

static int hostnamectl_main(sd_bus *bus, int argc, char *argv[]) {

        static const struct {
                const char* verb;
                const enum {
                        MORE,
                        LESS,
                        EQUAL
                } argc_cmp;
                const int argc;
                int (* const dispatch)(sd_bus *bus, char **args, unsigned n);
        } verbs[] = {
                { "status",        LESS,  1, show_status   },
                { "set-hostname",  EQUAL, 2, set_hostname  },
                { "set-icon-name", EQUAL, 2, set_icon_name },
                { "set-chassis",   EQUAL, 2, set_chassis   },
        };

        int left;
        unsigned i;

        assert(argc >= 0);
        assert(argv);

        left = argc - optind;

        if (left <= 0)
                /* Special rule: no arguments means "status" */
                i = 0;
        else {
                if (streq(argv[optind], "help")) {
                        help();
                        return 0;
                }

                for (i = 0; i < ELEMENTSOF(verbs); i++)
                        if (streq(argv[optind], verbs[i].verb))
                                break;

                if (i >= ELEMENTSOF(verbs)) {
                        log_error("Unknown operation %s", argv[optind]);
                        return -EINVAL;
                }
        }

        switch (verbs[i].argc_cmp) {

        case EQUAL:
                if (left != verbs[i].argc) {
                        log_error("Invalid number of arguments.");
                        return -EINVAL;
                }

                break;

        case MORE:
                if (left < verbs[i].argc) {
                        log_error("Too few arguments.");
                        return -EINVAL;
                }

                break;

        case LESS:
                if (left > verbs[i].argc) {
                        log_error("Too many arguments.");
                        return -EINVAL;
                }

                break;

        default:
                assert_not_reached("Unknown comparison operator.");
        }

        return verbs[i].dispatch(bus, argv + optind, left);
}

int main(int argc, char *argv[]) {
        _cleanup_bus_unref_ sd_bus *bus = NULL;
        int r;

        setlocale(LC_ALL, "");
        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        r = bus_open_transport(arg_transport, arg_host, false, &bus);
        if (r < 0) {
                log_error("Failed to create bus connection: %s", strerror(-r));
                goto finish;
        }

        r = hostnamectl_main(bus, argc, argv);

finish:
        return r < 0 ? EXIT_FAILURE : r;
}
