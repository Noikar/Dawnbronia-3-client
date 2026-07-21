/*
 * Part of Astonia content tooling (Dawnbronia). Please read license.txt.
 *
 * zoneio - lossless round-trip reader/writer for .itm/.chr/.map content files.
 * See zoneio.h. The grammar and token tables mirror the server's create.c so
 * that anything written here loads cleanly on the server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "zoneio.h"

/* ================================================================ tables == */
/* Copied verbatim (order-sensitive: the array index is the bit/enum value)
 * from create.c. These are the authoritative token spellings. */

static const char *V_tab[ZIO_V_MAX] = {"V_HP", "V_ENDURANCE", "V_MANA", "V_WIS", "V_INT", "V_AGI", "V_STR", "V_ARMOR",
    "V_WEAPON", "V_LIGHT", "V_SPEED", "V_PULSE", "V_DAGGER", "V_HAND", "V_STAFF", "V_SWORD", "V_TWOHAND",
    "V_ARMORSKILL", "V_ATTACK", "V_PARRY", "V_WARCRY", "V_TACTICS", "V_SURROUND", "V_BODYCONTROL", "V_SPEEDSKILL",
    "V_BARTER", "V_PERCEPT", "V_STEALTH", "V_BLESS", "V_HEAL", "V_FREEZE", "V_MAGICSHIELD", "V_FLASH", "V_FIREBALL",
    "V_ARCANE", "V_REGENERATE", "V_MEDITATE", "V_IMMUNITY", "V_DEMON", "V_DURATION", "V_RAGE", "V_COLD",
    "V_PROFESSION"};

static const char *P_tab[ZIO_P_MAX] = {"P_ATHLETE", "P_ALCHEMIST", "P_MINER", "P_ASSASSIN", "P_THIEF", "P_LIGHT",
    "P_DARK", "P_TRADER", "P_MERCENARY", "P_CLAN", "P_HERBALIST", "P_DEMON", "P_NULL", "P_NULL", "P_NULL", "P_NULL",
    "P_NULL", "P_NULL", "P_NULL", "P_NULL"};

static const char *WN_tab[ZIO_WN_MAX] = {"WN_NECK", "WN_HEAD", "WN_CLOAK", "WN_ARMS", "WN_BODY", "WN_BELT", "WN_RHAND",
    "WN_LEGS", "WN_LHAND", "WN_RRING", "WN_FEET", "WN_LRING"};

static const char *IF_tab[] = {"IF_USED", "IF_MOVEBLOCK", "IF_SIGHTBLOCK", "IF_TAKE", "IF_USE", "IF_WNHEAD",
    "IF_WNNECK", "IF_WNBODY", "IF_WNARMS", "IF_WNBELT", "IF_WNLEGS", "IF_WNFEET", "IF_WNLHAND", "IF_WNRHAND",
    "IF_WNCLOAK", "IF_WNLRING", "IF_WNRRING", "IF_WNTWOHANDED", "IF_AXE", "IF_DAGGER", "IF_HAND", "IF_SHANON",
    "IF_STAFF", "IF_SWORD", "IF_TWOHAND", "IF_DOOR", "IF_QUEST", "IF_SOUNDBLOCK", "IF_STEPACTION", "IF_MONEY",
    "IF_NODECAY", "IF_FRONTWALL", "IF_DEPOT", "IF_NODEPOT", "IF_NODROP", "IF_NOJUNK", "IF_PLAYERBODY", "IF_BONDTAKE",
    "IF_BONDWEAR", "IF_LABITEM", "IF_VOID", "IF_NOENHANCE", "IF_BEYONDBOUNDS", "IF_BEYONDMAXMOD"};

static const char *CF_tab[] = {"CF_USED", "CF_IMMORTAL", "CF_GOD", "CF_PLAYER", "CF_STAFF", "CF_INVISIBLE", "CF_SHUTUP",
    "CF_KICKED", "CF_UPDATE", "CF_STUNNED", "CF_UNCONSCIOUS", "CF_DEAD", "CF_ITEMS", "CF_RESPAWN", "CF_MALE",
    "CF_FEMALE", "CF_WARRIOR", "CF_MAGE", "CF_ARCH", "CF_BERSERK", "CF_NOATTACK", "CF_HASNAME", "CF_QUESTITEM",
    "CF_INFRARED", "CF_PK", "CF_ITEMDEATH", "CF_NODEATH", "CF_NOBODY", "CF_EDEMON", "CF_FDEMON", "CF_IDEMON",
    "CF_NOGIVE", "CF_PLAYERLIKE", "CF_RESERVED0", "CF_PAID", "CF_PROF", "CF_ALIVE", "CF_DEMON", "CF_UNDEAD",
    "CF_HARDKILL", "CF_NOBLESS", "CF_AREACHANGE", "CF_LAG", "CF_WON", "CF_THIEFMODE", "CF_NOTELL", "CF_INFRAVISION",
    "CF_NOMAGIC", "CF_NONOMAGIC", "CF_OXYGEN", "CF_NOPLRATT", "CF_ALLOWSWAP", "CF_LQMASTER", "CF_HARDCORE",
    "CF_NONOTIFY", "CF_SMALLUPDATE"};

static const char *MF_tab[] = {"MF_MOVEBLOCK", "MF_SIGHTBLOCK", "MF_TMOVEBLOCK", "MF_TSIGHTBLOCK", "MF_INDOORS",
    "MF_RESTAREA", "MF_DOOR", "MF_SOUNDBLOCK", "MF_TSOUNDBLOCK", "MF_SHOUTBLOCK", "MF_CLAN", "MF_ARENA", "MF_PEACE",
    "MF_NEUTRAL", "MF_FIRETHRU", "MF_SLOWDEATH", "MF_NOLIGHT", "MF_NOMAGIC", "MF_UNDERWATER", "MF_NOREGEN"};

#define IF_COUNT ((int)(sizeof(IF_tab) / sizeof(IF_tab[0])))
#define CF_COUNT ((int)(sizeof(CF_tab) / sizeof(CF_tab[0])))
#define MF_COUNT ((int)(sizeof(MF_tab) / sizeof(MF_tab[0])))

static int lookup_tab(const char **tab, int n, const char *name)
{
	int i;
	for (i = 0; i < n; i++) {
		if (!strcasecmp(tab[i], name)) {
			return i;
		}
	}
	return -1;
}

int zio_lookup_V(const char *name)
{
	return lookup_tab(V_tab, ZIO_V_MAX, name);
}

int zio_lookup_P(const char *name)
{
	return lookup_tab(P_tab, ZIO_P_MAX, name);
}

int zio_lookup_WN(const char *name)
{
	return lookup_tab(WN_tab, ZIO_WN_MAX, name);
}

int zio_lookup_IF(const char *name)
{
	return lookup_tab(IF_tab, IF_COUNT, name);
}

int zio_lookup_CF(const char *name)
{
	return lookup_tab(CF_tab, CF_COUNT, name);
}

int zio_lookup_MF(const char *name)
{
	return lookup_tab(MF_tab, MF_COUNT, name);
}

const char *zio_name_V(int i)
{
	return (i >= 0 && i < ZIO_V_MAX) ? V_tab[i] : NULL;
}

const char *zio_name_P(int i)
{
	return (i >= 0 && i < ZIO_P_MAX) ? P_tab[i] : NULL;
}

const char *zio_name_WN(int i)
{
	return (i >= 0 && i < ZIO_WN_MAX) ? WN_tab[i] : NULL;
}

const char *zio_name_IF(int i)
{
	return (i >= 0 && i < IF_COUNT) ? IF_tab[i] : NULL;
}

const char *zio_name_CF(int i)
{
	return (i >= 0 && i < CF_COUNT) ? CF_tab[i] : NULL;
}

const char *zio_name_MF(int i)
{
	return (i >= 0 && i < MF_COUNT) ? MF_tab[i] : NULL;
}

/* ================================================================ lexer === */

enum { ZTOK_EOF, ZTOK_SEMI, ZTOK_COMMENT, ZTOK_RECORD, ZTOK_ASSIGN, ZTOK_ERR };

typedef struct {
	const char *base;
	size_t len;
	size_t pos;
} zio_lexer;

typedef struct {
	int kind;
	char name[256];
	char value[256];
	size_t start; /* offset of first meaningful char (post leading ws) */
	size_t end; /* offset just past the consumed token               */
} zio_tok;

static int is_name_char(int c)
{
	return isalnum(c) || c == '_' || c == '-';
}

/* Mirrors create.c get_text(), but reports byte offsets. */
static void lex_next(zio_lexer *lx, zio_tok *t)
{
	const char *base = lx->base;
	size_t p = lx->pos, len = lx->len;
	size_t i;

	memset(t, 0, sizeof(*t));

	while (p < len && isspace((unsigned char)base[p])) {
		p++;
	}
	t->start = p;

	if (p >= len) {
		t->kind = ZTOK_EOF;
		t->end = p;
		lx->pos = p;
		return;
	}

	if (base[p] == ';') {
		t->kind = ZTOK_SEMI;
		t->end = p + 1;
		lx->pos = p + 1;
		return;
	}

	if (base[p] == '#') {
		while (p < len && base[p] != '\n' && base[p] != '\r') {
			p++;
		}
		t->kind = ZTOK_COMMENT;
		t->end = p;
		lx->pos = p;
		return;
	}

	/* read name */
	i = 0;
	while (p < len && i < 255 && is_name_char((unsigned char)base[p])) {
		t->name[i++] = base[p++];
	}
	t->name[i] = 0;

	while (p < len && isspace((unsigned char)base[p])) {
		p++;
	}

	if (p < len && base[p] == ':') {
		t->kind = ZTOK_RECORD;
		t->end = p + 1;
		lx->pos = p + 1;
		return;
	}

	if (p >= len || base[p] != '=') {
		/* An empty name at end-of-buffer is a clean EOF, not an error. */
		if (t->name[0] == 0) {
			t->kind = ZTOK_EOF;
		} else {
			t->kind = ZTOK_ERR;
		}
		t->end = p;
		lx->pos = p;
		return;
	}
	p++; /* skip '=' */

	while (p < len && isspace((unsigned char)base[p])) {
		p++;
	}

	if (p < len && base[p] == '"') {
		p++;
		i = 0;
		while (p < len && i < 255 && base[p] != '"') {
			t->value[i++] = base[p++];
		}
		t->value[i] = 0;
		if (p >= len || base[p] != '"') {
			t->kind = ZTOK_ERR;
			t->end = p;
			lx->pos = p;
			return;
		}
		p++; /* closing quote */
	} else {
		i = 0;
		while (p < len && i < 255 && is_name_char((unsigned char)base[p])) {
			t->value[i++] = base[p++];
		}
		t->value[i] = 0;
	}

	t->kind = ZTOK_ASSIGN;
	t->end = p;
	lx->pos = p;
}

/* ============================================================= helpers ==== */

static unsigned char hex1(char c)
{
	if (c >= '0' && c <= '9') {
		return (unsigned char)(c - '0');
	}
	if (c >= 'A' && c <= 'F') {
		return (unsigned char)(c - 'A' + 10);
	}
	if (c >= 'a' && c <= 'f') {
		return (unsigned char)(c - 'a' + 10);
	}
	return 0;
}

/* Decode a hex string into bytes (2 chars per byte), capped at IT_DR_SIZE.
 * Returns number of bytes decoded. */
static int decode_hex(unsigned char *dst, const char *src)
{
	int n = 0;
	while (n < ZIO_IT_DR_SIZE && src[0] && src[1]) {
		dst[n++] = (unsigned char)(hex1(src[0]) * 16 + hex1(src[1]));
		src += 2;
	}
	return n;
}

/* Parse "<digits>,<digits>" with optional surrounding whitespace; the whole
 * string must be consumed. Mirrors create.c comma(). */
static int comma_parse(const char *s, int *a, int *b)
{
	char one[64], two[64];
	int i;

	while (isspace((unsigned char)*s)) {
		s++;
	}
	i = 0;
	while (isdigit((unsigned char)*s) && i < 63) {
		one[i++] = *s++;
	}
	one[i] = 0;
	while (isspace((unsigned char)*s)) {
		s++;
	}
	if (*s != ',') {
		return 0;
	}
	s++;
	i = 0;
	while (isdigit((unsigned char)*s) && i < 63) {
		two[i++] = *s++;
	}
	two[i] = 0;
	while (isspace((unsigned char)*s)) {
		s++;
	}
	if (*s) {
		return 0;
	}
	if (!one[0] || !two[0]) {
		return 0;
	}

	*a = atoi(one);
	*b = atoi(two);
	return 1;
}

/* --- string builder --------------------------------------------------- */

static int sb_ensure(char **buf, size_t *cap, size_t need)
{
	if (need <= *cap) {
		return 0;
	}
	size_t nc = *cap ? *cap : 256;
	while (nc < need) {
		nc *= 2;
	}
	char *nb = realloc(*buf, nc);
	if (!nb) {
		return -1;
	}
	*buf = nb;
	*cap = nc;
	return 0;
}

static int sb_putn(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
	if (sb_ensure(buf, cap, *len + n + 1) != 0) {
		return -1;
	}
	memcpy(*buf + *len, s, n);
	*len += n;
	(*buf)[*len] = 0;
	return 0;
}

static int sb_puts(char **buf, size_t *len, size_t *cap, const char *s)
{
	return sb_putn(buf, len, cap, s, strlen(s));
}

/* Emit "key=value\n". quoted controls whether the value is wrapped in "". */
static int sb_field(char **buf, size_t *len, size_t *cap, const char *key, const char *value, int quoted)
{
	if (sb_puts(buf, len, cap, key) != 0) {
		return -1;
	}
	if (sb_puts(buf, len, cap, "=") != 0) {
		return -1;
	}
	if (quoted && sb_puts(buf, len, cap, "\"") != 0) {
		return -1;
	}
	if (sb_puts(buf, len, cap, value) != 0) {
		return -1;
	}
	if (quoted && sb_puts(buf, len, cap, "\"") != 0) {
		return -1;
	}
	return sb_puts(buf, len, cap, "\n");
}

static int sb_field_int(char **buf, size_t *len, size_t *cap, const char *key, int value)
{
	char tmp[32];
	snprintf(tmp, sizeof(tmp), "%d", value);
	return sb_field(buf, len, cap, key, tmp, 0);
}

/* ======================================================= record decoding == */

/* Decode a single .itm field into *it. *cmod tracks the current modifier slot.
 * Returns 0 on success, -1 on error (message into err). */
static int decode_item_field(zio_item *it, int *cmod, const char *name, const char *value, char *err, size_t errn)
{
	int v;

	if (!strcasecmp(name, "name")) {
		strncpy(it->item_name, value, sizeof(it->item_name) - 1);
	} else if (!strcasecmp(name, "description")) {
		strncpy(it->description, value, sizeof(it->description) - 1);
	} else if (!strcasecmp(name, "value")) {
		it->value = atoi(value);
	} else if (!strcasecmp(name, "sprite")) {
		it->sprite = atoi(value);
	} else if (!strcasecmp(name, "driver")) {
		it->driver = atoi(value);
	} else if (!strcasecmp(name, "min_level")) {
		it->min_level = atoi(value);
	} else if (!strcasecmp(name, "max_level")) {
		it->max_level = atoi(value);
	} else if (!strcasecmp(name, "needs_class")) {
		it->needs_class = atoi(value);
	} else if (!strcasecmp(name, "ID")) {
		it->ID = (unsigned int)strtoul(value, NULL, 16);
		it->has_ID = 1;
	} else if (!strcasecmp(name, "arg")) {
		it->arg_len = decode_hex(it->arg, value);
		it->has_arg = 1;
	} else if (!strcasecmp(name, "mod_index")) {
		v = zio_lookup_V(value);
		if (v == -1) {
			snprintf(err, errn, "unknown V_name \"%s\"", value);
			return -1;
		}
		if (*cmod < ZIO_MAXMOD) {
			it->mod_index[*cmod] = v;
		}
	} else if (!strcasecmp(name, "req_index")) {
		v = zio_lookup_V(value);
		if (v == -1) {
			snprintf(err, errn, "unknown V_name \"%s\"", value);
			return -1;
		}
		if (*cmod < ZIO_MAXMOD) {
			it->mod_index[*cmod] = -v;
		}
	} else if (!strcasecmp(name, "mod_value") || !strcasecmp(name, "req_value")) {
		if (*cmod >= ZIO_MAXMOD) {
			snprintf(err, errn, "MAXMOD (%d) exceeded", ZIO_MAXMOD);
			return -1;
		}
		it->mod_value[*cmod] = atoi(value);
		(*cmod)++;
		it->mod_count = *cmod;
	} else if (!strcasecmp(name, "flag")) {
		v = zio_lookup_IF(value);
		if (v == -1) {
			snprintf(err, errn, "unknown IF \"%s\"", value);
			return -1;
		}
		it->flags |= (1ull << v);
	} else {
		snprintf(err, errn, "unknown item field \"%s\"", name);
		return -1;
	}
	return 0;
}

static int add_name(char (**list)[ZIO_NAMELEN], int *count, const char *value)
{
	char (*nl)[ZIO_NAMELEN] = realloc(*list, sizeof(*nl) * (size_t)(*count + 1));
	if (!nl) {
		return -1;
	}
	*list = nl;
	strncpy(nl[*count], value, ZIO_NAMELEN - 1);
	nl[*count][ZIO_NAMELEN - 1] = 0;
	(*count)++;
	return 0;
}

/* Decode a single .chr field. *rslot tracks the current random-item slot. */
static int decode_char_field(zio_char *ch, int *rslot, const char *name, const char *value, char *err, size_t errn)
{
	int v, wn, p;

	if (!strcasecmp(name, "name")) {
		strncpy(ch->ch_name, value, sizeof(ch->ch_name) - 1);
	} else if (!strcasecmp(name, "description")) {
		strncpy(ch->description, value, sizeof(ch->description) - 1);
	} else if (!strcasecmp(name, "sprite")) {
		ch->sprite = atoi(value);
	} else if (!strcasecmp(name, "sound")) {
		ch->sound = atoi(value);
	} else if (!strcasecmp(name, "gold")) {
		ch->gold = atoi(value);
	} else if (!strcasecmp(name, "driver")) {
		ch->driver = atoi(value);
	} else if (!strcasecmp(name, "group")) {
		ch->group = atoi(value);
	} else if (!strcasecmp(name, "class")) {
		ch->class_ = atoi(value);
	} else if (!strcasecmp(name, "respawn")) {
		ch->respawn = atoi(value);
		ch->has_respawn = 1;
	} else if (!strcasecmp(name, "special_prob")) {
		ch->special_prob = atoi(value);
	} else if (!strcasecmp(name, "special_strength")) {
		ch->special_str = atoi(value);
	} else if (!strcasecmp(name, "special_base")) {
		ch->special_base = atoi(value);
	} else if (!strcasecmp(name, "gold_prob")) {
		ch->gold_prob = atoi(value);
	} else if (!strcasecmp(name, "gold_base")) {
		ch->gold_base = atoi(value);
	} else if (!strcasecmp(name, "gold_random")) {
		ch->gold_random = atoi(value);
	} else if (!strcasecmp(name, "arg")) {
		/* multiple arg= lines concatenate (create.c) */
		size_t old = ch->arg ? strlen(ch->arg) : 0;
		char *na = realloc(ch->arg, old + strlen(value) + 1);
		if (!na) {
			snprintf(err, errn, "out of memory");
			return -1;
		}
		strcpy(na + old, value);
		ch->arg = na;
	} else if (!strcasecmp(name, "item")) {
		if (add_name(&ch->items, &ch->item_count, value) != 0) {
			snprintf(err, errn, "out of memory");
			return -1;
		}
	} else if (!strcasecmp(name, "spell")) {
		if (add_name(&ch->spells, &ch->spell_count, value) != 0) {
			snprintf(err, errn, "out of memory");
			return -1;
		}
	} else if (!strcasecmp(name, "ritem")) {
		if (*rslot >= ZIO_MAXRANDITEM) {
			snprintf(err, errn, "random carrying capacity (%d) exceeded", ZIO_MAXRANDITEM);
			return -1;
		}
		if (!ch->rand[*rslot].prob) {
			snprintf(err, errn, "no random value (rprob) set before ritem");
			return -1;
		}
		strncpy(ch->rand[*rslot].name, value, ZIO_NAMELEN - 1);
		(*rslot)++;
		ch->rand_count = *rslot;
	} else if (!strcasecmp(name, "rprob")) {
		v = atoi(value);
		if (v < 1 || v > 9999) {
			snprintf(err, errn, "illegal probability \"%s\"", value);
			return -1;
		}
		if (*rslot < ZIO_MAXRANDITEM) {
			ch->rand[*rslot].prob = v;
		}
	} else if (!strcasecmp(name, "flag")) {
		v = zio_lookup_CF(value);
		if (v == -1) {
			snprintf(err, errn, "unknown CF \"%s\"", value);
			return -1;
		}
		ch->flags |= (1ull << v);
	} else if ((v = zio_lookup_V(name)) != -1) {
		ch->val[v] = atoi(value);
		ch->val_set[v] = 1;
	} else if ((wn = zio_lookup_WN(name)) != -1) {
		strncpy(ch->worn[wn], value, ZIO_NAMELEN - 1);
	} else if ((p = zio_lookup_P(name)) != -1) {
		ch->prof[p] = atoi(value);
		ch->prof_set[p] = 1;
	} else {
		snprintf(err, errn, "unknown char field \"%s\"", name);
		return -1;
	}
	return 0;
}

/* =============================================================== container = */

enum { ZSEG_RAW, ZSEG_REC };

typedef struct {
	int type;
	size_t start, end; /* span in the original text */
	int dirty;
	int rec_index; /* index into records array (ZSEG_REC only) */
} zio_seg;

struct zio_file {
	zio_kind kind;
	char *text;
	size_t text_len;

	zio_seg *segs;
	int seg_count, seg_cap;

	zio_item *items; /* kind == ZIO_ITM */
	zio_char *chars; /* kind == ZIO_CHR */
	int rec_count, rec_cap;
	int *rec_seg; /* record index -> segment index */
};

static int push_seg(zio_file *zf, int type, size_t start, size_t end, int rec_index)
{
	if (start == end && type == ZSEG_RAW) {
		return 0; /* skip empty raw spans */
	}
	if (zf->seg_count >= zf->seg_cap) {
		int nc = zf->seg_cap ? zf->seg_cap * 2 : 16;
		zio_seg *ns = realloc(zf->segs, sizeof(zio_seg) * (size_t)nc);
		if (!ns) {
			return -1;
		}
		zf->segs = ns;
		zf->seg_cap = nc;
	}
	zf->segs[zf->seg_count].type = type;
	zf->segs[zf->seg_count].start = start;
	zf->segs[zf->seg_count].end = end;
	zf->segs[zf->seg_count].dirty = 0;
	zf->segs[zf->seg_count].rec_index = rec_index;
	zf->seg_count++;
	return 0;
}

static int reserve_record(zio_file *zf)
{
	if (zf->rec_count >= zf->rec_cap) {
		int nc = zf->rec_cap ? zf->rec_cap * 2 : 32;
		if (zf->kind == ZIO_ITM) {
			zio_item *ni = realloc(zf->items, sizeof(zio_item) * (size_t)nc);
			if (!ni) {
				return -1;
			}
			zf->items = ni;
		} else {
			zio_char *nc2 = realloc(zf->chars, sizeof(zio_char) * (size_t)nc);
			if (!nc2) {
				return -1;
			}
			zf->chars = nc2;
		}
		int *nrs = realloc(zf->rec_seg, sizeof(int) * (size_t)nc);
		if (!nrs) {
			return -1;
		}
		zf->rec_seg = nrs;
		zf->rec_cap = nc;
	}
	return zf->rec_count;
}

/* Parse a record-structured file (.itm or .chr). */
static int parse_records(zio_file *zf, char *err, size_t errn)
{
	zio_lexer lx = {zf->text, zf->text_len, 0};
	zio_tok t;
	size_t seg_start = 0, rec_start = 0;
	int gotname = 0, ri = -1, cmod = 0, rslot = 0;

	for (;;) {
		lex_next(&lx, &t);

		if (t.kind == ZTOK_ERR) {
			snprintf(err, errn, "syntax error near byte %zu", t.start);
			return -1;
		}
		if (t.kind == ZTOK_COMMENT) {
			continue; /* stays inside the surrounding span */
		}

		if (t.kind == ZTOK_EOF) {
			break;
		}

		if (t.kind == ZTOK_RECORD) {
			if (gotname) {
				snprintf(err, errn, "missing ';' before new record \"%s\"", t.name);
				return -1;
			}
			/* everything before the identifier is raw pass-through */
			if (push_seg(zf, ZSEG_RAW, seg_start, t.start, -1) != 0) {
				goto oom;
			}
			ri = reserve_record(zf);
			if (ri < 0) {
				goto oom;
			}
			if (zf->kind == ZIO_ITM) {
				memset(&zf->items[ri], 0, sizeof(zio_item));
				zf->items[ri].flags = 1ull; /* IF_USED */
				strncpy(zf->items[ri].name, t.name, ZIO_NAMELEN - 1);
			} else {
				memset(&zf->chars[ri], 0, sizeof(zio_char));
				zf->chars[ri].flags = 1ull; /* CF_USED */
				strncpy(zf->chars[ri].name, t.name, ZIO_NAMELEN - 1);
			}
			rec_start = t.start;
			cmod = 0;
			rslot = 0;
			gotname = 1;
			continue;
		}

		if (t.kind == ZTOK_SEMI) {
			if (!gotname) {
				snprintf(err, errn, "';' without a record");
				return -1;
			}
			zf->rec_count++;
			zf->rec_seg[ri] = zf->seg_count;
			if (push_seg(zf, ZSEG_REC, rec_start, t.end, ri) != 0) {
				goto oom;
			}
			gotname = 0;
			seg_start = t.end;
			continue;
		}

		/* ZTOK_ASSIGN */
		if (!gotname) {
			snprintf(err, errn, "field \"%s\" before any record", t.name);
			return -1;
		}
		if (zf->kind == ZIO_ITM) {
			if (decode_item_field(&zf->items[ri], &cmod, t.name, t.value, err, errn) != 0) {
				return -1;
			}
		} else {
			if (decode_char_field(&zf->chars[ri], &rslot, t.name, t.value, err, errn) != 0) {
				return -1;
			}
		}
	}

	if (gotname) {
		snprintf(err, errn, "premature end of input (missing ';')");
		return -1;
	}
	/* trailing raw */
	if (push_seg(zf, ZSEG_RAW, seg_start, zf->text_len, -1) != 0) {
		goto oom;
	}
	return 0;

oom:
	snprintf(err, errn, "out of memory");
	return -1;
}

/* ================================================================ public == */

static int ext_kind(const char *path)
{
	size_t n = strlen(path);
	if (n >= 4) {
		const char *e = path + n - 4;
		if (!strcasecmp(e, ".itm")) {
			return ZIO_ITM;
		}
		if (!strcasecmp(e, ".chr")) {
			return ZIO_CHR;
		}
		if (!strcasecmp(e, ".map")) {
			return ZIO_MAP;
		}
	}
	return -1;
}

zio_file *zio_parse(const char *text, size_t len, int kind, char *errbuf, size_t errbuf_len)
{
	char scratch[256];
	char *err = errbuf ? errbuf : scratch;
	size_t errn = errbuf ? errbuf_len : sizeof(scratch);

	if (kind != ZIO_ITM && kind != ZIO_CHR && kind != ZIO_MAP) {
		snprintf(err, errn, "unknown file kind");
		return NULL;
	}

	zio_file *zf = calloc(1, sizeof(zio_file));
	if (!zf) {
		snprintf(err, errn, "out of memory");
		return NULL;
	}
	zf->kind = (zio_kind)kind;
	zf->text = malloc(len + 1);
	if (!zf->text) {
		free(zf);
		snprintf(err, errn, "out of memory");
		return NULL;
	}
	memcpy(zf->text, text, len);
	zf->text[len] = 0;
	zf->text_len = len;

	if (kind == ZIO_MAP) {
		/* The map body is preserved verbatim; the grid is decoded on demand
		 * via zio_map_parse(). */
		if (push_seg(zf, ZSEG_RAW, 0, len, -1) != 0) {
			zio_free(zf);
			snprintf(err, errn, "out of memory");
			return NULL;
		}
		return zf;
	}

	if (parse_records(zf, err, errn) != 0) {
		zio_free(zf);
		return NULL;
	}
	return zf;
}

zio_file *zio_load(const char *path, int kind, char *errbuf, size_t errbuf_len)
{
	char scratch[256];
	char *err = errbuf ? errbuf : scratch;
	size_t errn = errbuf ? errbuf_len : sizeof(scratch);

	if (kind == ZIO_KIND_AUTO) {
		kind = ext_kind(path);
		if (kind < 0) {
			snprintf(err, errn, "cannot infer kind from extension: %s", path);
			return NULL;
		}
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(err, errn, "cannot open %s", path);
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		snprintf(err, errn, "seek failed on %s", path);
		return NULL;
	}
	long sz = ftell(f);
	if (sz < 0) {
		fclose(f);
		snprintf(err, errn, "tell failed on %s", path);
		return NULL;
	}
	rewind(f);
	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		snprintf(err, errn, "out of memory");
		return NULL;
	}
	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[rd] = 0;

	zio_file *zf = zio_parse(buf, rd, kind, err, errn);
	free(buf);
	return zf;
}

void zio_free(zio_file *zf)
{
	if (!zf) {
		return;
	}
	if (zf->kind == ZIO_CHR && zf->chars) {
		int i;
		for (i = 0; i < zf->rec_count; i++) {
			free(zf->chars[i].arg);
			free(zf->chars[i].items);
			free(zf->chars[i].spells);
		}
	}
	free(zf->items);
	free(zf->chars);
	free(zf->rec_seg);
	free(zf->segs);
	free(zf->text);
	free(zf);
}

zio_kind zio_file_kind(const zio_file *zf)
{
	return zf->kind;
}

int zio_record_count(const zio_file *zf)
{
	return zf->rec_count;
}

zio_item *zio_item_at(zio_file *zf, int i)
{
	if (zf->kind != ZIO_ITM || i < 0 || i >= zf->rec_count) {
		return NULL;
	}
	return &zf->items[i];
}

zio_char *zio_char_at(zio_file *zf, int i)
{
	if (zf->kind != ZIO_CHR || i < 0 || i >= zf->rec_count) {
		return NULL;
	}
	return &zf->chars[i];
}

void zio_mark_dirty(zio_file *zf, int i)
{
	if (i >= 0 && i < zf->rec_count) {
		zf->segs[zf->rec_seg[i]].dirty = 1;
	}
}

/* ------------------------------------------------------- canonical render */

int zio_item_render(const zio_item *it, char **buf, size_t *len, size_t *cap)
{
	int i;
	char tmp[128];

	if (sb_puts(buf, len, cap, it->name) != 0 || sb_puts(buf, len, cap, ":\n") != 0) {
		return -1;
	}
	if (it->item_name[0]) {
		sb_field(buf, len, cap, "name", it->item_name, 1);
	}
	if (it->description[0]) {
		sb_field(buf, len, cap, "description", it->description, 1);
	}
	if (it->value) {
		sb_field_int(buf, len, cap, "value", it->value);
	}
	if (it->sprite) {
		sb_field_int(buf, len, cap, "sprite", it->sprite);
	}
	if (it->driver) {
		sb_field_int(buf, len, cap, "driver", it->driver);
	}
	if (it->min_level) {
		sb_field_int(buf, len, cap, "min_level", it->min_level);
	}
	if (it->max_level) {
		sb_field_int(buf, len, cap, "max_level", it->max_level);
	}
	if (it->needs_class) {
		sb_field_int(buf, len, cap, "needs_class", it->needs_class);
	}
	if (it->has_ID) {
		snprintf(tmp, sizeof(tmp), "%X", it->ID);
		sb_field(buf, len, cap, "ID", tmp, 0);
	}
	if (it->has_arg) {
		size_t o = 0;
		for (i = 0; i < it->arg_len && o + 2 < sizeof(tmp); i++) {
			o += (size_t)snprintf(tmp + o, sizeof(tmp) - o, "%02X", it->arg[i]);
		}
		tmp[o] = 0;
		sb_field(buf, len, cap, "arg", tmp, 1);
	}
	for (i = 0; i < it->mod_count; i++) {
		int idx = it->mod_index[i];
		const char *vn = zio_name_V(idx < 0 ? -idx : idx);
		if (!vn) {
			continue;
		}
		sb_field(buf, len, cap, idx < 0 ? "req_index" : "mod_index", vn, 0);
		sb_field_int(buf, len, cap, idx < 0 ? "req_value" : "mod_value", it->mod_value[i]);
	}
	/* flags (skip bit 0 IF_USED, added implicitly on load) */
	for (i = 1; i < IF_COUNT; i++) {
		if (it->flags & (1ull << i)) {
			sb_field(buf, len, cap, "flag", IF_tab[i], 0);
		}
	}
	return sb_puts(buf, len, cap, ";");
}

int zio_char_render(const zio_char *ch, char **buf, size_t *len, size_t *cap)
{
	int i;

	if (sb_puts(buf, len, cap, ch->name) != 0 || sb_puts(buf, len, cap, ":\n") != 0) {
		return -1;
	}
	if (ch->ch_name[0]) {
		sb_field(buf, len, cap, "name", ch->ch_name, 1);
	}
	if (ch->description[0]) {
		sb_field(buf, len, cap, "description", ch->description, 1);
	}
	if (ch->sprite) {
		sb_field_int(buf, len, cap, "sprite", ch->sprite);
	}
	if (ch->sound) {
		sb_field_int(buf, len, cap, "sound", ch->sound);
	}
	if (ch->gold) {
		sb_field_int(buf, len, cap, "gold", ch->gold);
	}
	if (ch->driver) {
		sb_field_int(buf, len, cap, "driver", ch->driver);
	}
	if (ch->group) {
		sb_field_int(buf, len, cap, "group", ch->group);
	}
	if (ch->class_) {
		sb_field_int(buf, len, cap, "class", ch->class_);
	}
	if (ch->has_respawn) {
		sb_field_int(buf, len, cap, "respawn", ch->respawn);
	}
	if (ch->special_prob) {
		sb_field_int(buf, len, cap, "special_prob", ch->special_prob);
	}
	if (ch->special_str) {
		sb_field_int(buf, len, cap, "special_strength", ch->special_str);
	}
	if (ch->special_base) {
		sb_field_int(buf, len, cap, "special_base", ch->special_base);
	}
	if (ch->gold_prob) {
		sb_field_int(buf, len, cap, "gold_prob", ch->gold_prob);
	}
	if (ch->gold_base) {
		sb_field_int(buf, len, cap, "gold_base", ch->gold_base);
	}
	if (ch->gold_random) {
		sb_field_int(buf, len, cap, "gold_random", ch->gold_random);
	}
	if (ch->arg) {
		sb_field(buf, len, cap, "arg", ch->arg, 1);
	}

	for (i = 1; i < CF_COUNT; i++) {
		if (ch->flags & (1ull << i)) {
			sb_field(buf, len, cap, "flag", CF_tab[i], 0);
		}
	}
	for (i = 0; i < ZIO_V_MAX; i++) {
		if (ch->val_set[i]) {
			sb_field_int(buf, len, cap, V_tab[i], ch->val[i]);
		}
	}
	for (i = 0; i < ZIO_WN_MAX; i++) {
		if (ch->worn[i][0]) {
			sb_field(buf, len, cap, WN_tab[i], ch->worn[i], 0);
		}
	}
	for (i = 0; i < ch->item_count; i++) {
		sb_field(buf, len, cap, "item", ch->items[i], 0);
	}
	for (i = 0; i < ch->spell_count; i++) {
		sb_field(buf, len, cap, "spell", ch->spells[i], 0);
	}
	for (i = 0; i < ch->rand_count; i++) {
		sb_field_int(buf, len, cap, "rprob", ch->rand[i].prob);
		sb_field(buf, len, cap, "ritem", ch->rand[i].name, 0);
	}
	for (i = 0; i < ZIO_P_MAX; i++) {
		if (ch->prof_set[i]) {
			sb_field_int(buf, len, cap, P_tab[i], ch->prof[i]);
		}
	}
	return sb_puts(buf, len, cap, ";");
}

/* ------------------------------------------------------------- serialize */

char *zio_serialize(const zio_file *zf, size_t *out_len)
{
	char *buf = NULL;
	size_t len = 0, cap = 0;
	int i;

	for (i = 0; i < zf->seg_count; i++) {
		zio_seg *s = &zf->segs[i];
		if (s->type == ZSEG_RAW || !s->dirty) {
			if (sb_putn(&buf, &len, &cap, zf->text + s->start, s->end - s->start) != 0) {
				goto fail;
			}
		} else {
			int ri = s->rec_index;
			int rc = (zf->kind == ZIO_ITM) ? zio_item_render(&zf->items[ri], &buf, &len, &cap)
			                               : zio_char_render(&zf->chars[ri], &buf, &len, &cap);
			if (rc != 0) {
				goto fail;
			}
		}
	}
	if (!buf) {
		buf = malloc(1);
		if (!buf) {
			return NULL;
		}
		buf[0] = 0;
	}
	if (out_len) {
		*out_len = len;
	}
	return buf;

fail:
	free(buf);
	return NULL;
}

int zio_save(const zio_file *zf, const char *path)
{
	size_t len = 0;
	char *buf = zio_serialize(zf, &len);
	if (!buf) {
		return -1;
	}
	FILE *f = fopen(path, "wb");
	if (!f) {
		free(buf);
		return -1;
	}
	size_t wr = fwrite(buf, 1, len, f);
	fclose(f);
	free(buf);
	return (wr == len) ? 0 : -1;
}

/* ------------------------------------------------------------- map decode */

static int map_add_place(zio_map *m, int x, int y, int is_char, const char *name)
{
	if (m->place_count >= m->place_cap) {
		int nc = m->place_cap ? m->place_cap * 2 : 64;
		zio_placement *np = realloc(m->place, sizeof(zio_placement) * (size_t)nc);
		if (!np) {
			return -1;
		}
		m->place = np;
		m->place_cap = nc;
	}
	m->place[m->place_count].x = x;
	m->place[m->place_count].y = y;
	m->place[m->place_count].is_char = is_char;
	strncpy(m->place[m->place_count].name, name, ZIO_NAMELEN - 1);
	m->place[m->place_count].name[ZIO_NAMELEN - 1] = 0;
	m->place_count++;
	return 0;
}

int zio_map_parse(const zio_file *zf, zio_map *out, char *errbuf, size_t errbuf_len)
{
	char scratch[256];
	char *err = errbuf ? errbuf : scratch;
	size_t errn = errbuf ? errbuf_len : sizeof(scratch);

	if (zf->kind != ZIO_MAP) {
		snprintf(err, errn, "not a map file");
		return -1;
	}
	memset(out, 0, sizeof(*out));
	size_t cells = (size_t)ZIO_MAXMAP * ZIO_MAXMAP;
	out->gsprite = calloc(cells, sizeof(unsigned int));
	out->fsprite = calloc(cells, sizeof(unsigned int));
	out->flags = calloc(cells, sizeof(unsigned int));
	if (!out->gsprite || !out->fsprite || !out->flags) {
		zio_map_free(out);
		snprintf(err, errn, "out of memory");
		return -1;
	}

	zio_lexer lx = {zf->text, zf->text_len, 0};
	zio_tok t;
	int x = 0, y = 0, ox = 0, oy = 0;
	int xf = 0, yf = 0, xt = 0, yt = 0;
	int a, b, v;

	for (;;) {
		lex_next(&lx, &t);
		if (t.kind == ZTOK_EOF) {
			break;
		}
		if (t.kind == ZTOK_COMMENT) {
			continue;
		}
		if (t.kind != ZTOK_ASSIGN) {
			snprintf(err, errn, "unexpected token in map near byte %zu", t.start);
			goto fail;
		}

#define CELL(xx, yy) ((size_t)(xx) + (size_t)(yy) * ZIO_MAXMAP)
		if (!strcasecmp(t.name, "origin")) {
			if (!comma_parse(t.value, &a, &b)) {
				goto badcoord;
			}
			ox = a;
			oy = b;
		} else if (!strcasecmp(t.name, "field")) {
			if (!comma_parse(t.value, &a, &b)) {
				goto badcoord;
			}
			x = a + ox;
			y = b + oy;
			if (x < 0 || y < 0 || x >= ZIO_MAXMAP || y >= ZIO_MAXMAP) {
				snprintf(err, errn, "field %d,%d out of range", x, y);
				goto fail;
			}
			out->gsprite[CELL(x, y)] = 0;
			out->fsprite[CELL(x, y)] = 0;
			out->flags[CELL(x, y)] = 0;
		} else if (!strcasecmp(t.name, "from")) {
			if (!comma_parse(t.value, &a, &b)) {
				goto badcoord;
			}
			xf = a + ox;
			yf = b + oy;
		} else if (!strcasecmp(t.name, "to")) {
			if (!comma_parse(t.value, &a, &b)) {
				goto badcoord;
			}
			xt = a + ox;
			yt = b + oy;
			int cx, cy;
			for (cy = yf; cy <= yt; cy++) {
				for (cx = xf; cx <= xt; cx++) {
					if (cx < 0 || cy < 0 || cx >= ZIO_MAXMAP || cy >= ZIO_MAXMAP) {
						continue;
					}
					out->gsprite[CELL(cx, cy)] = out->gsprite[CELL(x, y)];
					out->fsprite[CELL(cx, cy)] = out->fsprite[CELL(x, y)];
					out->flags[CELL(cx, cy)] = out->flags[CELL(x, y)];
				}
			}
		} else if (!strcasecmp(t.name, "gsprite")) {
			out->gsprite[CELL(x, y)] = (unsigned int)atoi(t.value);
		} else if (!strcasecmp(t.name, "fsprite")) {
			out->fsprite[CELL(x, y)] = (unsigned int)atoi(t.value);
		} else if (!strcasecmp(t.name, "ch")) {
			if (map_add_place(out, x, y, 1, t.value) != 0) {
				goto oom;
			}
			/* Deliberately no MF_TMOVEBLOCK here. The server raises that flag at
			 * runtime for whatever tile a character currently occupies; it is
			 * never stored in a .map (no shipped zone file contains it). Setting
			 * it at parse time would serialize back out and permanently bake a
			 * runtime-only flag into the source file on every round-trip. */
		} else if (!strcasecmp(t.name, "it")) {
			if (map_add_place(out, x, y, 0, t.value) != 0) {
				goto oom;
			}
		} else if (!strcasecmp(t.name, "flag")) {
			v = zio_lookup_MF(t.value);
			if (v == -1) {
				snprintf(err, errn, "unknown MF \"%s\"", t.value);
				goto fail;
			}
			out->flags[CELL(x, y)] |= (1u << v);
		} else {
			snprintf(err, errn, "unknown map field \"%s\"", t.name);
			goto fail;
		}
#undef CELL
	}
	return 0;

badcoord:
	snprintf(err, errn, "expected two comma-separated numbers, got \"%s\"", t.value);
fail:
	zio_map_free(out);
	return -1;
oom:
	snprintf(err, errn, "out of memory");
	zio_map_free(out);
	return -1;
}

void zio_map_free(zio_map *m)
{
	if (!m) {
		return;
	}
	free(m->gsprite);
	free(m->fsprite);
	free(m->flags);
	free(m->place);
	memset(m, 0, sizeof(*m));
}

char *zio_map_serialize(const zio_map *m, size_t *out_len)
{
	char *buf = NULL;
	size_t len = 0, cap = 0;
	char tmp[64];
	int x, y, bit, p;
	size_t cells = (size_t)ZIO_MAXMAP * ZIO_MAXMAP;

	/* Index the first char/item placement on each cell so we can emit ch=/it=
	 * alongside that cell's terrain. */
	int *char_at = malloc(sizeof(int) * cells);
	int *item_at = malloc(sizeof(int) * cells);
	if (!char_at || !item_at) {
		free(char_at);
		free(item_at);
		return NULL;
	}
	for (size_t i = 0; i < cells; i++) {
		char_at[i] = -1;
		item_at[i] = -1;
	}
	for (p = 0; p < m->place_count; p++) {
		if (m->place[p].x < 0 || m->place[p].y < 0 || m->place[p].x >= ZIO_MAXMAP || m->place[p].y >= ZIO_MAXMAP) {
			continue;
		}
		size_t c = (size_t)m->place[p].x + (size_t)m->place[p].y * ZIO_MAXMAP;
		if (m->place[p].is_char) {
			if (char_at[c] < 0) {
				char_at[c] = p;
			}
		} else if (item_at[c] < 0) {
			item_at[c] = p;
		}
	}

	for (y = 0; y < ZIO_MAXMAP; y++) {
		for (x = 0; x < ZIO_MAXMAP; x++) {
			size_t c = (size_t)x + (size_t)y * ZIO_MAXMAP;
			unsigned int g = m->gsprite[c], fspr = m->fsprite[c], fl = m->flags[c];
			int ci = char_at[c], ii = item_at[c];
			if (!g && !fspr && !fl && ci < 0 && ii < 0) {
				continue;
			}

			snprintf(tmp, sizeof(tmp), "field=\"%d,%d\"\n", x, y);
			if (sb_puts(&buf, &len, &cap, tmp) != 0) {
				goto oom;
			}
			/* Emit as signed: the server parses sprite values with atoi() into a
			 * signed int, so sprites with the high bit set are stored negative in
			 * the source files. Writing %d (not %u) round-trips every 32-bit value. */
			if (g) {
				snprintf(tmp, sizeof(tmp), "gsprite=%d\n", (int)g);
				sb_puts(&buf, &len, &cap, tmp);
			}
			if (fspr) {
				snprintf(tmp, sizeof(tmp), "fsprite=%d\n", (int)fspr);
				sb_puts(&buf, &len, &cap, tmp);
			}
			if (ci >= 0) {
				sb_puts(&buf, &len, &cap, "ch=");
				sb_puts(&buf, &len, &cap, m->place[ci].name);
				sb_puts(&buf, &len, &cap, "\n");
			}
			if (ii >= 0) {
				sb_puts(&buf, &len, &cap, "it=");
				sb_puts(&buf, &len, &cap, m->place[ii].name);
				sb_puts(&buf, &len, &cap, "\n");
			}
			for (bit = 0; bit < MF_COUNT; bit++) {
				if (fl & (1u << bit)) {
					sb_puts(&buf, &len, &cap, "flag=");
					sb_puts(&buf, &len, &cap, MF_tab[bit]);
					sb_puts(&buf, &len, &cap, "\n");
				}
			}
			sb_puts(&buf, &len, &cap, "\n");
		}
	}

	free(char_at);
	free(item_at);
	if (!buf) {
		buf = malloc(1);
		if (!buf) {
			return NULL;
		}
		buf[0] = 0;
	}
	if (out_len) {
		*out_len = len;
	}
	return buf;

oom:
	free(char_at);
	free(item_at);
	free(buf);
	return NULL;
}

int zio_map_write(const zio_map *m, const char *path)
{
	size_t len = 0;
	char *buf = zio_map_serialize(m, &len);
	if (!buf) {
		return -1;
	}
	FILE *f = fopen(path, "wb");
	if (!f) {
		free(buf);
		return -1;
	}
	size_t wr = fwrite(buf, 1, len, f);
	fclose(f);
	free(buf);
	return (wr == len) ? 0 : -1;
}
