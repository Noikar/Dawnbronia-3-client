/*
 * Part of Astonia Client. Please read license.txt.
 *
 * Account sub-protocol (pre-login registration and character management).
 *
 * Mirror of the server's account_proto.h. The client talks this over a
 * short-lived connection to the normal game port, before login. The server
 * tells an account request apart from a classic login by the FIRST byte (a
 * login always starts with an alphabetic character-name byte, so the op codes
 * below are deliberately non-alpha).
 *
 * IMPORTANT: keep this file's constants identical to the server copy
 * (astonia_community_server3/account_proto.h).
 */

#ifndef ACCOUNT_PROTO_H
#define ACCOUNT_PROTO_H

// Field widths on the wire, matching the login blob.
#define ACC_NAMELEN 40
#define ACC_PWLEN   16

// Request op codes (first byte). Non-alpha on purpose.
#define ACC_OP_REGISTER 0x01
#define ACC_OP_LIST     0x02
#define ACC_OP_CREATE   0x03
#define ACC_OP_DELETE   0x04 // reserved for a later phase

// CREATE character flags byte.
#define ACC_FLAG_MALE    0x01 // clear = female
#define ACC_FLAG_WARRIOR 0x02 // warrior bit
#define ACC_FLAG_MAGE    0x04 // mage bit (warrior+mage both set = seyan)
#define ACC_FLAG_ARCH    0x08 // arch variant - honored only for admin accounts
#define ACC_FLAG_GOD     0x10 // god powers - honored only for admin accounts

// Per-account character cap (also the max entries in a LIST reply).
#define ACC_MAXCHARS 8

// Request layouts (fixed size, little-endian x86 both ends). The password field
// is obfuscated with the same XOR scheme login uses, keyed by the account name.
#define ACC_REQ_BASE   (1 + ACC_NAMELEN + ACC_PWLEN) // REGISTER / LIST
#define ACC_REQ_CREATE (ACC_REQ_BASE + ACC_NAMELEN + 1) // CREATE

// Reply frame (raw, uncompressed), read after the connection's SV_REALTIME
// greeting frame:
//   u8  magic     = ACC_REPLY_MAGIC
//   u8  op        = echoed op
//   u8  status    = ACC_ST_*
//   u8  count     = number of list entries (LIST only, else 0)
//   u8  acctflags = account-level flags (ACC_ACCT_*); 0 unless authenticated
//   count x entry: u8 namelen, u8 name[namelen], u32 flags, u32 exp
#define ACC_REPLY_MAGIC 0xA5

// Account-level reply flags (the acctflags byte).
#define ACC_ACCT_ADMIN 0x01 // account may create arch / god characters

#define ACC_ST_OK        0
#define ACC_ST_BADCREDS  1
#define ACC_ST_TAKEN     2
#define ACC_ST_INVALID   3
#define ACC_ST_LIMIT     4
#define ACC_ST_SERVERERR 5

#endif
