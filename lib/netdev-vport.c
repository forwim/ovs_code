/*
 * Copyright (c) 2010, 2011 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "netdev-vport.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include "byte-order.h"
#include "daemon.h"
#include "dirs.h"
#include "dpif-linux.h"
#include "hash.h"
#include "hmap.h"
#include "list.h"
#include "netdev-provider.h"
#include "netlink.h"
#include "netlink-socket.h"
#include "ofpbuf.h"
#include "openvswitch/datapath-protocol.h"
#include "openvswitch/tunnel.h"
#include "packets.h"
#include "route-table.h"
#include "rtnetlink.h"
#include "shash.h"
#include "socket-util.h"
#include "vlog.h"

VLOG_DEFINE_THIS_MODULE(netdev_vport);

struct netdev_vport_notifier {
    struct netdev_notifier notifier;
    struct list list_node;
    struct shash_node *shash_node;
};

struct netdev_dev_vport {
    struct netdev_dev netdev_dev;
    struct ofpbuf *options;
};

struct netdev_vport {
    struct netdev netdev;
};

struct vport_class {
    enum odp_vport_type type;
    struct netdev_class netdev_class;
    int (*parse_config)(const char *name, const char *type,
                        const struct shash *args, struct ofpbuf *options);
    int (*unparse_config)(const char *name, const char *type,
                          const struct nlattr *options, size_t options_len,
                          struct shash *args);
};

static struct shash netdev_vport_notifiers =
                                    SHASH_INITIALIZER(&netdev_vport_notifiers);

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 20);

static int netdev_vport_create(const struct netdev_class *, const char *,
                               const struct shash *, struct netdev_dev **);
static void netdev_vport_poll_notify(const struct netdev *);
static int tnl_port_config_from_nlattr(const struct nlattr *options,
                                       size_t options_len,
                                       struct nlattr *a[ODP_TUNNEL_ATTR_MAX + 1]);

static const char *netdev_vport_get_tnl_iface(const struct netdev *netdev);

static bool
is_vport_class(const struct netdev_class *class)
{
    return class->create == netdev_vport_create;
}

static const struct vport_class *
vport_class_cast(const struct netdev_class *class)
{
    assert(is_vport_class(class));
    return CONTAINER_OF(class, struct vport_class, netdev_class);
}

static struct netdev_dev_vport *
netdev_dev_vport_cast(const struct netdev_dev *netdev_dev)
{
    assert(is_vport_class(netdev_dev_get_class(netdev_dev)));
    return CONTAINER_OF(netdev_dev, struct netdev_dev_vport, netdev_dev);
}

static struct netdev_vport *
netdev_vport_cast(const struct netdev *netdev)
{
    struct netdev_dev *netdev_dev = netdev_get_dev(netdev);
    assert(is_vport_class(netdev_dev_get_class(netdev_dev)));
    return CONTAINER_OF(netdev, struct netdev_vport, netdev);
}

/* If 'netdev' is a vport netdev, returns an ofpbuf that contains Netlink
 * options to include in ODP_VPORT_ATTR_OPTIONS for configuring that vport.
 * Otherwise returns NULL. */
const struct ofpbuf *
netdev_vport_get_options(const struct netdev *netdev)
{
    const struct netdev_dev *dev = netdev_get_dev(netdev);

    return (is_vport_class(netdev_dev_get_class(dev))
            ? netdev_dev_vport_cast(dev)->options
            : NULL);
}

enum odp_vport_type
netdev_vport_get_vport_type(const struct netdev *netdev)
{
    const struct netdev_dev *dev = netdev_get_dev(netdev);
    const struct netdev_class *class = netdev_dev_get_class(dev);

    return (is_vport_class(class) ? vport_class_cast(class)->type
            : class == &netdev_internal_class ? ODP_VPORT_TYPE_INTERNAL
            : class == &netdev_linux_class ? ODP_VPORT_TYPE_NETDEV
            : ODP_VPORT_TYPE_UNSPEC);
}

const char *
netdev_vport_get_netdev_type(const struct dpif_linux_vport *vport)
{
    struct nlattr *a[ODP_TUNNEL_ATTR_MAX + 1];

    switch (vport->type) {
    case ODP_VPORT_TYPE_UNSPEC:
        break;

    case ODP_VPORT_TYPE_NETDEV:
        return "system";

    case ODP_VPORT_TYPE_INTERNAL:
        return "internal";

    case ODP_VPORT_TYPE_PATCH:
        return "patch";

    case ODP_VPORT_TYPE_GRE:
        if (tnl_port_config_from_nlattr(vport->options, vport->options_len,
                                        a)) {
            break;
        }
        return (nl_attr_get_u32(a[ODP_TUNNEL_ATTR_FLAGS]) & TNL_F_IPSEC
                ? "ipsec_gre" : "gre");

    case ODP_VPORT_TYPE_CAPWAP:
        return "capwap";

    case __ODP_VPORT_TYPE_MAX:
        break;
    }

    VLOG_WARN_RL(&rl, "dp%d: port `%s' has unsupported type %u",
                 vport->dp_ifindex, vport->name, (unsigned int) vport->type);
    return "unknown";
}

static int
netdev_vport_create(const struct netdev_class *netdev_class, const char *name,
                    const struct shash *args,
                    struct netdev_dev **netdev_devp)
{
    const struct vport_class *vport_class = vport_class_cast(netdev_class);
    struct ofpbuf *options = NULL;
    struct shash fetched_args;
    int error;

    shash_init(&fetched_args);

    if (!shash_is_empty(args)) {
        /* Parse the provided configuration. */
        options = ofpbuf_new(64);
        error = vport_class->parse_config(name, netdev_class->type,
                                          args, options);
    } else {
        /* Fetch an existing configuration from the kernel.
         *
         * This case could be ambiguous with initializing a new vport with an
         * empty configuration, but none of the existing vport classes accept
         * an empty configuration. */
        struct dpif_linux_vport reply;
        struct ofpbuf *buf;

        error = dpif_linux_vport_get(name, &reply, &buf);
        if (!error) {
            /* XXX verify correct type */
            error = vport_class->unparse_config(name, netdev_class->type,
                                                reply.options,
                                                reply.options_len,
                                                &fetched_args);
            if (error) {
                VLOG_ERR_RL(&rl, "%s: failed to parse kernel config (%s)",
                            name, strerror(error));
            } else {
                options = ofpbuf_clone_data(reply.options, reply.options_len);
            }
            ofpbuf_delete(buf);
        } else {
            VLOG_ERR_RL(&rl, "%s: vport query failed (%s)",
                        name, strerror(error));
        }
    }

    if (!error) {
        struct netdev_dev_vport *dev;

        dev = xmalloc(sizeof *dev);
        netdev_dev_init(&dev->netdev_dev, name,
                        shash_is_empty(&fetched_args) ? args : &fetched_args,
                        netdev_class);
        dev->options = options;

        *netdev_devp = &dev->netdev_dev;
        route_table_register();
    } else {
        ofpbuf_delete(options);
    }

    shash_destroy(&fetched_args);

    return error;
}

static void
netdev_vport_destroy(struct netdev_dev *netdev_dev_)
{
    struct netdev_dev_vport *netdev_dev = netdev_dev_vport_cast(netdev_dev_);

    route_table_unregister();
    free(netdev_dev);
}

static int
netdev_vport_open(struct netdev_dev *netdev_dev_, int ethertype OVS_UNUSED,
                struct netdev **netdevp)
{
    struct netdev_vport *netdev;

    netdev = xmalloc(sizeof *netdev);
    netdev_init(&netdev->netdev, netdev_dev_);

    *netdevp = &netdev->netdev;
    return 0;
}

static void
netdev_vport_close(struct netdev *netdev_)
{
    struct netdev_vport *netdev = netdev_vport_cast(netdev_);
    free(netdev);
}

static int
netdev_vport_set_config(struct netdev_dev *dev_, const struct shash *args)
{
    const struct netdev_class *netdev_class = netdev_dev_get_class(dev_);
    const struct vport_class *vport_class = vport_class_cast(netdev_class);
    struct netdev_dev_vport *dev = netdev_dev_vport_cast(dev_);
    const char *name = netdev_dev_get_name(dev_);
    struct ofpbuf *options;
    int error;

    options = ofpbuf_new(64);
    error = vport_class->parse_config(name, netdev_dev_get_type(dev_),
                                      args, options);
    if (!error
        && (options->size != dev->options->size
            || memcmp(options->data, dev->options->data, options->size))) {
        struct dpif_linux_vport vport;

        dpif_linux_vport_init(&vport);
        vport.cmd = ODP_VPORT_CMD_SET;
        vport.name = name;
        vport.options = options->data;
        vport.options_len = options->size;
        error = dpif_linux_vport_transact(&vport, NULL, NULL);
        if (!error || error == ENODEV) {
            /* Either reconfiguration succeeded or this vport is not installed
             * in the kernel (e.g. it hasn't been added to a dpif yet with
             * dpif_port_add()). */
            ofpbuf_delete(dev->options);
            dev->options = options;
            options = NULL;
            error = 0;
        }
    }
    ofpbuf_delete(options);

    return error;
}

static int
netdev_vport_set_etheraddr(struct netdev *netdev,
                           const uint8_t mac[ETH_ADDR_LEN])
{
    struct dpif_linux_vport vport;
    int error;

    dpif_linux_vport_init(&vport);
    vport.cmd = ODP_VPORT_CMD_SET;
    vport.name = netdev_get_name(netdev);
    vport.address = mac;

    error = dpif_linux_vport_transact(&vport, NULL, NULL);
    if (!error) {
        netdev_vport_poll_notify(netdev);
    }
    return error;
}

static int
netdev_vport_get_etheraddr(const struct netdev *netdev,
                           uint8_t mac[ETH_ADDR_LEN])
{
    struct dpif_linux_vport reply;
    struct ofpbuf *buf;
    int error;

    error = dpif_linux_vport_get(netdev_get_name(netdev), &reply, &buf);
    if (!error) {
        if (reply.address) {
            memcpy(mac, reply.address, ETH_ADDR_LEN);
        } else {
            error = EOPNOTSUPP;
        }
        ofpbuf_delete(buf);
    }
    return error;
}

static int
netdev_vport_get_mtu(const struct netdev *netdev, int *mtup)
{
    struct dpif_linux_vport reply;
    struct ofpbuf *buf;
    int error;

    error = dpif_linux_vport_get(netdev_get_name(netdev), &reply, &buf);
    if (!error) {
        *mtup = reply.mtu;
        ofpbuf_delete(buf);
    }
    return error;
}

int
netdev_vport_get_stats(const struct netdev *netdev, struct netdev_stats *stats)
{
    struct dpif_linux_vport reply;
    struct ofpbuf *buf;
    int error;

    error = dpif_linux_vport_get(netdev_get_name(netdev), &reply, &buf);
    if (error) {
        return error;
    } else if (!reply.stats) {
        ofpbuf_delete(buf);
        return EOPNOTSUPP;
    }

    stats->rx_packets = reply.stats->rx_packets;
    stats->tx_packets = reply.stats->tx_packets;
    stats->rx_bytes = reply.stats->rx_bytes;
    stats->tx_bytes = reply.stats->tx_bytes;
    stats->rx_errors = reply.stats->rx_errors;
    stats->tx_errors = reply.stats->tx_errors;
    stats->rx_dropped = reply.stats->rx_dropped;
    stats->tx_dropped = reply.stats->tx_dropped;
    stats->multicast = reply.stats->multicast;
    stats->collisions = reply.stats->collisions;
    stats->rx_length_errors = reply.stats->rx_length_errors;
    stats->rx_over_errors = reply.stats->rx_over_errors;
    stats->rx_crc_errors = reply.stats->rx_crc_errors;
    stats->rx_frame_errors = reply.stats->rx_frame_errors;
    stats->rx_fifo_errors = reply.stats->rx_fifo_errors;
    stats->rx_missed_errors = reply.stats->rx_missed_errors;
    stats->tx_aborted_errors = reply.stats->tx_aborted_errors;
    stats->tx_carrier_errors = reply.stats->tx_carrier_errors;
    stats->tx_fifo_errors = reply.stats->tx_fifo_errors;
    stats->tx_heartbeat_errors = reply.stats->tx_heartbeat_errors;
    stats->tx_window_errors = reply.stats->tx_window_errors;

    ofpbuf_delete(buf);

    return 0;
}

int
netdev_vport_set_stats(struct netdev *netdev, const struct netdev_stats *stats)
{
    struct rtnl_link_stats64 rtnl_stats;
    struct dpif_linux_vport vport;
    int err;

    rtnl_stats.rx_packets = stats->rx_packets;
    rtnl_stats.tx_packets = stats->tx_packets;
    rtnl_stats.rx_bytes = stats->rx_bytes;
    rtnl_stats.tx_bytes = stats->tx_bytes;
    rtnl_stats.rx_errors = stats->rx_errors;
    rtnl_stats.tx_errors = stats->tx_errors;
    rtnl_stats.rx_dropped = stats->rx_dropped;
    rtnl_stats.tx_dropped = stats->tx_dropped;
    rtnl_stats.multicast = stats->multicast;
    rtnl_stats.collisions = stats->collisions;
    rtnl_stats.rx_length_errors = stats->rx_length_errors;
    rtnl_stats.rx_over_errors = stats->rx_over_errors;
    rtnl_stats.rx_crc_errors = stats->rx_crc_errors;
    rtnl_stats.rx_frame_errors = stats->rx_frame_errors;
    rtnl_stats.rx_fifo_errors = stats->rx_fifo_errors;
    rtnl_stats.rx_missed_errors = stats->rx_missed_errors;
    rtnl_stats.tx_aborted_errors = stats->tx_aborted_errors;
    rtnl_stats.tx_carrier_errors = stats->tx_carrier_errors;
    rtnl_stats.tx_fifo_errors = stats->tx_fifo_errors;
    rtnl_stats.tx_heartbeat_errors = stats->tx_heartbeat_errors;
    rtnl_stats.tx_window_errors = stats->tx_window_errors;

    dpif_linux_vport_init(&vport);
    vport.cmd = ODP_VPORT_CMD_SET;
    vport.name = netdev_get_name(netdev);
    vport.stats = &rtnl_stats;

    err = dpif_linux_vport_transact(&vport, NULL, NULL);

    /* If the vport layer doesn't know about the device, that doesn't mean it
     * doesn't exist (after all were able to open it when netdev_open() was
     * called), it just means that it isn't attached and we'll be getting
     * stats a different way. */
    if (err == ENODEV) {
        err = EOPNOTSUPP;
    }

    return err;
}

static int
netdev_vport_get_status(const struct netdev *netdev, struct shash *sh)
{
    const char *iface = netdev_vport_get_tnl_iface(netdev);

    if (iface) {
        struct netdev *egress_netdev;

        shash_add(sh, "tunnel_egress_iface", xstrdup(iface));

        if (!netdev_open_default(iface, &egress_netdev)) {
            shash_add(sh, "tunnel_egress_iface_carrier",
                      xstrdup(netdev_get_carrier(egress_netdev)
                              ? "up" : "down"));
            netdev_close(egress_netdev);
        }
    }

    return 0;
}

static int
netdev_vport_update_flags(struct netdev *netdev OVS_UNUSED,
                        enum netdev_flags off, enum netdev_flags on OVS_UNUSED,
                        enum netdev_flags *old_flagsp)
{
    if (off & (NETDEV_UP | NETDEV_PROMISC)) {
        return EOPNOTSUPP;
    }

    *old_flagsp = NETDEV_UP | NETDEV_PROMISC;
    return 0;
}

static char *
make_poll_name(const struct netdev *netdev)
{
    return xasprintf("%s:%s", netdev_get_type(netdev), netdev_get_name(netdev));
}

static int
netdev_vport_poll_add(struct netdev *netdev,
                      void (*cb)(struct netdev_notifier *), void *aux,
                      struct netdev_notifier **notifierp)
{
    char *poll_name = make_poll_name(netdev);
    struct netdev_vport_notifier *notifier;
    struct list *list;
    struct shash_node *shash_node;

    shash_node = shash_find(&netdev_vport_notifiers, poll_name);
    if (!shash_node) {
        list = xmalloc(sizeof *list);
        list_init(list);
        shash_node = shash_add(&netdev_vport_notifiers, poll_name, list);
    } else {
        list = shash_node->data;
    }

    notifier = xmalloc(sizeof *notifier);
    netdev_notifier_init(&notifier->notifier, netdev, cb, aux);
    list_push_back(list, &notifier->list_node);
    notifier->shash_node = shash_node;

    *notifierp = &notifier->notifier;
    free(poll_name);

    return 0;
}

static void
netdev_vport_poll_remove(struct netdev_notifier *notifier_)
{
    struct netdev_vport_notifier *notifier =
                CONTAINER_OF(notifier_, struct netdev_vport_notifier, notifier);

    struct list *list;

    list = list_remove(&notifier->list_node);
    if (list_is_empty(list)) {
        shash_delete(&netdev_vport_notifiers, notifier->shash_node);
        free(list);
    }

    free(notifier);
}

static void
netdev_vport_run(void)
{
    route_table_run();
}

static void
netdev_vport_wait(void)
{
    route_table_wait();
}

/* get_tnl_iface() implementation. */
static const char *
netdev_vport_get_tnl_iface(const struct netdev *netdev)
{
    struct nlattr *a[ODP_TUNNEL_ATTR_MAX + 1];
    uint32_t route;
    struct netdev_dev_vport *ndv;
    static char name[IFNAMSIZ];

    ndv = netdev_dev_vport_cast(netdev_get_dev(netdev));
    if (tnl_port_config_from_nlattr(ndv->options->data, ndv->options->size,
                                    a)) {
        return NULL;
    }
    route = nl_attr_get_be32(a[ODP_TUNNEL_ATTR_DST_IPV4]);

    if (route_table_get_name(route, name)) {
        return name;
    }

    return NULL;
}

/* Helper functions. */

static void
netdev_vport_poll_notify(const struct netdev *netdev)
{
    char *poll_name = make_poll_name(netdev);
    struct list *list = shash_find_data(&netdev_vport_notifiers,
                                        poll_name);

    if (list) {
        struct netdev_vport_notifier *notifier;

        LIST_FOR_EACH (notifier, list_node, list) {
            struct netdev_notifier *n = &notifier->notifier;
            n->cb(n);
        }
    }

    free(poll_name);
}

/* Code specific to individual vport types. */

static void
set_key(const struct shash *args, const char *name, uint16_t type,
        struct ofpbuf *options)
{
    const char *s;

    s = shash_find_data(args, name);
    if (!s) {
        s = shash_find_data(args, "key");
        if (!s) {
            s = "0";
        }
    }

    if (!strcmp(s, "flow")) {
        /* This is the default if no attribute is present. */
    } else {
        nl_msg_put_be64(options, type, htonll(strtoull(s, NULL, 0)));
    }
}

static int
parse_tunnel_config(const char *name, const char *type,
                    const struct shash *args, struct ofpbuf *options)
{
    bool is_gre = false;
    bool is_ipsec = false;
    struct shash_node *node;
    bool ipsec_mech_set = false;
    ovs_be32 daddr = htonl(0);
    uint32_t flags;

    flags = TNL_F_PMTUD | TNL_F_HDR_CACHE;
    if (!strcmp(type, "gre")) {
        is_gre = true;
    } else if (!strcmp(type, "ipsec_gre")) {
        is_gre = true;
        is_ipsec = true;
        flags |= TNL_F_IPSEC;
        flags &= ~TNL_F_HDR_CACHE;
    }

    SHASH_FOR_EACH (node, args) {
        if (!strcmp(node->name, "remote_ip")) {
            struct in_addr in_addr;
            if (lookup_ip(node->data, &in_addr)) {
                VLOG_WARN("%s: bad %s 'remote_ip'", name, type);
            } else {
                daddr = in_addr.s_addr;
            }
        } else if (!strcmp(node->name, "local_ip")) {
            struct in_addr in_addr;
            if (lookup_ip(node->data, &in_addr)) {
                VLOG_WARN("%s: bad %s 'local_ip'", name, type);
            } else {
                nl_msg_put_be32(options, ODP_TUNNEL_ATTR_SRC_IPV4,
                                in_addr.s_addr);
            }
        } else if (!strcmp(node->name, "tos")) {
            if (!strcmp(node->data, "inherit")) {
                flags |= TNL_F_TOS_INHERIT;
            } else {
                nl_msg_put_u8(options, ODP_TUNNEL_ATTR_TOS, atoi(node->data));
            }
        } else if (!strcmp(node->name, "ttl")) {
            if (!strcmp(node->data, "inherit")) {
                flags |= TNL_F_TTL_INHERIT;
            } else {
                nl_msg_put_u8(options, ODP_TUNNEL_ATTR_TTL, atoi(node->data));
            }
        } else if (!strcmp(node->name, "csum") && is_gre) {
            if (!strcmp(node->data, "true")) {
                flags |= TNL_F_CSUM;
            }
        } else if (!strcmp(node->name, "pmtud")) {
            if (!strcmp(node->data, "false")) {
                flags &= ~TNL_F_PMTUD;
            }
        } else if (!strcmp(node->name, "header_cache")) {
            if (!strcmp(node->data, "false")) {
                flags &= ~TNL_F_HDR_CACHE;
            }
        } else if (!strcmp(node->name, "peer_cert") && is_ipsec) {
            if (shash_find(args, "certificate")) {
                ipsec_mech_set = true;
            } else {
                const char *use_ssl_cert;

                /* If the "use_ssl_cert" is true, then "certificate" and
                 * "private_key" will be pulled from the SSL table.  The
                 * use of this option is strongly discouraged, since it
                 * will like be removed when multiple SSL configurations
                 * are supported by OVS.
                 */
                use_ssl_cert = shash_find_data(args, "use_ssl_cert");
                if (!use_ssl_cert || strcmp(use_ssl_cert, "true")) {
                    VLOG_ERR("%s: 'peer_cert' requires 'certificate' argument",
                             name);
                    return EINVAL;
                }
                ipsec_mech_set = true;
            }
        } else if (!strcmp(node->name, "psk") && is_ipsec) {
            ipsec_mech_set = true;
        } else if (is_ipsec
                && (!strcmp(node->name, "certificate")
                    || !strcmp(node->name, "private_key")
                    || !strcmp(node->name, "use_ssl_cert"))) {
            /* Ignore options not used by the netdev. */
        } else if (is_gre && (!strcmp(node->name, "key") ||
                              !strcmp(node->name, "in_key") ||
                              !strcmp(node->name, "out_key"))) {
            /* Handled separately below. */
        } else {
            VLOG_WARN("%s: unknown %s argument '%s'", name, type, node->name);
        }
    }

    if (is_ipsec) {
        char *file_name = xasprintf("%s/%s", ovs_rundir(),
                "ovs-monitor-ipsec.pid");
        pid_t pid = read_pidfile(file_name);
        free(file_name);
        if (pid < 0) {
            VLOG_ERR("%s: IPsec requires the ovs-monitor-ipsec daemon",
                     name);
            return EINVAL;
        }

        if (shash_find(args, "peer_cert") && shash_find(args, "psk")) {
            VLOG_ERR("%s: cannot define both 'peer_cert' and 'psk'", name);
            return EINVAL;
        }

        if (!ipsec_mech_set) {
            VLOG_ERR("%s: IPsec requires an 'peer_cert' or psk' argument",
                     name);
            return EINVAL;
        }
    }

    if (is_gre) {
        set_key(args, "in_key", ODP_TUNNEL_ATTR_IN_KEY, options);
        set_key(args, "out_key", ODP_TUNNEL_ATTR_OUT_KEY, options);
    }

    if (!daddr) {
        VLOG_ERR("%s: %s type requires valid 'remote_ip' argument",
                 name, type);
        return EINVAL;
    }
    nl_msg_put_be32(options, ODP_TUNNEL_ATTR_DST_IPV4, daddr);

    nl_msg_put_u32(options, ODP_TUNNEL_ATTR_FLAGS, flags);

    return 0;
}

static int
tnl_port_config_from_nlattr(const struct nlattr *options, size_t options_len,
                            struct nlattr *a[ODP_TUNNEL_ATTR_MAX + 1])
{
    static const struct nl_policy odp_tunnel_policy[] = {
        [ODP_TUNNEL_ATTR_FLAGS] = { .type = NL_A_U32 },
        [ODP_TUNNEL_ATTR_DST_IPV4] = { .type = NL_A_BE32 },
        [ODP_TUNNEL_ATTR_SRC_IPV4] = { .type = NL_A_BE32, .optional = true },
        [ODP_TUNNEL_ATTR_IN_KEY] = { .type = NL_A_BE64, .optional = true },
        [ODP_TUNNEL_ATTR_OUT_KEY] = { .type = NL_A_BE64, .optional = true },
        [ODP_TUNNEL_ATTR_TOS] = { .type = NL_A_U8, .optional = true },
        [ODP_TUNNEL_ATTR_TTL] = { .type = NL_A_U8, .optional = true },
    };
    struct ofpbuf buf;

    ofpbuf_use_const(&buf, options, options_len);
    if (!nl_policy_parse(&buf, 0, odp_tunnel_policy,
                         a, ARRAY_SIZE(odp_tunnel_policy))) {
        return EINVAL;
    }
    return 0;
}

static uint64_t
get_be64_or_zero(const struct nlattr *a)
{
    return a ? ntohll(nl_attr_get_be64(a)) : 0;
}

static int
unparse_tunnel_config(const char *name OVS_UNUSED, const char *type OVS_UNUSED,
                      const struct nlattr *options, size_t options_len,
                      struct shash *args)
{
    struct nlattr *a[ODP_TUNNEL_ATTR_MAX + 1];
    ovs_be32 daddr;
    uint32_t flags;
    int error;

    error = tnl_port_config_from_nlattr(options, options_len, a);
    if (error) {
        return error;
    }

    flags = nl_attr_get_u32(a[ODP_TUNNEL_ATTR_FLAGS]);
    if (!(flags & TNL_F_HDR_CACHE) == !(flags & TNL_F_IPSEC)) {
        smap_add(args, "header_cache",
                 flags & TNL_F_HDR_CACHE ? "true" : "false");
    }

    daddr = nl_attr_get_be32(a[ODP_TUNNEL_ATTR_DST_IPV4]);
    shash_add(args, "remote_ip", xasprintf(IP_FMT, IP_ARGS(&daddr)));

    if (a[ODP_TUNNEL_ATTR_SRC_IPV4]) {
        ovs_be32 saddr = nl_attr_get_be32(a[ODP_TUNNEL_ATTR_SRC_IPV4]);
        shash_add(args, "local_ip", xasprintf(IP_FMT, IP_ARGS(&saddr)));
    }

    if (!a[ODP_TUNNEL_ATTR_IN_KEY] && !a[ODP_TUNNEL_ATTR_OUT_KEY]) {
        smap_add(args, "key", "flow");
    } else {
        uint64_t in_key = get_be64_or_zero(a[ODP_TUNNEL_ATTR_IN_KEY]);
        uint64_t out_key = get_be64_or_zero(a[ODP_TUNNEL_ATTR_OUT_KEY]);

        if (in_key && in_key == out_key) {
            shash_add(args, "key", xasprintf("%"PRIu64, in_key));
        } else {
            if (!a[ODP_TUNNEL_ATTR_IN_KEY]) {
                smap_add(args, "in_key", "flow");
            } else if (in_key) {
                shash_add(args, "in_key", xasprintf("%"PRIu64, in_key));
            }

            if (!a[ODP_TUNNEL_ATTR_OUT_KEY]) {
                smap_add(args, "out_key", "flow");
            } else if (out_key) {
                shash_add(args, "out_key", xasprintf("%"PRIu64, out_key));
            }
        }
    }

    if (flags & TNL_F_TTL_INHERIT) {
        smap_add(args, "tos", "inherit");
    } else if (a[ODP_TUNNEL_ATTR_TTL]) {
        int ttl = nl_attr_get_u8(a[ODP_TUNNEL_ATTR_TTL]);
        shash_add(args, "tos", xasprintf("%d", ttl));
    }

    if (flags & TNL_F_TOS_INHERIT) {
        smap_add(args, "tos", "inherit");
    } else if (a[ODP_TUNNEL_ATTR_TOS]) {
        int tos = nl_attr_get_u8(a[ODP_TUNNEL_ATTR_TOS]);
        shash_add(args, "tos", xasprintf("%d", tos));
    }

    if (flags & TNL_F_CSUM) {
        smap_add(args, "csum", "true");
    }
    if (!(flags & TNL_F_PMTUD)) {
        smap_add(args, "pmtud", "false");
    }

    return 0;
}

static int
parse_patch_config(const char *name, const char *type OVS_UNUSED,
                   const struct shash *args, struct ofpbuf *options)
{
    const char *peer;

    peer = shash_find_data(args, "peer");
    if (!peer) {
        VLOG_ERR("%s: patch type requires valid 'peer' argument", name);
        return EINVAL;
    }

    if (shash_count(args) > 1) {
        VLOG_ERR("%s: patch type takes only a 'peer' argument", name);
        return EINVAL;
    }

    if (strlen(peer) >= IFNAMSIZ) {
        VLOG_ERR("%s: patch 'peer' arg too long", name);
        return EINVAL;
    }

    if (!strcmp(name, peer)) {
        VLOG_ERR("%s: patch peer must not be self", name);
        return EINVAL;
    }

    nl_msg_put_string(options, ODP_PATCH_ATTR_PEER, peer);

    return 0;
}

static int
unparse_patch_config(const char *name OVS_UNUSED, const char *type OVS_UNUSED,
                     const struct nlattr *options, size_t options_len,
                     struct shash *args)
{
    static const struct nl_policy odp_patch_policy[] = {
        [ODP_PATCH_ATTR_PEER] = { .type = NL_A_STRING,
                               .max_len = IFNAMSIZ,
                               .optional = false }
    };

    struct nlattr *a[ARRAY_SIZE(odp_patch_policy)];
    struct ofpbuf buf;

    ofpbuf_use_const(&buf, options, options_len);
    if (!nl_policy_parse(&buf, 0, odp_patch_policy,
                         a, ARRAY_SIZE(odp_patch_policy))) {
        return EINVAL;
    }

    smap_add(args, "peer", nl_attr_get_string(a[ODP_PATCH_ATTR_PEER]));
    return 0;
}

#define VPORT_FUNCTIONS(GET_STATUS)                         \
    NULL,                                                   \
    netdev_vport_run,                                       \
    netdev_vport_wait,                                      \
                                                            \
    netdev_vport_create,                                    \
    netdev_vport_destroy,                                   \
    netdev_vport_set_config,                                \
                                                            \
    netdev_vport_open,                                      \
    netdev_vport_close,                                     \
                                                            \
    NULL,                       /* enumerate */             \
                                                            \
    NULL,                       /* recv */                  \
    NULL,                       /* recv_wait */             \
    NULL,                       /* drain */                 \
                                                            \
    NULL,                       /* send */                  \
    NULL,                       /* send_wait */             \
                                                            \
    netdev_vport_set_etheraddr,                             \
    netdev_vport_get_etheraddr,                             \
    netdev_vport_get_mtu,                                   \
    NULL,                       /* get_ifindex */           \
    NULL,                       /* get_carrier */           \
    NULL,                       /* get_miimon */            \
    netdev_vport_get_stats,                                 \
    netdev_vport_set_stats,                                 \
                                                            \
    NULL,                       /* get_features */          \
    NULL,                       /* set_advertisements */    \
    NULL,                       /* get_vlan_vid */          \
                                                            \
    NULL,                       /* set_policing */          \
    NULL,                       /* get_qos_types */         \
    NULL,                       /* get_qos_capabilities */  \
    NULL,                       /* get_qos */               \
    NULL,                       /* set_qos */               \
    NULL,                       /* get_queue */             \
    NULL,                       /* set_queue */             \
    NULL,                       /* delete_queue */          \
    NULL,                       /* get_queue_stats */       \
    NULL,                       /* dump_queues */           \
    NULL,                       /* dump_queue_stats */      \
                                                            \
    NULL,                       /* get_in4 */               \
    NULL,                       /* set_in4 */               \
    NULL,                       /* get_in6 */               \
    NULL,                       /* add_router */            \
    NULL,                       /* get_next_hop */          \
    GET_STATUS,                                             \
    NULL,                       /* arp_lookup */            \
                                                            \
    netdev_vport_update_flags,                              \
                                                            \
    netdev_vport_poll_add,                                  \
    netdev_vport_poll_remove,

void
netdev_vport_register(void)
{
    static const struct vport_class vport_classes[] = {
        { ODP_VPORT_TYPE_GRE,
          { "gre", VPORT_FUNCTIONS(netdev_vport_get_status) },
          parse_tunnel_config, unparse_tunnel_config },

        { ODP_VPORT_TYPE_GRE,
          { "ipsec_gre", VPORT_FUNCTIONS(netdev_vport_get_status) },
          parse_tunnel_config, unparse_tunnel_config },

        { ODP_VPORT_TYPE_CAPWAP,
          { "capwap", VPORT_FUNCTIONS(netdev_vport_get_status) },
          parse_tunnel_config, unparse_tunnel_config },

        { ODP_VPORT_TYPE_PATCH,
          { "patch", VPORT_FUNCTIONS(NULL) },
          parse_patch_config, unparse_patch_config }
    };

    int i;

    for (i = 0; i < ARRAY_SIZE(vport_classes); i++) {
        netdev_register_provider(&vport_classes[i].netdev_class);
    }
}
