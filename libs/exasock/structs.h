#ifndef EXASOCK_STRUCTS_H
#define EXASOCK_STRUCTS_H

/*
 * Be careful adding/removing/reordering entries. There are some parts of
 * the code that rely on this enum going (approximately) from least
 * available to most available.
 */
enum exa_bypass_state
{
    /* permanently disabled - cannot leave this state */
    EXA_BYPASS_DISABLED,

    /* disabled, but not permanently (e.g. user can enable). The main use
     * case for this is "global default disable" in exasock.
     */
    EXA_BYPASS_INACTIVE,

    /* available, but not yet active */
    EXA_BYPASS_AVAIL,

    /* This socket is currently being used for bypass. This state is
       (currently) only left when closing a socket. */
    EXA_BYPASS_ACTIVE
};

struct exa_endpoint_ipaddr
{
    in_addr_t local;
    in_addr_t peer;
};

struct exa_endpoint_port
{
    in_port_t local;
    in_port_t peer;
};

struct exa_endpoint
{
    struct exa_endpoint_ipaddr addr;
    struct exa_endpoint_port port;
};

struct exa_mcast_endpoint
{
    in_addr_t multiaddr;
    in_addr_t interface;
};

struct exa_mcast_membership
{
    struct exa_mcast_endpoint mcast_ep;
    bool mcast_ep_valid;
    unsigned int num_not_bypassed;
};

struct exa_timestamp
{
    uint32_t sec;
    uint32_t nsec;
};

struct exa_timeo
{
    bool enabled;
    struct timeval val;
};

/* This struct is input for exa_tcp_accept() */
struct exa_tcp_init_state
{
    uint32_t local_seq;
    uint32_t peer_seq;
    uint16_t peer_window;
    uint16_t peer_mss;
    uint8_t peer_wscale;
};

#define MAX_NUM_EPOLL   4

/* NOTE: Update exa_socket_zero() if struct layout is changed! */
struct exa_socket
{
    /* Read lock - all operations that use the socket
     * Write lock - creating/destroying socket data structures */
    exa_rwlock_t lock;

    /* Incremented whenever a socket is cleared so that blocking functions
     * can detect that the socket has changed */
    int gen_id;

    /* These fields mirror the native socket settings */
    int domain;
    int type;
    int protocol;
    int flags;

    /* Bypass/bound state */
    enum exa_bypass_state bypass_state;
    bool bound;
    bool connected;
    bool valid;

    /* ATE Connection ID (-1 if ATE not used) */
    int ate_id;
    bool ate_init_pending;

    /* Bound to specific device with SO_BINDTODEVICE */
    bool bound_to_device;

    /* Interfaces the socket is listening on.
     * If bound to all interfaces, listen.all_if is true and listen.interface
     * is NULL.
     * If bound to a single interface, listen.all_if is false and
     * listen.interface is not NULL.
     * If listening on an interface with which a multicast group has been
     * joined, listen.mcast is true. */
    struct {
        struct exanic_ip *interface;
        bool all_if;
        bool mcast;
    } listen;

    /* Protocol context structs */
    union {
        struct exanic_udp *udp;
        struct exanic_tcp *tcp;
    } ctx;

    /* Cannot change bound endpoint after insertion into hash table */
    union {
        struct exa_endpoint ip;
    } bind;

    /* For chaining hash table entries */
    int hashtable_next_fd;

    /* For chaining socket list entries */
    struct exa_socket *list_prev;
    struct exa_socket *list_next;

    /* Socket state shared by all copies of this fd */
    struct exa_socket_state *state;

    /* Protocol specific buffers shared by all copies of this fd */
    char *rx_buffer;
    char *tx_buffer;

    /* Local copy of the socket ready state, used for edge detection */
    bool rx_ready;
    bool tx_ready;
    bool eof_ready;

    /* Socket is of a type that needs to be polled for read readiness
     * (TCP only) */
    bool need_rx_ready_poll;

    /* Socket options */
    in_addr_t ip_multicast_if;
    unsigned char ip_multicast_ttl;
    struct exa_mcast_membership ip_membership;
    struct linger so_linger;
    bool so_timestamp;
    bool so_timestampns;
    int so_timestamping;
    struct exa_timeo so_sndtimeo;
    struct exa_timeo so_rcvtimeo;
    bool so_keepalive;
    unsigned tcp_user_timeout;
    int tcp_keepcnt;
    int tcp_keepidle;
    int tcp_keepintvl;

    /* Timestamp generation enable state */
    bool rx_sw_timestamp;

    /* Timestamp reporting enable state */
    bool report_timestamp;

    /* Record of epoll instance membership for non-bypass sockets
     * To be removed when a socket is put into bypass mode */
    unsigned num_epoll_fd;
    int epoll_fd[MAX_NUM_EPOLL];

    /* Non-null if this is an epoll file descriptor */
    struct exa_notify *notify;

    /* Membership of exa_notify instances */
    struct exa_notify *notify_parent;

    /* Membership status of notify_parent's kernel epoll instance */
    bool kern_epoll_member;

    /* Warnings tracking */
    struct {
        bool mcast_bound;
        bool so_sndbuf;
        bool so_rcvbuf;
    } warn;
};

#define EXA_USE_ATE(sock)   ((sock)->ate_id != -1)

#define EXA_HASHTABLE_SIZE_LOG2 16

struct exa_hashtable_key
{
    in_addr_t addr[2];
    in_port_t port[2];
};

struct exa_hashtable
{
    int table[1 << EXA_HASHTABLE_SIZE_LOG2];

    /* Write lock is held when modifying the hash table.
     * Hash table lookups are lockfree */
    uint32_t write_lock;
};

struct exa_socket_list
{
    struct exa_socket *head;
};

extern struct exa_socket *exa_socket_table;
extern size_t exa_socket_table_size;

struct exasock_poll_sync
{
    /* Poll lock protects the hardware rx buffer and related structs
     * The holder of the lock must poll for new packets in the rx buffer */
    uint32_t lock;
    /* code wishing to delete a socket increments "reclaim_req"
     * exanic_poll sets reclaim_ack to reclaim_req, indicating the
     * end of read critical section */
    uint32_t reclaim_req;
    uint32_t reclaim_ack;
};

extern struct exasock_poll_sync exasock_poll_sync;

#define exasock_poll_lock           (exasock_poll_sync.lock)
#define exasock_poll_reclaim_req    (exasock_poll_sync.reclaim_req)
#define exasock_poll_reclaim_ack    (exasock_poll_sync.reclaim_ack)

static inline void
exa_socket_reclaim_sync(void)
{
    uint32_t req;

    do
        req = exasock_poll_reclaim_req;
    while (!__sync_bool_compare_and_swap(&exasock_poll_reclaim_req,
                                         req, req + 1));

    /* if either no thread is holding the poll lock
     * or if the current iteration of exanic_poll has finished
     * then the socket is safe to reclaim */
    while (exasock_poll_lock &&
           (int32_t)(exasock_poll_reclaim_ack - req) <= 0)
        sched_yield();
}

static inline struct exa_socket *
exa_socket_get(int fd)
{
    if (fd < 0 || fd >= exa_socket_table_size)
        return NULL;
    else
        return exa_socket_table + fd;
}

static inline int
exa_socket_fd(struct exa_socket *sock)
{
    return sock - exa_socket_table;
}

static inline void
exa_hashtable_init(struct exa_hashtable * restrict ht)
{
    memset(ht->table, -1, sizeof(ht->table));
    ht->write_lock = 0;
}

static inline uint32_t
exa_hashtable_hash(struct exa_hashtable_key * restrict key)
{
    uint32_t a = key->addr[0];
    uint32_t b = key->addr[1];
    uint32_t c = ((uint32_t)key->port[0] << 16) | key->port[1];

    /* Based on final stage of the lookup3 hash function by Bob Jenkins.
     * http://burtleburtle.net/bob/c/lookup3.c */
#define rot(x, k) (((x) << (k)) | ((x) >> (32-(k))))
    c ^= b; c -= rot(b, 14);
    a ^= c; a -= rot(c, 11);
    b ^= a; b -= rot(a, 25);
    c ^= b; c -= rot(b, 16);
    a ^= c; a -= rot(c, 4);
    b ^= a; b -= rot(a, 14);
    c ^= b; c -= rot(b, 24);
#undef rot

    return c;
}

#define EXA_HASHTABLE_IDX(key)   \
                (exa_hashtable_hash(key) & ((1 << EXA_HASHTABLE_SIZE_LOG2) - 1))

static inline void
exa_hashtable_insert(struct exa_hashtable * restrict ht,
                     struct exa_socket * restrict sock, int fd,
                     struct exa_hashtable_key * restrict key)
{
    uint32_t idx = EXA_HASHTABLE_IDX(key);

    exa_lock(&ht->write_lock);
    sock->hashtable_next_fd = ht->table[idx];
    ht->table[idx] = fd;
    exa_unlock(&ht->write_lock);
}

static inline bool
exa_hashtable_remove(struct exa_hashtable * restrict ht,
                     struct exa_socket * restrict remove_sock, int remove_fd,
                     struct exa_hashtable_key * restrict key)
{
    uint32_t idx = EXA_HASHTABLE_IDX(key);
    int fd;
    bool ret = false;

    exa_lock(&ht->write_lock);

    if (ht->table[idx] == remove_fd)
    {
        ht->table[idx] = remove_sock->hashtable_next_fd;
        ret = true;
        goto ht_unlock;
    }

    fd = ht->table[idx];
    while (fd != -1)
    {
        struct exa_socket * restrict sock = exa_socket_get(fd);
        if (sock->hashtable_next_fd == remove_fd)
        {
            sock->hashtable_next_fd = remove_sock->hashtable_next_fd;
            ret = true;
            goto ht_unlock;
        }
    }

ht_unlock:
    exa_unlock(&ht->write_lock);
    return ret;
}

static inline void
exa_hashtable_ucast_insert(struct exa_hashtable * restrict ht, int fd)
{
    struct exa_socket * restrict sock = exa_socket_get(fd);
    struct exa_hashtable_key key;

    key.addr[0] = sock->bind.ip.addr.local;
    key.addr[1] = sock->bind.ip.addr.peer;
    key.port[0] = sock->bind.ip.port.local;
    key.port[1] = sock->bind.ip.port.peer;

    exa_hashtable_insert(ht, sock, fd, &key);
}

static inline bool
exa_hashtable_ucast_remove(struct exa_hashtable * restrict ht, int fd)
{
    struct exa_socket * restrict sock = exa_socket_get(fd);
    struct exa_hashtable_key key;

    key.addr[0] = sock->bind.ip.addr.local;
    key.addr[1] = sock->bind.ip.addr.peer;
    key.port[0] = sock->bind.ip.port.local;
    key.port[1] = sock->bind.ip.port.peer;

    return exa_hashtable_remove(ht, sock, fd, &key);
}

static inline void
exa_hashtable_mcast_insert(struct exa_hashtable * restrict ht, int fd,
                           struct exa_mcast_endpoint * restrict mc_ep)
{
    struct exa_socket * restrict sock = exa_socket_get(fd);
    struct exa_hashtable_key key;

    /* For multicast entries peer addr is not used as a hash key
     * and interface address is used instead. It is fine as long as
     * IP_ADD_SOURCE_MEMBERSHIP socket option is not supported.
     */
    key.addr[0] = mc_ep->multiaddr;
    key.addr[1] = mc_ep->interface;
    key.port[0] = sock->bind.ip.port.local;
    key.port[1] = 0;

    exa_hashtable_insert(ht, sock, fd, &key);
}

static inline bool
exa_hashtable_mcast_remove(struct exa_hashtable * restrict ht, int fd,
                           struct exa_mcast_endpoint * restrict mc_ep)
{
    struct exa_socket * restrict sock = exa_socket_get(fd);
    struct exa_hashtable_key key;

    key.addr[0] = mc_ep->multiaddr;
    key.addr[1] = mc_ep->interface;
    key.port[0] = sock->bind.ip.port.local;
    key.port[1] = 0;

    return exa_hashtable_remove(ht, sock, fd, &key);
}

static inline int
exa_hashtable_ucast_lookup(struct exa_hashtable * restrict ht,
                          struct exa_endpoint * restrict e)
{
    struct exa_hashtable_key key;
    uint32_t idx;
    int fd;

    key.addr[0] = e->addr.local;
    key.addr[1] = e->addr.peer;
    key.port[0] = e->port.local;
    key.port[1] = e->port.peer;

    /* Look up by (local addr, local port, peer addr, peer port) */
    idx = EXA_HASHTABLE_IDX(&key);
    fd = ht->table[idx];
    while (fd != -1)
    {
        struct exa_socket * restrict sock = exa_socket_get(fd);

        if (sock->bind.ip.addr.local == e->addr.local &&
            sock->bind.ip.addr.peer == e->addr.peer &&
            sock->bind.ip.port.local == e->port.local &&
            sock->bind.ip.port.peer == e->port.peer)
            return fd;

        fd = sock->hashtable_next_fd;
    }

    /* Look up by (local addr, local port) */
    key.addr[1] = htonl(INADDR_ANY);    /* peer addr */
    key.port[1] = 0;                    /* peer port */
    idx = EXA_HASHTABLE_IDX(&key);
    fd = ht->table[idx];
    while (fd != -1)
    {
        struct exa_socket * restrict sock = exa_socket_get(fd);

        if (sock->bind.ip.addr.local == e->addr.local &&
            sock->bind.ip.addr.peer == htonl(INADDR_ANY) &&
            sock->bind.ip.port.local == e->port.local &&
            sock->bind.ip.port.peer == 0)
            return fd;

        fd = sock->hashtable_next_fd;
    }

    /* Look up by local port only */
    key.addr[0] = htonl(INADDR_ANY);    /* local addr */
    idx = EXA_HASHTABLE_IDX(&key);
    fd = ht->table[idx];
    while (fd != -1)
    {
        struct exa_socket * restrict sock = exa_socket_get(fd);

        if (sock->bind.ip.addr.local == htonl(INADDR_ANY) &&
            sock->bind.ip.addr.peer == htonl(INADDR_ANY) &&
            sock->bind.ip.port.local == e->port.local &&
            sock->bind.ip.port.peer == 0)
            return fd;

        fd = sock->hashtable_next_fd;
    }

    return -1;
}

static inline int
exa_hashtable_mcast_lookup(struct exa_hashtable * restrict ht,
                           struct exa_endpoint * restrict e,
                           in_addr_t if_addr)
{
    struct exa_hashtable_key key;
    uint32_t idx;
    int fd;

    /* For multicast entries peer addr is not used as a hash key
     * and interface address is used instead. It is fine as long as
     * IP_ADD_SOURCE_MEMBERSHIP socket option is not supported.
     */
    key.addr[0] = e->addr.local;    /* multicast address */
    key.addr[1] = if_addr;          /* interface address */
    key.port[0] = e->port.local;    /* local port */
    key.port[1] = 0;                /* not used */

    /* Look up by (multicast addr, local port, interface addr) */
    idx = EXA_HASHTABLE_IDX(&key);
    fd = ht->table[idx];
    while (fd != -1)
    {
        struct exa_socket * restrict sock = exa_socket_get(fd);

        if (sock->bind.ip.port.local == e->port.local &&
            sock->bind.ip.port.peer == 0 &&
            sock->ip_membership.mcast_ep.multiaddr == e->addr.local &&
            sock->ip_membership.mcast_ep.interface == if_addr)
                return fd;

        fd = sock->hashtable_next_fd;
    }

    /* Look up by (multicast addr, local port) */
    key.addr[1] = htonl(INADDR_ANY);    /* interface address */
    idx = EXA_HASHTABLE_IDX(&key);
    fd = ht->table[idx];
    while (fd != -1)
    {
        struct exa_socket * restrict sock = exa_socket_get(fd);

        if (sock->bind.ip.port.local == e->port.local &&
            sock->bind.ip.port.peer == 0 &&
            sock->ip_membership.mcast_ep.multiaddr == e->addr.local &&
            sock->ip_membership.mcast_ep.interface == htonl(INADDR_ANY))
                return fd;

        fd = sock->hashtable_next_fd;
    }

    return -1;
}

#endif /* EXASOCK_STRUCTS_H */
