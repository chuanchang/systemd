/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Tom Gundersen <teg@jklm.no>

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

#include <netinet/ether.h>
#include <linux/if.h>

#include "networkd.h"
#include "libudev-private.h"
#include "util.h"

int link_new(Manager *manager, struct udev_device *device, Link **ret) {
        _cleanup_link_free_ Link *link = NULL;
        const char *mac;
        struct ether_addr *mac_addr;
        const char *ifname;
        int r;

        assert(device);
        assert(ret);

        link = new0(Link, 1);
        if (!link)
                return -ENOMEM;

        link->manager = manager;
        link->state = _LINK_STATE_INVALID;

        link->ifindex = udev_device_get_ifindex(device);
        if (link->ifindex <= 0)
                return -EINVAL;

        mac = udev_device_get_sysattr_value(device, "address");
        if (mac) {
                mac_addr = ether_aton(mac);
                if (mac_addr)
                        memcpy(&link->mac, mac_addr, sizeof(struct ether_addr));
        }

        ifname = udev_device_get_sysname(device);
        link->ifname = strdup(ifname);

        r = hashmap_put(manager->links, &link->ifindex, link);
        if (r < 0)
                return r;

        *ret = link;
        link = NULL;

        return 0;
}

void link_free(Link *link) {
        if (!link)
                return;

        assert(link->manager);

        hashmap_remove(link->manager->links, &link->ifindex);

        free(link->ifname);

        free(link);
}

int link_add(Manager *m, struct udev_device *device) {
        Link *link;
        Network *network;
        int r;
        uint64_t ifindex;
        const char *devtype;

        assert(m);
        assert(device);

        ifindex = udev_device_get_ifindex(device);
        link = hashmap_get(m->links, &ifindex);
        if (link)
                return 0;

        r = link_new(m, device, &link);
        if (r < 0) {
                log_error("Could not create link: %s", strerror(-r));
                return r;
        }

        devtype = udev_device_get_devtype(device);
        if (streq_ptr(devtype, "bridge")) {
                r = bridge_set_link(m, link);
                if (r < 0)
                        return r == -ENOENT ? 0 : r;
        }

        r = network_get(m, device, &network);
        if (r < 0)
                return r == -ENOENT ? 0 : r;

        r = network_apply(m, network, link);
        if (r < 0)
                return r;

        return 0;
}

static int link_enter_configured(Link *link) {
        log_info("Link '%s' configured", link->ifname);

        link->state = LINK_STATE_CONFIGURED;

        return 0;
}

static int link_enter_failed(Link *link) {
        log_warning("Could not configure link '%s'", link->ifname);

        link->state = LINK_STATE_FAILED;

        return 0;
}

static bool link_is_up(Link *link) {
        return link->flags & IFF_UP;
}

static int link_enter_routes_set(Link *link) {
        log_info("Routes set for link '%s'", link->ifname);

        if (link_is_up(link))
                return link_enter_configured(link);

        link->state = LINK_STATE_ROUTES_SET;

        return 0;
}

static int route_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(link->rtnl_messages > 0);
        assert(link->state == LINK_STATE_SET_ROUTES || link->state == LINK_STATE_FAILED);

        link->rtnl_messages --;

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0 && r != -EEXIST)
                log_warning("Could not set route on interface '%s': %s",
                            link->ifname, strerror(-r));

        if (link->rtnl_messages == 0)
                return link_enter_routes_set(link);

        return 1;
}

static int link_enter_set_routes(Link *link) {
        Route *route;
        int r;

        assert(link);
        assert(link->network);
        assert(link->rtnl_messages == 0);
        assert(link->state == LINK_STATE_ADDRESSES_SET);

        link->state = LINK_STATE_SET_ROUTES;

        if (!link->network->routes)
                return link_enter_routes_set(link);

        LIST_FOREACH(routes, route, link->network->routes) {
                r = route_configure(route, link, &route_handler);
                if (r < 0)
                        return link_enter_failed(link);

                link->rtnl_messages ++;
        }

        return 0;
}

static int link_enter_addresses_set(Link *link) {
        log_info("Addresses set for link '%s'", link->ifname);

        link->state = LINK_STATE_ADDRESSES_SET;

        return link_enter_set_routes(link);
}

static int address_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(link->rtnl_messages > 0);
        assert(link->state == LINK_STATE_SET_ADDRESSES || link->state == LINK_STATE_FAILED);

        link->rtnl_messages --;

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0 && r != -EEXIST)
                log_warning("Could not set address on interface '%s': %s",
                            link->ifname, strerror(-r));

        if (link->rtnl_messages == 0)
                link_enter_addresses_set(link);

        return 1;
}

static int link_enter_set_addresses(Link *link) {
        Address *address;
        int r;

        assert(link);
        assert(link->network);
        assert(link->rtnl_messages == 0);

        if (!link->network->addresses)
                return link_enter_addresses_set(link);

        link->state = LINK_STATE_SET_ADDRESSES;

        LIST_FOREACH(addresses, address, link->network->addresses) {
                r = address_configure(address, link, &address_handler);
                if (r < 0)
                        return link_enter_failed(link);

                link->rtnl_messages ++;
        }

        return 0;
}

static int link_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0)
                log_warning("Could not bring up interface '%s': %s",
                            link->ifname, strerror(-r));

        link->flags |= IFF_UP;

        log_info("Link '%s' is up", link->ifname);

        if (link->state == LINK_STATE_ROUTES_SET)
                return link_enter_configured(link);

        return 1;
}

static int link_up(Link *link) {
        _cleanup_sd_rtnl_message_unref_ sd_rtnl_message *req = NULL;
        int r;

        assert(link);
        assert(link->manager);
        assert(link->manager->rtnl);

        r = sd_rtnl_message_link_new(RTM_NEWLINK, link->ifindex, 0, IFF_UP, &req);
        if (r < 0) {
                log_error("Could not allocate RTM_NEWLINK message");
                return r;
        }

        r = sd_rtnl_call_async(link->manager->rtnl, req, link_handler, link, 0, NULL);
        if (r < 0) {
                log_error("Could not send rtnetlink message: %s", strerror(-r));
                return r;
        }

        return 0;
}

static int link_enter_bridge_joined(Link *link) {
        int r;

        r = link_up(link);
        if (r < 0)
                return link_enter_failed(link);

        link->state = LINK_STATE_BRIDGE_JOINED;

        return link_enter_set_addresses(link);
}

static int bridge_handler(sd_rtnl *rtnl, sd_rtnl_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(link->state == LINK_STATE_JOIN_BRIDGE || link->state == LINK_STATE_FAILED);

        if (link->state == LINK_STATE_FAILED)
                return 1;

        r = sd_rtnl_message_get_errno(m);
        if (r < 0)
                log_warning("Could not join interface '%s' to bridge: %s",
                            link->ifname, strerror(-r));

        link_enter_bridge_joined(link);

        return 1;
}

static int link_enter_join_bridge(Link *link) {
        int r;

        assert(link);
        assert(link->network);

        if (!link->network->bridge)
                return link_enter_bridge_joined(link);

        link->state = LINK_STATE_JOIN_BRIDGE;

        r = bridge_join(link->network->bridge, link, &bridge_handler);
        if (r < 0)
                return link_enter_failed(link);

        return 0;
}

int link_configure(Link *link) {
        int r;

        r = link_enter_join_bridge(link);
        if (r < 0)
                return link_enter_failed(link);

        return 0;
}
