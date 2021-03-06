/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

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

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

#include "util.h"
#include "strv.h"
#include "def.h"
#include "virt.h"
#include "env-util.h"
#include "fileio-label.h"
#include "label.h"
#include "bus-util.h"
#include "event-util.h"

enum {
        PROP_HOSTNAME,
        PROP_STATIC_HOSTNAME,
        PROP_PRETTY_HOSTNAME,
        PROP_ICON_NAME,
        PROP_CHASSIS,
        _PROP_MAX
};

typedef struct Context {
        char *data[_PROP_MAX];
        Hashmap *polkit_registry;
} Context;

static void context_reset(Context *c) {
        int p;

        assert(c);

        for (p = 0; p < _PROP_MAX; p++) {
                free(c->data[p]);
                c->data[p] = NULL;
        }
}

static void context_free(Context *c, sd_bus *bus) {
        assert(c);

        context_reset(c);
        bus_verify_polkit_async_registry_free(bus, c->polkit_registry);
}

static int context_read_data(Context *c) {
        int r;

        assert(c);

        context_reset(c);

        c->data[PROP_HOSTNAME] = gethostname_malloc();
        if (!c->data[PROP_HOSTNAME])
                return -ENOMEM;

        r = read_one_line_file("/etc/hostname", &c->data[PROP_STATIC_HOSTNAME]);
        if (r < 0 && r != -ENOENT)
                return r;

        r = parse_env_file("/etc/machine-info", NEWLINE,
                           "PRETTY_HOSTNAME", &c->data[PROP_PRETTY_HOSTNAME],
                           "ICON_NAME", &c->data[PROP_ICON_NAME],
                           "CHASSIS", &c->data[PROP_CHASSIS],
                           NULL);
        if (r < 0 && r != -ENOENT)
                return r;

        return 0;
}

static bool check_nss(void) {
        void *dl;

        dl = dlopen("libnss_myhostname.so.2", RTLD_LAZY);
        if (dl) {
                dlclose(dl);
                return true;
        }

        return false;
}

static bool valid_chassis(const char *chassis) {

        assert(chassis);

        return nulstr_contains(
                        "vm\0"
                        "container\0"
                        "desktop\0"
                        "laptop\0"
                        "server\0"
                        "tablet\0"
                        "handset\0",
                        chassis);
}

static const char* fallback_chassis(void) {
        int r;
        char *type;
        unsigned t;
        Virtualization v;

        v = detect_virtualization(NULL);

        if (v == VIRTUALIZATION_VM)
                return "vm";
        if (v == VIRTUALIZATION_CONTAINER)
                return "container";

        r = read_one_line_file("/sys/firmware/acpi/pm_profile", &type);
        if (r < 0)
                goto try_dmi;

        r = safe_atou(type, &t);
        free(type);
        if (r < 0)
                goto try_dmi;

        /* We only list the really obvious cases here as the ACPI data
         * is not really super reliable.
         *
         * See the ACPI 5.0 Spec Section 5.2.9.1 for details:
         *
         * http://www.acpi.info/DOWNLOADS/ACPIspec50.pdf
         */

        switch(t) {

        case 1:
        case 3:
        case 6:
                return "desktop";

        case 2:
                return "laptop";

        case 4:
        case 5:
        case 7:
                return "server";

        case 8:
                return "tablet";
        }

try_dmi:
        r = read_one_line_file("/sys/class/dmi/id/chassis_type", &type);
        if (r < 0)
                return NULL;

        r = safe_atou(type, &t);
        free(type);
        if (r < 0)
                return NULL;

        /* We only list the really obvious cases here. The DMI data is
           unreliable enough, so let's not do any additional guesswork
           on top of that.

           See the SMBIOS Specification 2.7.1 section 7.4.1 for
           details about the values listed here:

           http://www.dmtf.org/sites/default/files/standards/documents/DSP0134_2.7.1.pdf
         */

        switch (t) {

        case 0x3:
        case 0x4:
        case 0x6:
        case 0x7:
                return "desktop";

        case 0x8:
        case 0x9:
        case 0xA:
        case 0xE:
                return "laptop";

        case 0xB:
                return "handset";

        case 0x11:
        case 0x1C:
                return "server";
        }

        return NULL;
}

static char* context_fallback_icon_name(Context *c) {
        const char *chassis;

        assert(c);

        if (!isempty(c->data[PROP_CHASSIS]))
                return strappend("computer-", c->data[PROP_CHASSIS]);

        chassis = fallback_chassis();
        if (chassis)
                return strappend("computer-", chassis);

        return strdup("computer");
}

static int context_write_data_hostname(Context *c) {
        const char *hn;

        assert(c);

        if (isempty(c->data[PROP_HOSTNAME]))
                hn = "localhost";
        else
                hn = c->data[PROP_HOSTNAME];

        if (sethostname(hn, strlen(hn)) < 0)
                return -errno;

        return 0;
}

static int context_write_data_static_hostname(Context *c) {

        assert(c);

        if (isempty(c->data[PROP_STATIC_HOSTNAME])) {

                if (unlink("/etc/hostname") < 0)
                        return errno == ENOENT ? 0 : -errno;

                return 0;
        }
        return write_string_file_atomic_label("/etc/hostname", c->data[PROP_STATIC_HOSTNAME]);
}

static int context_write_data_other(Context *c) {

        static const char * const name[_PROP_MAX] = {
                [PROP_PRETTY_HOSTNAME] = "PRETTY_HOSTNAME",
                [PROP_ICON_NAME] = "ICON_NAME",
                [PROP_CHASSIS] = "CHASSIS"
        };

        _cleanup_strv_free_ char **l = NULL;
        int r, p;

        assert(c);

        r = load_env_file("/etc/machine-info", NULL, &l);
        if (r < 0 && r != -ENOENT)
                return r;

        for (p = 2; p < _PROP_MAX; p++) {
                char *t, **u;

                assert(name[p]);

                if (isempty(c->data[p]))  {
                        strv_env_unset(l, name[p]);
                        continue;
                }

                if (asprintf(&t, "%s=%s", name[p], strempty(c->data[p])) < 0)
                        return -ENOMEM;

                u = strv_env_set(l, t);
                free(t);

                if (!u)
                        return -ENOMEM;

                strv_free(l);
                l = u;
        }

        if (strv_isempty(l)) {

                if (unlink("/etc/machine-info") < 0)
                        return errno == ENOENT ? 0 : -errno;

                return 0;
        }

        return write_env_file_label("/etc/machine-info", l);
}

static int property_get_icon_name(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        _cleanup_free_ char *n = NULL;
        Context *c = userdata;
        const char *name;

        if (isempty(c->data[PROP_ICON_NAME]))
                name = n = context_fallback_icon_name(c);
        else
                name = c->data[PROP_ICON_NAME];

        if (!name)
                return -ENOMEM;

        return sd_bus_message_append(reply, "s", name);
}

static int property_get_chassis(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Context *c = userdata;
        const char *name;

        if (isempty(c->data[PROP_CHASSIS]))
                name = fallback_chassis();
        else
                name = c->data[PROP_CHASSIS];

        return sd_bus_message_append(reply, "s", name);
}

static int method_set_hostname(sd_bus *bus, sd_bus_message *m, void *userdata, sd_bus_error *error) {
        Context *c = userdata;
        const char *name;
        int interactive;
        char *h;
        int r;

        r = sd_bus_message_read(m, "sb", &name, &interactive);
        if (r < 0)
                return r;

        if (isempty(name))
                name = c->data[PROP_STATIC_HOSTNAME];

        if (isempty(name))
                name = "localhost";

        if (!hostname_is_valid(name))
                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid hostname '%s'", name);

        if (streq_ptr(name, c->data[PROP_HOSTNAME]))
                return sd_bus_reply_method_return(m, NULL);

        r = bus_verify_polkit_async(bus, &c->polkit_registry, m, "org.freedesktop.hostname1.set-hostname", interactive, error, method_set_hostname, c);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* No authorization for now, but the async polkit stuff will call us again when it has it */

        h = strdup(name);
        if (!h)
                return -ENOMEM;

        free(c->data[PROP_HOSTNAME]);
        c->data[PROP_HOSTNAME] = h;

        r = context_write_data_hostname(c);
        if (r < 0) {
                log_error("Failed to set host name: %s", strerror(-r));
                return sd_bus_error_set_errnof(error, r, "Failed to set hostname: %s", strerror(-r));
        }

        log_info("Changed host name to '%s'", strna(c->data[PROP_HOSTNAME]));

        sd_bus_emit_properties_changed(bus, "/org/freedesktop/hostname1", "org.freedesktop.hostname1", "Hostname", NULL);

        return sd_bus_reply_method_return(m, NULL);
}

static int method_set_static_hostname(sd_bus *bus, sd_bus_message *m, void *userdata, sd_bus_error *error) {
        Context *c = userdata;
        const char *name;
        int interactive;
        int r;

        r = sd_bus_message_read(m, "sb", &name, &interactive);
        if (r < 0)
                return r;

        if (isempty(name))
                name = NULL;

        if (streq_ptr(name, c->data[PROP_STATIC_HOSTNAME]))
                return sd_bus_reply_method_return(m, NULL);

        r = bus_verify_polkit_async(bus, &c->polkit_registry, m, "org.freedesktop.hostname1.set-static-hostname", interactive, error, method_set_static_hostname, c);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* No authorization for now, but the async polkit stuff will call us again when it has it */

        if (isempty(name)) {
                free(c->data[PROP_STATIC_HOSTNAME]);
                c->data[PROP_STATIC_HOSTNAME] = NULL;
        } else {
                char *h;

                if (!hostname_is_valid(name))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid static hostname '%s'", name);

                h = strdup(name);
                if (!h)
                        return -ENOMEM;

                free(c->data[PROP_STATIC_HOSTNAME]);
                c->data[PROP_STATIC_HOSTNAME] = h;
        }

        r = context_write_data_static_hostname(c);
        if (r < 0) {
                log_error("Failed to write static host name: %s", strerror(-r));
                return sd_bus_error_set_errnof(error, r, "Failed to set static hostname: %s", strerror(-r));
        }

        log_info("Changed static host name to '%s'", strna(c->data[PROP_STATIC_HOSTNAME]));

        sd_bus_emit_properties_changed(bus, "/org/freedesktop/hostname1", "org.freedesktop.hostname1", "StaticHostname", NULL);

        return sd_bus_reply_method_return(m, NULL);
}

static int set_machine_info(Context *c, sd_bus *bus, sd_bus_message *m, int prop, sd_bus_message_handler_t cb, sd_bus_error *error) {
        int interactive;
        const char *name;
        int r;

        assert(c);
        assert(bus);
        assert(m);

        r = sd_bus_message_read(m, "sb", &name, &interactive);
        if (r < 0)
                return r;

        if (isempty(name))
                name = NULL;

        if (streq_ptr(name, c->data[prop]))
                return sd_bus_reply_method_return(m, NULL);

        /* Since the pretty hostname should always be changed at the
         * same time as the static one, use the same policy action for
         * both... */

        r = bus_verify_polkit_async(bus, &c->polkit_registry, m, prop == PROP_PRETTY_HOSTNAME ?
                          "org.freedesktop.hostname1.set-static-hostname" :
                          "org.freedesktop.hostname1.set-machine-info", interactive, error, cb, c);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* No authorization for now, but the async polkit stuff will call us again when it has it */

        if (isempty(name)) {
                free(c->data[prop]);
                c->data[prop] = NULL;
        } else {
                char *h;

                /* The icon name might ultimately be used as file
                 * name, so better be safe than sorry */

                if (prop == PROP_ICON_NAME && !filename_is_safe(name))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid icon name '%s'", name);
                if (prop == PROP_PRETTY_HOSTNAME &&
                    (string_has_cc(name) || chars_intersect(name, "\t")))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid pretty host name '%s'", name);
                if (prop == PROP_CHASSIS && !valid_chassis(name))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid chassis '%s'", name);

                h = strdup(name);
                if (!h)
                        return -ENOMEM;

                free(c->data[prop]);
                c->data[prop] = h;
        }

        r = context_write_data_other(c);
        if (r < 0) {
                log_error("Failed to write machine info: %s", strerror(-r));
                return sd_bus_error_set_errnof(error, r, "Failed to write machine info: %s", strerror(-r));
        }

        log_info("Changed %s to '%s'",
                 prop == PROP_PRETTY_HOSTNAME ? "pretty host name" :
                 prop == PROP_CHASSIS ? "chassis" : "icon name", strna(c->data[prop]));

        sd_bus_emit_properties_changed(bus, "/org/freedesktop/hostname1", "org.freedesktop.hostname1",
                                       prop == PROP_PRETTY_HOSTNAME ? "PrettyHostname" :
                                       prop == PROP_CHASSIS ? "Chassis" : "IconName", NULL);

        return sd_bus_reply_method_return(m, NULL);
}

static int method_set_pretty_hostname(sd_bus *bus, sd_bus_message *m, void *userdata, sd_bus_error *error) {
        return set_machine_info(userdata, bus, m, PROP_PRETTY_HOSTNAME, method_set_pretty_hostname, error);
}

static int method_set_icon_name(sd_bus *bus, sd_bus_message *m, void *userdata, sd_bus_error *error) {
        return set_machine_info(userdata, bus, m, PROP_ICON_NAME, method_set_icon_name, error);
}

static int method_set_chassis(sd_bus *bus, sd_bus_message *m, void *userdata, sd_bus_error *error) {
        return set_machine_info(userdata, bus, m, PROP_CHASSIS, method_set_chassis, error);
}

static const sd_bus_vtable hostname_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Hostname", "s", NULL, offsetof(Context, data) + sizeof(char*) * PROP_HOSTNAME, 0),
        SD_BUS_PROPERTY("StaticHostname", "s", NULL, offsetof(Context, data) + sizeof(char*) * PROP_STATIC_HOSTNAME, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("PrettyHostname", "s", NULL, offsetof(Context, data) + sizeof(char*) * PROP_PRETTY_HOSTNAME, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("IconName", "s", property_get_icon_name, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Chassis", "s", property_get_chassis, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_METHOD("SetHostname", "sb", NULL, method_set_hostname, 0),
        SD_BUS_METHOD("SetStaticHostname", "sb", NULL, method_set_static_hostname, 0),
        SD_BUS_METHOD("SetPrettyHostname", "sb", NULL, method_set_pretty_hostname, 0),
        SD_BUS_METHOD("SetIconName", "sb", NULL, method_set_icon_name, 0),
        SD_BUS_METHOD("SetChassis", "sb", NULL, method_set_chassis, 0),
        SD_BUS_VTABLE_END,
};

static int connect_bus(Context *c, sd_event *event, sd_bus **_bus) {
        _cleanup_bus_unref_ sd_bus *bus = NULL;
        int r;

        assert(c);
        assert(event);
        assert(_bus);

        r = sd_bus_default_system(&bus);
        if (r < 0) {
                log_error("Failed to get system bus connection: %s", strerror(-r));
                return r;
        }

        r = sd_bus_add_object_vtable(bus, "/org/freedesktop/hostname1", "org.freedesktop.hostname1", hostname_vtable, c);
        if (r < 0) {
                log_error("Failed to register object: %s", strerror(-r));
                return r;
        }

        r = sd_bus_request_name(bus, "org.freedesktop.hostname1", SD_BUS_NAME_REPLACE_EXISTING|SD_BUS_NAME_DO_NOT_QUEUE);
        if (r < 0) {
                log_error("Failed to register name: %s", strerror(-r));
                return r;
        }

        r = sd_bus_attach_event(bus, event, 0);
        if (r < 0) {
                log_error("Failed to attach bus to event loop: %s", strerror(-r));
                return r;
        }

        *_bus = bus;
        bus = NULL;

        return 0;
}

int main(int argc, char *argv[]) {
        Context context = {};

        _cleanup_event_unref_ sd_event *event = NULL;
        _cleanup_bus_unref_ sd_bus *bus = NULL;
        int r;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);
        label_init("/etc");

        if (argc != 1) {
                log_error("This program takes no arguments.");
                r = -EINVAL;
                goto finish;
        }

        if (!check_nss())
                log_warning("Warning: nss-myhostname is not installed. Changing the local hostname might make it unresolveable. Please install nss-myhostname!");

        if (argc != 1) {
                log_error("This program takes no arguments.");
                r = -EINVAL;
                goto finish;
        }

        r = sd_event_default(&event);
        if (r < 0) {
                log_error("Failed to allocate event loop: %s", strerror(-r));
                goto finish;
        }

        r = connect_bus(&context, event, &bus);
        if (r < 0)
                goto finish;

        r = context_read_data(&context);
        if (r < 0) {
                log_error("Failed to read timezone data: %s", strerror(-r));
                goto finish;
        }

        r = bus_event_loop_with_idle(event, bus, "org.freedesktop.hostname1", DEFAULT_EXIT_USEC);
        if (r < 0) {
                log_error("Failed to run event loop: %s", strerror(-r));
                goto finish;
        }

        r = 0;

finish:
        context_free(&context, bus);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
