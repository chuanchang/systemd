/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

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

#include "special.h"
#include "bus-kernel.h"
#include "bus-internal.h"
#include "bus-util.h"
#include "service.h"
#include "dbus-busname.h"
#include "busname.h"

static const UnitActiveState state_translation_table[_BUSNAME_STATE_MAX] = {
        [BUSNAME_DEAD] = UNIT_INACTIVE,
        [BUSNAME_LISTENING] = UNIT_ACTIVE,
        [BUSNAME_RUNNING] = UNIT_ACTIVE,
        [BUSNAME_FAILED] = UNIT_FAILED
};

static int busname_dispatch_io(sd_event_source *source, int fd, uint32_t revents, void *userdata);

static void busname_init(Unit *u) {
        BusName *n = BUSNAME(u);

        assert(u);
        assert(u->load_state == UNIT_STUB);

        n->starter_fd = -1;
}

static void busname_done(Unit *u) {
        BusName *n = BUSNAME(u);

        assert(u);

        free(n->name);
        n->name = NULL;

        unit_ref_unset(&n->service);

        n->event_source = sd_event_source_unref(n->event_source);

        if (n->starter_fd >= 0) {
                close_nointr_nofail(n->starter_fd);
                n->starter_fd = -1;
        }
}

static int busname_add_default_default_dependencies(BusName *n) {
        int r;

        assert(n);

        r = unit_add_dependency_by_name(UNIT(n), UNIT_BEFORE, SPECIAL_BUSNAMES_TARGET, NULL, true);
        if (r < 0)
                return r;

        if (UNIT(n)->manager->running_as == SYSTEMD_SYSTEM) {
                r = unit_add_two_dependencies_by_name(UNIT(n), UNIT_AFTER, UNIT_REQUIRES, SPECIAL_SYSINIT_TARGET, NULL, true);
                if (r < 0)
                        return r;
        }

        return unit_add_two_dependencies_by_name(UNIT(n), UNIT_BEFORE, UNIT_CONFLICTS, SPECIAL_SHUTDOWN_TARGET, NULL, true);
}

static int busname_add_extras(BusName *n) {
        Unit *u = UNIT(n);
        int r;

        assert(n);

        if (!n->name) {
                n->name = unit_name_to_prefix(u->id);
                if (!n->name)
                        return -ENOMEM;
        }

        if (!u->description) {
                r = unit_set_description(u, n->name);
                if (r < 0)
                        return r;
        }

        if (!UNIT_DEREF(n->service)) {
                Unit *x;

                r = unit_load_related_unit(u, ".service", &x);
                if (r < 0)
                        return r;

                unit_ref_set(&n->service, x);
        }

        r = unit_add_two_dependencies(u, UNIT_BEFORE, UNIT_TRIGGERS, UNIT_DEREF(n->service), true);
        if (r < 0)
                return r;

        if (u->default_dependencies) {
                r = busname_add_default_default_dependencies(n);
                if (r < 0)
                        return r;
        }

        return 0;
}



static int busname_verify(BusName *n) {
        char *e;

        assert(n);

        if (UNIT(n)->load_state != UNIT_LOADED)
                return 0;

        if (!service_name_is_valid(n->name)) {
                log_error_unit(UNIT(n)->id, "%s's Name= setting is not a valid service name Refusing.", UNIT(n)->id);
                return -EINVAL;
        }

        e = strappenda(n->name, ".busname");
        if (!unit_has_name(UNIT(n), e)) {
                log_error_unit(UNIT(n)->id, "%s's Name= setting doesn't match unit name. Refusing.", UNIT(n)->id);
                return -EINVAL;
        }

        return 0;
}

static int busname_load(Unit *u) {
        BusName *n = BUSNAME(u);
        int r;

        assert(u);
        assert(u->load_state == UNIT_STUB);

        r = unit_load_fragment_and_dropin(u);
        if (r < 0)
                return r;

        if (u->load_state == UNIT_LOADED) {
                /* This is a new unit? Then let's add in some extras */
                r = busname_add_extras(n);
                if (r < 0)
                        return r;
        }

        return busname_verify(n);
}

static void busname_dump(Unit *u, FILE *f, const char *prefix) {
        BusName *n = BUSNAME(u);

        assert(n);
        assert(f);

        fprintf(f,
                "%sBus Name State: %s\n"
                "%sResult: %s\n"
                "%sName: %s\n",
                prefix, busname_state_to_string(n->state),
                prefix, busname_result_to_string(n->result),
                prefix, n->name);
}

static void busname_unwatch_fd(BusName *n) {
        int r;

        assert(n);

        if (n->event_source) {
                r = sd_event_source_set_enabled(n->event_source, SD_EVENT_OFF);
                if (r < 0)
                        log_debug_unit(UNIT(n)->id, "Failed to disable event source.");
        }
}

static void busname_close_fd(BusName *n) {
        assert(n);

        if (n->starter_fd <= 0)
                return;

        close_nointr_nofail(n->starter_fd);
        n->starter_fd = -1;
}

static int busname_watch_fd(BusName *n) {
        int r;

        assert(n);

        if (n->starter_fd < 0)
                return 0;

        if (n->event_source)
                r = sd_event_source_set_enabled(n->event_source, SD_EVENT_ON);
        else
                r = sd_event_add_io(UNIT(n)->manager->event, n->starter_fd, EPOLLIN, busname_dispatch_io, n, &n->event_source);
        if (r < 0) {
                log_warning_unit(UNIT(n)->id, "Failed to watch starter fd: %s", strerror(-r));
                busname_unwatch_fd(n);
                return r;
        }

        return 0;
}

static int busname_open_fd(BusName *n) {
        assert(n);

        if (n->starter_fd >= 0)
                return 0;

        n->starter_fd = bus_kernel_create_starter(UNIT(n)->manager->running_as == SYSTEMD_SYSTEM ? "system" : "user", n->name);
        if (n->starter_fd < 0) {
                log_warning_unit(UNIT(n)->id, "Failed to create starter fd: %s", strerror(-n->starter_fd));
                return n->starter_fd;
        }

        return 0;
}

static void busname_set_state(BusName *n, BusNameState state) {
        BusNameState old_state;
        assert(n);

        old_state = n->state;
        n->state = state;

        if (state != BUSNAME_LISTENING)
                busname_unwatch_fd(n);

        if (!IN_SET(state, BUSNAME_LISTENING, BUSNAME_RUNNING))
                busname_close_fd(n);

        if (state != old_state)
                log_debug_unit(UNIT(n)->id, "%s changed %s -> %s",
                               UNIT(n)->id, busname_state_to_string(old_state), busname_state_to_string(state));

        unit_notify(UNIT(n), state_translation_table[old_state], state_translation_table[state], true);
}

static int busname_coldplug(Unit *u) {
        BusName *n = BUSNAME(u);
        int r;

        assert(n);
        assert(n->state == BUSNAME_DEAD);

        if (n->deserialized_state == n->state)
                return 0;

        if (IN_SET(n->deserialized_state, BUSNAME_LISTENING, BUSNAME_RUNNING)) {
                r = busname_open_fd(n);
                if (r < 0)
                        return r;
        }

        if (n->deserialized_state == BUSNAME_LISTENING) {
                r = busname_watch_fd(n);
                if (r < 0)
                        return r;
        }

        busname_set_state(n, n->deserialized_state);
        return 0;
}

static void busname_enter_dead(BusName *n, BusNameResult f) {
        assert(n);

        if (f != BUSNAME_SUCCESS)
                n->result = f;

        busname_set_state(n, n->result != BUSNAME_SUCCESS ? BUSNAME_FAILED : BUSNAME_DEAD);
}

static void busname_enter_listening(BusName *n) {
        int r;

        assert(n);

        r = busname_open_fd(n);
        if (r < 0) {
                log_warning_unit(UNIT(n)->id, "%s failed to listen on bus names: %s", UNIT(n)->id, strerror(-r));
                goto fail;
        }

        r = busname_watch_fd(n);
        if (r < 0) {
                log_warning_unit(UNIT(n)->id, "%s failed to watch names: %s", UNIT(n)->id, strerror(-r));
                goto fail;
        }

        busname_set_state(n, BUSNAME_LISTENING);
        return;

fail:
        busname_enter_dead(n, BUSNAME_FAILURE_RESOURCES);
}

static void busname_enter_running(BusName *n) {
        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        bool pending = false;
        Unit *other;
        Iterator i;
        int r;

        assert(n);

        /* We don't take conenctions anymore if we are supposed to
         * shut down anyway */

        if (unit_stop_pending(UNIT(n))) {
                log_debug_unit(UNIT(n)->id, "Suppressing activation request on %s since unit stop is scheduled.", UNIT(n)->id);
                return;
        }

        /* If there's already a start pending don't bother to do
         * anything */
        SET_FOREACH(other, UNIT(n)->dependencies[UNIT_TRIGGERS], i)
                if (unit_active_or_pending(other)) {
                        pending = true;
                        break;
                }

        if (!pending) {
                r = manager_add_job(UNIT(n)->manager, JOB_START, UNIT_DEREF(n->service), JOB_REPLACE, true, &error, NULL);
                if (r < 0)
                        goto fail;
        }

        busname_set_state(n, BUSNAME_RUNNING);
        return;

fail:
        log_warning_unit(UNIT(n)->id, "%s failed to queue service startup job: %s", UNIT(n)->id, bus_error_message(&error, r));
        busname_enter_dead(n, BUSNAME_FAILURE_RESOURCES);
}

static int busname_start(Unit *u) {
        BusName *n = BUSNAME(u);

        assert(n);

        if (UNIT_ISSET(n->service)) {
                Service *service;

                service = SERVICE(UNIT_DEREF(n->service));

                if (UNIT(service)->load_state != UNIT_LOADED) {
                        log_error_unit(u->id, "Bus service %s not loaded, refusing.", UNIT(service)->id);
                        return -ENOENT;
                }
        }

        assert(IN_SET(n->state, BUSNAME_DEAD, BUSNAME_FAILED));

        n->result = BUSNAME_SUCCESS;
        busname_enter_listening(n);

        return 0;
}

static int busname_stop(Unit *u) {
        BusName *n = BUSNAME(u);

        assert(n);
        assert(n->state == BUSNAME_LISTENING || n->state == BUSNAME_RUNNING);

        busname_enter_dead(n, BUSNAME_SUCCESS);
        return 0;
}

static int busname_serialize(Unit *u, FILE *f, FDSet *fds) {
        BusName *n = BUSNAME(u);

        assert(n);
        assert(f);
        assert(fds);

        unit_serialize_item(u, f, "state", busname_state_to_string(n->state));
        unit_serialize_item(u, f, "result", busname_result_to_string(n->result));

        if (n->starter_fd >= 0) {
                int copy;

                copy = fdset_put_dup(fds, n->starter_fd);
                if (copy < 0)
                        return copy;

                unit_serialize_item_format(u, f, "starter-fd", "%i", copy);
        }

        return 0;
}

static int busname_deserialize_item(Unit *u, const char *key, const char *value, FDSet *fds) {
        BusName *n = BUSNAME(u);

        assert(n);
        assert(key);
        assert(value);

        if (streq(key, "state")) {
                BusNameState state;

                state = busname_state_from_string(value);
                if (state < 0)
                        log_debug_unit(u->id, "Failed to parse state value %s", value);
                else
                        n->deserialized_state = state;

        } else if (streq(key, "result")) {
                BusNameResult f;

                f = busname_result_from_string(value);
                if (f < 0)
                        log_debug_unit(u->id, "Failed to parse result value %s", value);
                else if (f != BUSNAME_SUCCESS)
                        n->result = f;

        } else if (streq(key, "starter-fd")) {
                int fd;

                if (safe_atoi(value, &fd) < 0 || fd < 0 || !fdset_contains(fds, fd))
                        log_debug_unit(u->id, "Failed to parse starter fd value %s", value);
                else {
                        if (n->starter_fd >= 0)
                                close_nointr_nofail(n->starter_fd);
                        n->starter_fd = fdset_remove(fds, fd);
                }
        } else
                log_debug_unit(u->id, "Unknown serialization key '%s'", key);

        return 0;
}

_pure_ static UnitActiveState busname_active_state(Unit *u) {
        assert(u);

        return state_translation_table[BUSNAME(u)->state];
}

_pure_ static const char *busname_sub_state_to_string(Unit *u) {
        assert(u);

        return busname_state_to_string(BUSNAME(u)->state);
}

static int busname_dispatch_io(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        BusName *n = userdata;

        assert(n);
        assert(fd >= 0);

        if (n->state != BUSNAME_LISTENING)
                return 0;

        log_debug_unit(UNIT(n)->id, "Activation request on %s", UNIT(n)->id);

        if (revents != EPOLLIN) {
                log_error_unit(UNIT(n)->id, "%s: Got unexpected poll event (0x%x) on starter fd.",
                               UNIT(n)->id, revents);
                goto fail;
        }

        busname_enter_running(n);
        return 0;
fail:

        busname_enter_dead(n, BUSNAME_FAILURE_RESOURCES);
        return 0;
}

static void busname_reset_failed(Unit *u) {
        BusName *n = BUSNAME(u);

        assert(n);

        if (n->state == BUSNAME_FAILED)
                busname_set_state(n, BUSNAME_DEAD);

        n->result = BUSNAME_SUCCESS;
}

static void busname_trigger_notify(Unit *u, Unit *other) {
        BusName *n = BUSNAME(u);
        Service *s;

        assert(n);
        assert(other);

        if (!IN_SET(n->state, BUSNAME_RUNNING, BUSNAME_LISTENING))
                return;

        if (other->load_state != UNIT_LOADED || other->type != UNIT_SERVICE)
                return;

        s = SERVICE(other);

        if (s->state == SERVICE_FAILED) {
                if (s->result == SERVICE_FAILURE_START_LIMIT)
                        busname_enter_dead(n, BUSNAME_FAILURE_SERVICE_FAILED_PERMANENT);
                else
                        busname_enter_listening(n);
        }

        if (IN_SET(n->state,
                   SERVICE_DEAD,
                   SERVICE_STOP, SERVICE_STOP_SIGTERM, SERVICE_STOP_SIGKILL,
                   SERVICE_STOP_POST, SERVICE_FINAL_SIGTERM, SERVICE_FINAL_SIGKILL,
                   SERVICE_AUTO_RESTART))
                busname_enter_listening(n);
}

static const char* const busname_state_table[_BUSNAME_STATE_MAX] = {
        [BUSNAME_DEAD] = "dead",
        [BUSNAME_LISTENING] = "listening",
        [BUSNAME_RUNNING] = "running",
        [BUSNAME_FAILED] = "failed"
};

DEFINE_STRING_TABLE_LOOKUP(busname_state, BusNameState);

static const char* const busname_result_table[_BUSNAME_RESULT_MAX] = {
        [BUSNAME_SUCCESS] = "success",
        [BUSNAME_FAILURE_RESOURCES] = "resources",
};

DEFINE_STRING_TABLE_LOOKUP(busname_result, BusNameResult);

const UnitVTable busname_vtable = {
        .object_size = sizeof(BusName),

        .sections =
                "Unit\0"
                "BusName\0"
                "Install\0",
        .private_section = "BusName",

        .init = busname_init,
        .done = busname_done,
        .load = busname_load,

        .coldplug = busname_coldplug,

        .dump = busname_dump,

        .start = busname_start,
        .stop = busname_stop,

        .serialize = busname_serialize,
        .deserialize_item = busname_deserialize_item,

        .active_state = busname_active_state,
        .sub_state_to_string = busname_sub_state_to_string,

        .trigger_notify = busname_trigger_notify,

        .reset_failed = busname_reset_failed,

        .bus_interface = "org.freedesktop.systemd1.BusName",
        .bus_vtable = bus_busname_vtable,
        .bus_changing_properties = bus_busname_changing_properties,

        .status_message_formats = {
                .finished_start_job = {
                        [JOB_DONE]       = "Listening on %s.",
                        [JOB_FAILED]     = "Failed to listen on %s.",
                        [JOB_DEPENDENCY] = "Dependency failed for %s.",
                        [JOB_TIMEOUT]    = "Timed out starting %s.",
                },
                .finished_stop_job = {
                        [JOB_DONE]       = "Closed %s.",
                        [JOB_FAILED]     = "Failed stopping %s.",
                        [JOB_TIMEOUT]    = "Timed out stopping %s.",
                },
        },
};
