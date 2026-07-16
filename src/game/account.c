/*
 * Part of Astonia Client. Please read license.txt.
 *
 * Account operations (register / list characters / create character), spoken to
 * the server before login over a short-lived connection. See account_proto.h
 * for the wire format.
 *
 * The reply arrives right after the connection's tiny uncompressed SV_REALTIME
 * greeting frame, which we skip by its length header; the account reply itself
 * is sent raw by the server, so no zlib is involved here.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "game/account.h"
#include "astonia_net.h"

#define ACC_PARSE_NEED (-2) // reply not fully received yet

// XOR obfuscation, identical to the server's decrypt() and the client's login
// path. It is its own inverse: applying it here obfuscates, and the server
// applies it again to recover the plaintext. Keyed by the account username.
static void acc_obfuscate(const char *name, char *pass_buf)
{
    static const char secret[4][ACC_PWLEN + 1] = {
        "\000cgf\000de8etzdf\000dx",
        "jrfa\000v7d\000drt\000edm",
        "t6zh\000dlr\000fu4dms\000",
        "jkdm\000u7z5g\000j77\000g"};
    int i;

    for (i = 0; i < ACC_PWLEN; i++) {
        pass_buf[i] = (char)(pass_buf[i] ^ secret[name[1] % 4][i] ^ name[i % 3]);
    }
}

// Write a zero-padded ACC_NAMELEN name field.
static void put_name(unsigned char *dst, const char *s)
{
    size_t n = strlen(s);

    if (n > (size_t)(ACC_NAMELEN - 1)) {
        n = (size_t)(ACC_NAMELEN - 1);
    }
    memset(dst, 0, ACC_NAMELEN);
    memcpy(dst, s, n);
}

// Parse a partly/fully received buffer. Returns an ACC_ST_* status when a
// complete reply is present, ACC_PARSE_NEED if more bytes are needed, or
// ACC_NET_ERR if a located reply is malformed.
//
// The server writes the reply raw, but it is preceded by the connection's
// SV_REALTIME greeting frame and interleaved with single-byte empty "tick"
// frames (0x40) that pflush emits every tick. So rather than assume a fixed
// offset, we scan for the reply's magic, validated by the op/status/count that
// follow it (0x40 and the greeting bytes cannot form that signature).
static int parse_reply(const unsigned char *buf, int len, struct acc_char *chars, int maxchars, int *pcount,
    int *padmin)
{
    int off, p, n, count, status;

    if (pcount) {
        *pcount = 0;
    }
    if (padmin) {
        *padmin = 0;
    }

    for (off = 0; off + 4 <= len; off++) {
        if (buf[off] != ACC_REPLY_MAGIC) {
            continue;
        }
        if (buf[off + 1] < ACC_OP_REGISTER || buf[off + 1] > ACC_OP_CREATE) {
            continue; // not a valid op echo
        }
        if (buf[off + 2] > ACC_ST_SERVERERR) {
            continue; // not a valid status
        }
        if (buf[off + 3] > ACC_MAXCHARS) {
            continue; // not a valid count
        }
        break; // found a plausible reply header
    }
    if (off + 4 > len) {
        return ACC_PARSE_NEED; // header not fully present yet
    }
    if (off + 5 > len) {
        return ACC_PARSE_NEED; // acctflags byte not yet received
    }

    status = buf[off + 2];
    count = buf[off + 3];
    if (padmin) {
        *padmin = (buf[off + 4] & ACC_ACCT_ADMIN) ? 1 : 0;
    }

    p = off + 5;
    for (n = 0; n < count; n++) {
        int nl, cl;

        if (len < p + 1) {
            return ACC_PARSE_NEED;
        }
        nl = buf[p];
        p += 1;
        if (nl >= ACC_NAMELEN) {
            return ACC_NET_ERR; // malformed name length
        }
        if (len < p + nl + 8) {
            return ACC_PARSE_NEED;
        }
        if (chars && n < maxchars) {
            cl = nl;
            memcpy(chars[n].name, buf + p, (size_t)cl);
            chars[n].name[cl] = 0;
            memcpy(&chars[n].flags, buf + p + nl, 4);
            memcpy(&chars[n].exp, buf + p + nl + 4, 4);
        }
        p += nl + 8;
    }

    if (pcount) {
        *pcount = (count < maxchars) ? count : maxchars;
    }
    return status;
}

// Connect, send one request, read the reply. Returns ACC_ST_* or ACC_NET_ERR.
static int account_transact(const char *host, int port, const unsigned char *req, int reqlen, struct acc_char *chars,
    int maxchars, int *pcount, int *padmin)
{
    astonia_sock *s;
    unsigned char rbuf[1024];
    int rused = 0, sent = 0;
    Uint64 deadline;

    if (pcount) {
        *pcount = 0;
    }
    if (padmin) {
        *padmin = 0;
    }

    s = astonia_net_connect(host, (uint16_t)port, 3000);
    if (!s) {
        return ACC_NET_ERR;
    }

    deadline = SDL_GetTicks() + 5000;

    // send the whole request (tiny, but handle partial writes anyway)
    while (sent < reqlen) {
        int pr;
        ptrdiff_t w;

        if (SDL_GetTicks() > deadline) {
            astonia_net_close(s);
            return ACC_NET_ERR;
        }
        pr = astonia_net_poll(s, 2, 50);
        if (pr < 0) {
            astonia_net_close(s);
            return ACC_NET_ERR;
        }
        if (!(pr & 2)) {
            continue;
        }
        w = astonia_net_send(s, req + sent, (size_t)(reqlen - sent));
        if (w > 0) {
            sent += (int)w;
        }
    }

    // read until we have a complete reply or time out
    for (;;) {
        int pr, status;
        ptrdiff_t r;

        if (SDL_GetTicks() > deadline) {
            astonia_net_close(s);
            return ACC_NET_ERR;
        }
        pr = astonia_net_poll(s, 1, 50);
        if (pr < 0) {
            astonia_net_close(s);
            return ACC_NET_ERR;
        }
        if (!(pr & 1)) {
            continue;
        }
        r = astonia_net_recv(s, rbuf + rused, sizeof(rbuf) - (size_t)rused);
        if (r > 0) {
            rused += (int)r;
            status = parse_reply(rbuf, rused, chars, maxchars, pcount, padmin);
            if (status != ACC_PARSE_NEED) {
                astonia_net_close(s);
                return status;
            }
            // The server streams empty tick-frames (0x40) continuously, so the
            // buffer can fill with noise before the reply. When it nears full,
            // drop all but the tail (large enough to hold a full LIST reply) so
            // an in-progress reply is preserved but the noise is discarded.
            if ((size_t)rused > sizeof(rbuf) - 64) {
                int keep = 600;
                memmove(rbuf, rbuf + rused - keep, (size_t)keep);
                rused = keep;
            }
        } else if (r == 0) {
            // server closed the connection; make a final parse attempt
            status = parse_reply(rbuf, rused, chars, maxchars, pcount, padmin);
            astonia_net_close(s);
            return (status == ACC_PARSE_NEED) ? ACC_NET_ERR : status;
        }
        // r < 0: would-block, keep polling until the deadline
    }
}

// Build the shared username(40) + obfuscated-password(16) prefix at buf+1.
static void build_credentials(unsigned char *buf, const char *username, const char *password)
{
    char uname[ACC_NAMELEN];
    char pw[ACC_PWLEN];

    memset(uname, 0, sizeof(uname));
    strncpy(uname, username, (size_t)(ACC_NAMELEN - 1));

    memset(pw, 0, sizeof(pw));
    strncpy(pw, password, (size_t)(ACC_PWLEN - 1));
    acc_obfuscate(uname, pw);

    put_name(buf + 1, username);
    memcpy(buf + 1 + ACC_NAMELEN, pw, ACC_PWLEN);
}

int account_register(const char *host, int port, const char *username, const char *password)
{
    unsigned char req[ACC_REQ_BASE];

    req[0] = (unsigned char)ACC_OP_REGISTER;
    build_credentials(req, username, password);

    return account_transact(host, port, req, (int)sizeof(req), NULL, 0, NULL, NULL);
}

int account_list(const char *host, int port, const char *username, const char *password, struct acc_char *out,
    int max, int *out_count, int *out_admin)
{
    unsigned char req[ACC_REQ_BASE];

    req[0] = (unsigned char)ACC_OP_LIST;
    build_credentials(req, username, password);

    return account_transact(host, port, req, (int)sizeof(req), out, max, out_count, out_admin);
}

int account_create(const char *host, int port, const char *username, const char *password, const char *charname,
    int flags)
{
    unsigned char req[ACC_REQ_CREATE];

    req[0] = (unsigned char)ACC_OP_CREATE;
    build_credentials(req, username, password);
    put_name(req + 1 + ACC_NAMELEN + ACC_PWLEN, charname);
    req[1 + ACC_NAMELEN + ACC_PWLEN + ACC_NAMELEN] = (unsigned char)flags;

    return account_transact(host, port, req, (int)sizeof(req), NULL, 0, NULL, NULL);
}
