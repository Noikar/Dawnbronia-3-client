/*
 * zoneio round-trip + semantic tests.
 *
 * Hermetic fixtures always run. If a zones directory is reachable (argv[1], or
 * the sibling server repo by default) every real .itm/.chr/.map in zones/1 is
 * round-tripped and must come back byte-for-byte identical.
 */

#include "../src/zoneio/zoneio.h"
#include "test.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

/* Read a whole file into a malloc'd, NUL-terminated buffer. */
static char *read_file(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	rewind(f);
	if (sz < 0) {
		fclose(f);
		return NULL;
	}
	char *buf = malloc((size_t)sz + 1);
	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[rd] = 0;
	if (out_len) {
		*out_len = rd;
	}
	return buf;
}

/* Parse text, serialize it back, and return 1 if byte-identical. */
static int round_trip_text(const char *text, size_t len, int kind)
{
	char err[256];
	zio_file *zf = zio_parse(text, len, kind, err, sizeof(err));
	if (!zf) {
		fprintf(stderr, "  parse failed: %s\n", err);
		return 0;
	}
	size_t out_len = 0;
	char *out = zio_serialize(zf, &out_len);
	int ok = out && out_len == len && memcmp(out, text, len) == 0;
	if (!ok) {
		fprintf(stderr, "  round-trip mismatch: in=%zu out=%zu\n", len, out_len);
		/* locate first differing byte */
		size_t i, n = (out_len < len) ? out_len : len;
		for (i = 0; i < n; i++) {
			if (out[i] != text[i]) {
				fprintf(stderr, "  first diff at byte %zu: '%c'(0x%02x) vs '%c'(0x%02x)\n", i, text[i],
				    (unsigned char)text[i], out[i], (unsigned char)out[i]);
				break;
			}
		}
	}
	free(out);
	zio_free(zf);
	return ok;
}

/* ------------------------------------------------------- hermetic fixtures */

static const char *FIX_ITM = "# an item file\n"
                             "\n"
                             "shrike_amulet1:\n"
                             "name=\"Crystal\"\n"
                             "description=\"A light blue crystal.\"\n"
                             "value=5\n"
                             "sprite=51618\n"
                             "flag=IF_TAKE\n"
                             "flag=IF_QUEST\n"
                             "flag=IF_USE\n"
                             "driver=118\n"
                             "arg=\"01\"\n"
                             ";\n"
                             "\n"
                             "sword1:\n"
                             "name=\"Short Sword\"\n"
                             "sprite=100\n"
                             "flag=IF_TAKE\n"
                             "flag=IF_WNRHAND\n"
                             "flag=IF_SWORD\n"
                             "mod_index=V_WEAPON\n"
                             "mod_value=10\n"
                             "req_index=V_STR\n"
                             "req_value=30\n"
                             ";\n";

static const char *FIX_CHR = "# monsters\n"
                             "direwolf:\n"
                             "name=\"Dire Wolf\"\n"
                             "description=\"A fierce looking dire wolf.\"\n"
                             "sprite=550\n"
                             "sound=1\n"
                             "flag=CF_RESPAWN\n"
                             "flag=CF_ALIVE\n"
                             "flag=CF_INFRARED\n"
                             "V_HP=80\n"
                             "V_ATTACK=90\n"
                             "V_PARRY=90\n"
                             "driver=7\n"
                             "arg=\"aggressive=1;helper=0;\"\n"
                             "group=2\n"
                             "#class=182\n"
                             ";\n";

static const char *FIX_MAP = "# map\n"
                             "field=\"0,0\"\n"
                             "gsprite=12000\n"
                             "fsprite=21411\n"
                             "flag=MF_SIGHTBLOCK\n"
                             "flag=MF_MOVEBLOCK\n"
                             "\n"
                             "field=\"1,0\"\n"
                             "gsprite=12001\n"
                             "from=\"1,0\"\n"
                             "to=\"3,0\"\n"
                             "\n"
                             "field=\"5,5\"\n"
                             "gsprite=99\n"
                             "ch=direwolf\n";

TEST(fixture_roundtrip)
{
	ASSERT_TRUE(round_trip_text(FIX_ITM, strlen(FIX_ITM), ZIO_ITM));
	ASSERT_TRUE(round_trip_text(FIX_CHR, strlen(FIX_CHR), ZIO_CHR));
	ASSERT_TRUE(round_trip_text(FIX_MAP, strlen(FIX_MAP), ZIO_MAP));
}

TEST(item_semantics)
{
	char err[256];
	zio_file *zf = zio_parse(FIX_ITM, strlen(FIX_ITM), ZIO_ITM, err, sizeof(err));
	ASSERT_PTR_NOT_NULL(zf);
	ASSERT_EQ_INT(2, zio_record_count(zf));

	zio_item *a = zio_item_at(zf, 0);
	ASSERT_PTR_NOT_NULL(a);
	ASSERT_TRUE(strcmp(a->name, "shrike_amulet1") == 0);
	ASSERT_TRUE(strcmp(a->item_name, "Crystal") == 0);
	ASSERT_EQ_INT(5, a->value);
	ASSERT_EQ_INT(51618, a->sprite);
	ASSERT_EQ_INT(118, a->driver);
	ASSERT_EQ_INT(1, a->has_arg);
	ASSERT_EQ_INT(1, a->arg_len);
	ASSERT_EQ_INT(1, a->arg[0]);
	/* IF_TAKE(3), IF_QUEST(26), IF_USE(4), plus IF_USED(0) */
	ASSERT_TRUE((a->flags & (1ull << zio_lookup_IF("IF_TAKE"))) != 0);
	ASSERT_TRUE((a->flags & (1ull << zio_lookup_IF("IF_QUEST"))) != 0);

	zio_item *b = zio_item_at(zf, 1);
	ASSERT_PTR_NOT_NULL(b);
	ASSERT_EQ_INT(2, b->mod_count);
	ASSERT_EQ_INT(zio_lookup_V("V_WEAPON"), b->mod_index[0]);
	ASSERT_EQ_INT(10, b->mod_value[0]);
	ASSERT_EQ_INT(-zio_lookup_V("V_STR"), b->mod_index[1]);
	ASSERT_EQ_INT(30, b->mod_value[1]);

	zio_free(zf);
}

TEST(char_semantics)
{
	char err[256];
	zio_file *zf = zio_parse(FIX_CHR, strlen(FIX_CHR), ZIO_CHR, err, sizeof(err));
	ASSERT_PTR_NOT_NULL(zf);
	ASSERT_EQ_INT(1, zio_record_count(zf));

	zio_char *c = zio_char_at(zf, 0);
	ASSERT_PTR_NOT_NULL(c);
	ASSERT_TRUE(strcmp(c->name, "direwolf") == 0);
	ASSERT_TRUE(strcmp(c->ch_name, "Dire Wolf") == 0);
	ASSERT_EQ_INT(550, c->sprite);
	ASSERT_EQ_INT(7, c->driver);
	ASSERT_EQ_INT(2, c->group);
	ASSERT_EQ_INT(80, c->val[zio_lookup_V("V_HP")]);
	ASSERT_EQ_INT(1, c->val_set[zio_lookup_V("V_HP")]);
	ASSERT_TRUE(c->arg != NULL && strcmp(c->arg, "aggressive=1;helper=0;") == 0);
	ASSERT_TRUE((c->flags & (1ull << zio_lookup_CF("CF_RESPAWN"))) != 0);

	zio_free(zf);
}

TEST(map_semantics)
{
	char err[256];
	zio_file *zf = zio_parse(FIX_MAP, strlen(FIX_MAP), ZIO_MAP, err, sizeof(err));
	ASSERT_PTR_NOT_NULL(zf);

	zio_map m;
	ASSERT_EQ_INT(0, zio_map_parse(zf, &m, err, sizeof(err)));

	size_t c00 = 0; /* 0,0 */
	size_t c30 = 3 + 0 * ZIO_MAXMAP; /* 3,0 filled by from/to */
	ASSERT_EQ_INT(12000, (int)m.gsprite[c00]);
	ASSERT_EQ_INT(21411, (int)m.fsprite[c00]);
	ASSERT_TRUE((m.flags[c00] & (1u << zio_lookup_MF("MF_MOVEBLOCK"))) != 0);
	/* field 1,0 had gsprite 12001; from 1,0 to 3,0 copies it across */
	ASSERT_EQ_INT(12001, (int)m.gsprite[c30]);
	/* one char placement (direwolf at 5,5) */
	ASSERT_EQ_INT(1, m.place_count);
	ASSERT_EQ_INT(5, m.place[0].x);
	ASSERT_EQ_INT(1, m.place[0].is_char);
	ASSERT_TRUE(strcmp(m.place[0].name, "direwolf") == 0);

	zio_map_free(&m);
	zio_free(zf);
}

/* A map decoded, serialized, and decoded again yields an identical grid and
 * placement set (the write path the editor uses to save edits). */
static int maps_grid_equal(const zio_map *a, const zio_map *b)
{
	size_t cells = (size_t)ZIO_MAXMAP * ZIO_MAXMAP, i;
	for (i = 0; i < cells; i++) {
		if (a->gsprite[i] != b->gsprite[i] || a->fsprite[i] != b->fsprite[i] || a->flags[i] != b->flags[i]) {
			fprintf(stderr, "  grid mismatch at cell %zu (%zu,%zu): g %u/%u  f %u/%u  fl %u/%u\n", i, i % ZIO_MAXMAP,
			    i / ZIO_MAXMAP, a->gsprite[i], b->gsprite[i], a->fsprite[i], b->fsprite[i], a->flags[i], b->flags[i]);
			return 0;
		}
	}
	return a->place_count == b->place_count;
}

TEST(map_write_roundtrip)
{
	char err[256];
	zio_file *zf = zio_parse(FIX_MAP, strlen(FIX_MAP), ZIO_MAP, err, sizeof(err));
	ASSERT_PTR_NOT_NULL(zf);
	zio_map m1;
	ASSERT_EQ_INT(0, zio_map_parse(zf, &m1, err, sizeof(err)));
	zio_free(zf);

	size_t len = 0;
	char *text = zio_map_serialize(&m1, &len);
	ASSERT_PTR_NOT_NULL(text);

	zio_file *zf2 = zio_parse(text, len, ZIO_MAP, err, sizeof(err));
	ASSERT_PTR_NOT_NULL(zf2);
	zio_map m2;
	ASSERT_EQ_INT(0, zio_map_parse(zf2, &m2, err, sizeof(err)));

	ASSERT_TRUE(maps_grid_equal(&m1, &m2));
	/* the direwolf placement at 5,5 survives */
	ASSERT_EQ_INT(1, m2.place_count);
	ASSERT_TRUE(strcmp(m2.place[0].name, "direwolf") == 0);

	free(text);
	zio_map_free(&m1);
	zio_map_free(&m2);
	zio_free(zf2);
}

/* Editing one record regenerates only that record; neighbors stay verbatim,
 * and the edit survives a re-parse. */
TEST(edit_roundtrip)
{
	char err[256];
	zio_file *zf = zio_parse(FIX_ITM, strlen(FIX_ITM), ZIO_ITM, err, sizeof(err));
	ASSERT_PTR_NOT_NULL(zf);

	zio_item *a = zio_item_at(zf, 0);
	a->value = 42;
	strncpy(a->item_name, "Blue Crystal", sizeof(a->item_name) - 1);
	zio_mark_dirty(zf, 0);

	size_t out_len = 0;
	char *out = zio_serialize(zf, &out_len);
	ASSERT_PTR_NOT_NULL(out);

	/* second record was untouched; its verbatim text must still be present */
	ASSERT_TRUE(strstr(out, "sword1:\nname=\"Short Sword\"\n") != NULL);

	/* re-parse and confirm the edit stuck without corrupting record 1 */
	zio_file *zf2 = zio_parse(out, out_len, ZIO_ITM, err, sizeof(err));
	ASSERT_PTR_NOT_NULL(zf2);
	ASSERT_EQ_INT(2, zio_record_count(zf2));
	zio_item *a2 = zio_item_at(zf2, 0);
	ASSERT_EQ_INT(42, a2->value);
	ASSERT_TRUE(strcmp(a2->item_name, "Blue Crystal") == 0);
	zio_item *b2 = zio_item_at(zf2, 1);
	ASSERT_EQ_INT(2, b2->mod_count);
	ASSERT_TRUE(strcmp(b2->name, "sword1") == 0);

	free(out);
	zio_free(zf2);
	zio_free(zf);
}

/* ----------------------------------------------------- real zone files ---- */

static int rt_files_in(const char *dir, const char *ext, int kind, int *checked)
{
	DIR *d = opendir(dir);
	if (!d) {
		return 1; /* directory absent: not a failure, just skipped */
	}
	struct dirent *de;
	int ok = 1;
	char path[1024];
	size_t elen = strlen(ext);
	while ((de = readdir(d))) {
		size_t nlen = strlen(de->d_name);
		if (nlen < elen || strcasecmp(de->d_name + nlen - elen, ext) != 0) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		size_t len = 0;
		char *text = read_file(path, &len);
		if (!text) {
			continue;
		}
		if (!round_trip_text(text, len, kind)) {
			fprintf(stderr, "  FAILED round-trip: %s\n", path);
			ok = 0;
		} else {
			(*checked)++;
		}
		free(text);
	}
	closedir(d);
	return ok;
}

static const char *g_zones_dir = "../astonia_community_server3/zones";

TEST(real_zone_files)
{
	char dir[1024];
	int checked = 0;

	/* zone 1 has the richest mix of .itm/.chr/.map */
	snprintf(dir, sizeof(dir), "%s/1", g_zones_dir);
	ASSERT_TRUE(rt_files_in(dir, ".itm", ZIO_ITM, &checked));
	ASSERT_TRUE(rt_files_in(dir, ".chr", ZIO_CHR, &checked));
	ASSERT_TRUE(rt_files_in(dir, ".map", ZIO_MAP, &checked));

	snprintf(dir, sizeof(dir), "%s/generic", g_zones_dir);
	ASSERT_TRUE(rt_files_in(dir, ".itm", ZIO_ITM, &checked));
	ASSERT_TRUE(rt_files_in(dir, ".chr", ZIO_CHR, &checked));

	fprintf(stderr, "  (real-file round-trip: %d files checked)\n", checked);
}

/* If the real zone 1 map is reachable, decode -> serialize -> decode it and
 * confirm the grid survives (exercises the write path on real data). */
TEST(real_map_write_roundtrip)
{
	char path[1024], err[256];
	snprintf(path, sizeof(path), "%s/1/above1.map", g_zones_dir);
	size_t len = 0;
	char *text = read_file(path, &len);
	if (!text) {
		return; /* not reachable: skip */
	}

	zio_file *zf = zio_parse(text, len, ZIO_MAP, err, sizeof(err));
	ASSERT_PTR_NOT_NULL(zf);
	zio_map m1;
	ASSERT_EQ_INT(0, zio_map_parse(zf, &m1, err, sizeof(err)));

	size_t out_len = 0;
	char *written = zio_map_serialize(&m1, &out_len);
	ASSERT_PTR_NOT_NULL(written);

	zio_file *zf2 = zio_parse(written, out_len, ZIO_MAP, err, sizeof(err));
	ASSERT_PTR_NOT_NULL(zf2);
	zio_map m2;
	ASSERT_EQ_INT(0, zio_map_parse(zf2, &m2, err, sizeof(err)));

	ASSERT_TRUE(maps_grid_equal(&m1, &m2));
	fprintf(stderr, "  (real map write round-trip: %d placements)\n", m1.place_count);

	free(text);
	free(written);
	zio_map_free(&m1);
	zio_map_free(&m2);
	zio_free(zf);
	zio_free(zf2);
}

int main(int argc, char *argv[])
{
	if (argc > 1) {
		g_zones_dir = argv[1];
	}

	fprintf(stderr, "Running tests...\n");
	fixture_roundtrip();
	item_semantics();
	char_semantics();
	map_semantics();
	map_write_roundtrip();
	edit_roundtrip();
	real_zone_files();
	real_map_write_roundtrip();
	fprintf(stderr, "\n=== Test Results ===\n");
	fprintf(stderr, "Tests run: %d\n", tests_run);
	fprintf(stderr, "Tests failed: %d\n", tests_failed);
	if (tests_failed == 0) {
		fprintf(stderr, "ALL TESTS PASSED\n");
	} else {
		fprintf(stderr, "SOME TESTS FAILED\n");
	}
	return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
