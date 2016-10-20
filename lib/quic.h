#pragma once

#include <ev.h>
#include <sys/socket.h>

#include "tommy.h"


/// Represent QUIC tags in a way that lets them be used as integers or
/// printed as strings. These strings are not null-terminated, and therefore
/// need to be printed as @p %.4s with @p printf() or similar.
typedef union {
    uint32_t as_int; ///< QUIC tag in network byte-order.
    char as_str[4];  ///< QUIC tag as non-null-terminated string.
} q_tag;


/// The versions of QUIC supported by this implementation
extern const q_tag vers[];

/// The length of @p vers[] in bytes. Divide by @p sizeof(vers[0]) for number of
/// elements.
extern const size_t vers_len;


struct q_stream {
    uint64_t id;
};


/// A QUIC connection.
struct q_conn {
    node node;

    uint64_t id;   ///< Connection ID
    uint64_t out;  ///< The highest packet number sent on this connection
    uint64_t in;   ///< The highest packet number received on this connection
    uint8_t state; ///< State of the connection.
    uint8_t vers;  ///< QUIC version in use for this connection. (Index into
                   ///< @p vers[].)
    uint8_t _unused[2]; ///< Unused.
    int fd;             ///< File descriptor (socket) for the connection.
    hash streams;
    struct sockaddr peer; ///< Address of our peer.
    socklen_t plen;       ///< Length of @p peer.
    uint8_t _unused2[4];  ///< Unused.
};

#define CLOSED 0
#define VERS_SENT 1
#define VERS_RECV 2
#define ESTABLISHED 3


void q_init(struct ev_loop * restrict const reloop, const long timeout);

void q_connect(struct ev_loop * restrict const loop,
               const int s,
               const struct sockaddr * restrict const peer,
               const socklen_t plen);

void q_serve(struct ev_loop * restrict const loop, const int s);
