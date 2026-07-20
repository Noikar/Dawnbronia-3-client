/*
 * Part of Astonia Client. Please read license.txt.
 *
 * Account operations (register / list characters / create character) spoken to
 * the server before login. Each call opens a short-lived connection, sends one
 * request, reads the reply, and closes.
 */

#ifndef ACCOUNT_H
#define ACCOUNT_H

#include "client/account_proto.h"

// A character as returned by account_list().
struct acc_char {
	char name[ACC_NAMELEN];
	unsigned int flags; // character class flag bits (gender + warrior/mage/seyan/arch/god)
	unsigned int exp;
};

// All three return an ACC_ST_* status (>= 0) on a completed server reply, or
// ACC_NET_ERR on a connection/timeout failure.
#define ACC_NET_ERR (-1)

int account_register(const char *host, int port, const char *username, const char *password);

// Fills up to max entries; *out_count receives how many were returned. If
// out_admin is non-NULL, *out_admin receives 1 when the account may create
// arch / god characters (ACC_ACCT_ADMIN), else 0.
int account_list(const char *host, int port, const char *username, const char *password, struct acc_char *out, int max,
    int *out_count, int *out_admin);

// flags is a bitmask of ACC_FLAG_MALE / ACC_FLAG_WARRIOR / ACC_FLAG_MAGE /
// ACC_FLAG_ARCH / ACC_FLAG_GOD (arch and god honored only for admin accounts).
int account_create(
    const char *host, int port, const char *username, const char *password, const char *charname, int flags);

// Permanently delete a character owned by the account. The password is the one
// the player re-typed to confirm, so a mismatch returns ACC_ST_BADCREDS. The
// server also refuses with ACC_ST_ONLINE while the character is logged in, or
// ACC_ST_INCLAN while it belongs to a clan or club (admin accounts bypass both).
int account_delete(const char *host, int port, const char *username, const char *password, const char *charname);

#endif
