// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2016-2019, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <fcntl.h>
#include <inttypes.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef __linux__
#include <sys/types.h>
#endif

#include <http_parser.h>

#include <quant/quant.h>
#include <warpcore/warpcore.h>

struct q_conn;


static void __attribute__((noreturn)) usage(const char * const name,
                                            const char * const ifname,
                                            const uint16_t port,
                                            const char * const dir,
                                            const char * const cert,
                                            const char * const key,
                                            const uint64_t timeout,
                                            const uint64_t num_bufs)
{
    printf("%s [options]\n", name);
    printf("\t[-i interface]\tinterface to run over; default %s\n", ifname);
    printf("\t[-p port]\tdestination port; default %d\n", port);
    printf("\t[-d dir]\tserver root directory; default %s\n", dir);
    printf("\t[-c cert]\tTLS certificate; default %s\n", cert);
    printf("\t[-k key]\tTLS key; default %s\n", key);
    printf("\t[-t timeout]\tidle timeout in seconds; default %" PRIu64 "\n",
           timeout);
    printf(
        "\t[-b bufs]\tnumber of network buffers to allocate; default %" PRIu64
        "\n",
        num_bufs);
#ifndef NDEBUG
    printf("\t[-v verbosity]\tverbosity level (0-%d, default %d)\n", DLEVEL,
           util_dlevel);
#endif
    exit(0);
}


struct cb_data {
    struct q_stream * s;
    struct q_conn * c;
    struct w_engine * w;
    int dir;
    uint32_t _dummy;
};


static bool send_err(const struct cb_data * const d, const uint16_t code)
{
    const char * msg;
    bool close = false;

    switch (code) {
    case 400:
        msg = "400 Bad Request";
        close = true;
        break;
    case 403:
        msg = "403 Forbidden";
        break;
    case 404:
        msg = "404 Not Found";
        break;
    case 505:
        msg = "505 HTTP Version Not Supported";
        close = true;
        break;
    default:
        msg = "500 Internal Server Error";
    }

    if (close)
        q_close(d->c, 0x0003, msg);
    else
        q_write_str(d->w, d->s, msg, strlen(msg), true);
    return close;
}


static int serve_cb(http_parser * parser, const char * at, size_t len)
{
    (void)parser;
    const struct cb_data * const d = parser->data;
    warn(INF, "conn %s str %" PRId64 " serving URL %.*s", q_cid(d->c),
         q_sid(d->s), (int)len, at);

    char path[MAXPATHLEN] = ".";
    strncpy(&path[*at == '/' ? 1 : 0], at, MIN(len, sizeof(path) - 1));

    // hacky way to prevent directory traversals
    if (strstr(path, ".."))
        return send_err(d, 403);

    // check if this is a "GET /n" request for random data
    const uint64_t n = (uint32_t)MIN(UINT64_MAX, strtoul(&path[2], 0, 10));
    if (n) {
        struct w_iov_sq out = w_iov_sq_initializer(out);
        q_alloc(d->w, &out, n);
        // check whether we managed to allow enough buffers
        if (w_iov_sq_len(&out) != n) {
            warn(ERR,
                 "could only allocate %" PRIu64 "/%" PRIu64 " bytes of buffer",
                 w_iov_sq_len(&out), n);
            q_free(&out);
            return send_err(d, 500);
        }

        // randomize data
        struct w_iov * v = 0;
        char c = 'A';
        sq_foreach (v, &out, next) {
            memset(v->buf, c, v->len);
            c = (char)(c == 'Z' ? 'A' : c + 1);
        }
        q_write(d->s, &out, true);
        q_free(&out);
        return 0;
    }

    struct stat info;
    if (fstatat(d->dir, path, &info, 0) == -1)
        return send_err(d, 404);

    // if this a directory, look up its index
    if (info.st_mode & S_IFDIR) {
        strncat(path, "/index.html", sizeof(path) - len - 1);
        if (fstatat(d->dir, path, &info, 0) == -1)
            return send_err(d, 404);
    }

    if ((info.st_mode & S_IFREG) == 0 || (info.st_mode & S_IFLNK) == 0)
        return send_err(d, 403);

    if (info.st_size >= UINT32_MAX)
        return send_err(d, 500);

    const int f = openat(d->dir, path, O_RDONLY | O_CLOEXEC);
    ensure(f != -1, "could not open %s", path);

    q_write_file(d->w, d->s, f, (uint32_t)info.st_size, true);

    return 0;
}


#define MAXPORTS 16

int main(int argc, char * argv[])
{
    uint64_t timeout = 10;
#ifndef NDEBUG
    util_dlevel = DLEVEL; // default to maximum compiled-in verbosity
#endif
    char ifname[IFNAMSIZ] = "lo"
#ifndef __linux__
                            "0"
#endif
        ;
    char dir[MAXPATHLEN] = ".";
    char cert[MAXPATHLEN] = "test/dummy.crt";
    char key[MAXPATHLEN] = "test/dummy.key";
    uint16_t port[MAXPORTS] = {4433, 4434};
    size_t num_ports = 0;
    uint64_t num_bufs = 100000;
    int ch;
    int ret = 0;

    while ((ch = getopt(argc, argv, "hi:p:d:v:c:k:t:b:")) != -1) {
        switch (ch) {
        case 'i':
            strncpy(ifname, optarg, sizeof(ifname) - 1);
            break;
        case 'd':
            strncpy(dir, optarg, sizeof(dir) - 1);
            break;
        case 'c':
            strncpy(cert, optarg, sizeof(cert) - 1);
            break;
        case 'k':
            strncpy(key, optarg, sizeof(key) - 1);
            break;
        case 'p':
            port[num_ports++] =
                (uint16_t)MIN(UINT16_MAX, strtoul(optarg, 0, 10));
            ensure(num_ports < MAXPORTS, "can only listen on at most %u ports",
                   MAXPORTS);
            break;
        case 't':
            timeout = MIN(600, strtoul(optarg, 0, 10)); // 10 min
            break;
        case 'b':
            num_bufs = MAX(1000, MIN(strtoul(optarg, 0, 10), UINT32_MAX));
            break;
        case 'v':
#ifndef NDEBUG
            util_dlevel = (short)MIN(DLEVEL, strtoul(optarg, 0, 10));
#endif
            break;
        case 'h':
        case '?':
        default:
            usage(basename(argv[0]), ifname, port[0], dir, cert, key, timeout,
                  num_bufs);
        }
    }

    if (num_ports == 0)
        // if no -p args were given, we listen on two ports by default
        num_ports = 2;

    const int dir_fd = open(dir, O_RDONLY | O_CLOEXEC);
    ensure(dir_fd != -1, "%s does not exist", dir);

    struct w_engine * const w = q_init(
        ifname, &(const struct q_conf){
                    .num_bufs = num_bufs, .tls_cert = cert, .tls_key = key});
    struct q_conn * conn[MAXPORTS];
    size_t bound = 0;
    for (size_t i = 0; i < num_ports; i++) {
        conn[i] = q_bind(w, port[i]);
        if (conn[i]) {
            warn(DBG, "%s waiting on %s port %d", basename(argv[0]), ifname,
                 port[i]);
            bound++;
        } else
            warn(ERR, "%s failed to bind to %s port %d", basename(argv[0]),
                 ifname, port[i]);
    }

    bool first_conn = true;
    http_parser_settings settings = {.on_url = serve_cb};

    while (bound) {
        struct q_conn * c = q_rx_ready(first_conn ? 0 : timeout);
        if (c == 0)
            break;
        first_conn = false;

        // do we need to q_accept?
        if (q_is_new_serv_conn(c))
            q_accept(&(struct q_conn_conf){
                .idle_timeout = timeout,
                .enable_spinbit = true,
            });

        while (1) {
            // do we need to handle a request?
            struct cb_data d = {.c = c, .w = w, .dir = dir_fd};
            http_parser parser = {.data = &d};

            http_parser_init(&parser, HTTP_REQUEST);
            struct w_iov_sq q = w_iov_sq_initializer(q);
            struct q_stream * s = q_read(c, &q, false);

            if (sq_empty(&q))
                // no more streams with pending reqs, try next conn
                break;

            if (q_is_uni_stream(s)) {
                warn(NTE, "can't serve request on uni stream");

            } else {
                d.s = s;
                struct w_iov * v = 0;
                sq_foreach (v, &q, next) {
                    const size_t parsed = http_parser_execute(
                        &parser, &settings, (char *)v->buf, v->len);
                    if (parsed != v->len) {
                        warn(ERR, "HTTP parser error: %.*s",
                             (int)(v->len - parsed), &v->buf[parsed]);
                        // XXX the strnlen() test is super-hacky
                        if (strnlen((char *)v->buf, v->len) == v->len)
                            send_err(&d, 400);
                        else
                            send_err(&d, 505);
                        ret = 1;
                        q_free(&q);
                        goto err;
                    }
                    if (q_peer_has_closed_stream(s)) {
                        q_close_stream(s);
                        break;
                    }
                }
            }
            q_free(&q);
        }
    err:;
    }

    q_cleanup(w);
    warn(DBG, "%s exiting", basename(argv[0]));
    return ret;
}
