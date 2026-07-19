/*
 * Part of Astonia content tooling (Dawnbronia). Please read license.txt.
 *
 * zoneio - lossless round-trip reader/writer for Astonia v3 zone content files:
 *   .itm  item templates
 *   .chr  character / NPC / monster templates
 *   .map  tile maps (grid + char/item placements)
 *
 * This mirrors the server's authoritative parser (create.c: get_text,
 * process_item/process_char/process_map and the V_/P_/WN_/IF_/CF_/MF_ token
 * tables). Anything zoneio writes, the server must be able to load; and a file
 * loaded and written back unchanged is byte-identical.
 *
 * The module is deliberately free of any client, SDL or server dependency so it
 * can also be compiled into the server-side quest generator.
 */

#ifndef ZONEIO_H
#define ZONEIO_H

#include <stddef.h>

/* ------------------------------------------------------------------ limits */
/* These mirror the server (server.h / create.c). Kept local so zoneio has no
 * external headers. */
#define ZIO_V_MAX       43 /* number of V_* stat tokens (create.c V_tab)   */
#define ZIO_P_MAX       20 /* number of P_* profession slots               */
#define ZIO_WN_MAX      12 /* number of WN_* worn slots                    */
#define ZIO_MAXMOD      5 /* item modifier/requirement slots (MAXMOD)     */
#define ZIO_IT_DR_SIZE  40 /* item driver-data bytes (IT_DR_SIZE)          */
#define ZIO_MAXRANDITEM 10 /* random-drop slots on a char (MAXRANDITEM)    */
#define ZIO_MAXMAP      256 /* map grid dimension (MAXMAP)                   */
#define ZIO_NAMELEN     80 /* template identifier / lookup-name buffer     */

/* ------------------------------------------------------------ token lookups */
/* Return the bit index for a flag token, or -1 if unknown. Case-insensitive,
 * matching the server's strcasecmp lookups. The reverse functions return the
 * canonical token string for a bit index, or NULL if out of range. */
int zio_lookup_MF(const char *name);
int zio_lookup_CF(const char *name);
int zio_lookup_IF(const char *name);
int zio_lookup_V(const char *name);
int zio_lookup_WN(const char *name);
int zio_lookup_P(const char *name);

const char *zio_name_MF(int bit);
const char *zio_name_CF(int bit);
const char *zio_name_IF(int bit);
const char *zio_name_V(int idx);
const char *zio_name_WN(int idx);
const char *zio_name_P(int idx);

/* --------------------------------------------------------------- item model */
typedef struct {
	char name[ZIO_NAMELEN]; /* template identifier (lookup name) */
	char item_name[40]; /* "name" field (display name)       */
	char description[80]; /* "description" field               */
	int value;
	int sprite;
	int driver;
	int min_level;
	int max_level;
	int needs_class;
	unsigned int ID; /* "ID" field, hex in the file       */
	int has_ID; /* whether an ID= was present        */
	unsigned char arg[ZIO_IT_DR_SIZE];
	int arg_len; /* bytes decoded from "arg" hex      */
	int has_arg;
	/* Modifier/requirement pairs. index holds a V_* index; a requirement is
	 * stored negated (matching create.c). count is the number of used slots. */
	int mod_index[ZIO_MAXMOD];
	int mod_value[ZIO_MAXMOD];
	int mod_count;
	unsigned long long flags; /* IF_* bitmask                      */
} zio_item;

/* --------------------------------------------------------------- char model */
typedef struct {
	int prob; /* rprob (out of 10000) */
	char name[ZIO_NAMELEN]; /* ritem template name  */
} zio_randitem;

typedef struct {
	char name[ZIO_NAMELEN]; /* template identifier (lookup name)      */
	char ch_name[40]; /* "name" field (display name)            */
	char description[80];
	int sprite;
	int sound;
	int gold;
	int driver;
	int group;
	int class_; /* "class" field (class is a C keyword)   */
	int respawn; /* raw seconds as written in the file     */
	int has_respawn;
	int special_prob;
	int special_str;
	int special_base;
	int gold_prob;
	int gold_base;
	int gold_random;
	char *arg; /* driver arg string (owned, may be NULL) */

	int val[ZIO_V_MAX]; /* V_* base stats (value[1][] layer)      */
	unsigned char val_set[ZIO_V_MAX];
	int prof[ZIO_P_MAX]; /* P_* profession levels                  */
	unsigned char prof_set[ZIO_P_MAX];
	char worn[ZIO_WN_MAX][ZIO_NAMELEN]; /* WN_* slot -> item name ("" none) */

	char (*items)[ZIO_NAMELEN]; /* "item=" inventory entries (owned)      */
	int item_count;
	char (*spells)[ZIO_NAMELEN]; /* "spell=" entries (owned)               */
	int spell_count;
	zio_randitem rand[ZIO_MAXRANDITEM];
	int rand_count;

	unsigned long long flags; /* CF_* bitmask                           */
} zio_char;

/* --------------------------------------------------------------- map model */
/* A char or item instance placed on a tile by a "ch=" / "it=" directive. */
typedef struct {
	int x, y;
	int is_char; /* 1 = ch=, 0 = it=          */
	char name[ZIO_NAMELEN]; /* referenced template name  */
} zio_placement;

typedef struct {
	/* Dense per-tile terrain, indexed x + y * ZIO_MAXMAP. Allocated lazily. */
	unsigned int *gsprite; /* NULL until zio_map_parse()  */
	unsigned int *fsprite;
	unsigned int *flags; /* MF_* bitmask                */
	zio_placement *place;
	int place_count;
	int place_cap;
} zio_map;

/* ------------------------------------------------------------ file container */
/* A parsed content file. Losslessness is achieved by splitting the original
 * text into an ordered list of segments: raw pass-through spans (comments,
 * blank lines, whitespace, and the whole body of a .map) plus decoded template
 * records. Serializing re-emits raw spans verbatim and only regenerates a
 * record when it has been marked dirty, so an untouched file round-trips
 * byte-for-byte. */
typedef enum { ZIO_ITM, ZIO_CHR, ZIO_MAP } zio_kind;

typedef struct zio_file zio_file;

/* Load/parse a file from disk. kind is inferred from the extension when
 * ZIO_KIND_AUTO is passed. Returns NULL on I/O or parse error; when errbuf is
 * non-NULL a human-readable reason is written there. */
#define ZIO_KIND_AUTO (-1)
zio_file *zio_load(const char *path, int kind, char *errbuf, size_t errbuf_len);
zio_file *zio_parse(const char *text, size_t len, int kind, char *errbuf, size_t errbuf_len);

void zio_free(zio_file *zf);

zio_kind zio_file_kind(const zio_file *zf);

/* Serialize the (possibly edited) file back to text. The returned buffer is
 * heap-allocated and NUL-terminated; *out_len receives the byte length
 * (excluding the terminator). Caller frees with free(). */
char *zio_serialize(const zio_file *zf, size_t *out_len);

/* Write serialized content straight to disk. Returns 0 on success. */
int zio_save(const zio_file *zf, const char *path);

/* -------------------------------------------------------- record accessors */
/* Number of decoded template records (ZIO_ITM / ZIO_CHR files). */
int zio_record_count(const zio_file *zf);

/* Borrowed pointers to a decoded record (NULL if index/kind mismatch). Edits
 * made through these pointers take effect only after zio_mark_dirty(). */
zio_item *zio_item_at(zio_file *zf, int index);
zio_char *zio_char_at(zio_file *zf, int index);

/* Mark a record changed so serialize regenerates it instead of emitting the
 * original span. */
void zio_mark_dirty(zio_file *zf, int index);

/* Decode a .map file's grid + placements into caller-supplied storage. The
 * zio_map's arrays are heap-allocated; free with zio_map_free(). Returns 0 on
 * success. */
int zio_map_parse(const zio_file *zf, zio_map *out, char *errbuf, size_t errbuf_len);
void zio_map_free(zio_map *m);

/* Serialize an (edited) map grid + placements to canonical .map text. Unlike
 * the record files, a map is emitted freshly (one field block per non-empty
 * cell) rather than preserved verbatim, since the editor mutates the decoded
 * grid; the result is semantically identical and the server loads it the same.
 * The returned buffer is heap-allocated and NUL-terminated; caller frees.
 * zio_map_write() serializes straight to disk (0 on success). */
char *zio_map_serialize(const zio_map *m, size_t *out_len);
int zio_map_write(const zio_map *m, const char *path);

/* Render one item/char record to its canonical text form (used by the codegen
 * generator and the "new template" path). Appends to *buf, growing it; caller
 * frees *buf. Returns 0 on success. */
int zio_item_render(const zio_item *it, char **buf, size_t *len, size_t *cap);
int zio_char_render(const zio_char *ch, char **buf, size_t *len, size_t *cap);

#endif /* ZONEIO_H */
