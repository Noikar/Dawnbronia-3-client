/*
 * Part of Astonia content tooling (Dawnbronia). Please read license.txt.
 *
 * questgen - quest builder backend. Phase 3a: LiveQuest script emitter.
 *
 * Reads a quest definition in the tool's own clean JSON schema (deliberately NOT
 * the fragile .qst raw-struct dump) and emits a LiveQuest slash-command script.
 * Run by a GOD or LQ master on area 20 (or 35), that script builds the quest
 * live for instant playtest - zero server code, zero recompile. The same schema
 * is a superset designed to also feed the codegen-C backend (Phase 3b) that
 * bakes a permanent server driver.
 *
 * The emitted script is PURE commands (no comment lines). On area 20/35 the LQ
 * parser (lq.c special_driver) runs before the speech check, and an unrecognized
 * '#'/'/' line falls through to "Unknown command" (command.c). Comments would
 * therefore spam errors, so human-readable structure is written to stderr.
 *
 * NPC positions are set explicitly with "/npcpos <nick> <x> <y>" right after
 * "/npc" creates the template - so an NPC's tile never depends on where the
 * author happens to be standing (a "/goto"-then-"/npc" scheme is fragile: the
 * template is stamped with the author's live position, and "/nspawn" re-spawns
 * it there forever). Only the "/questentrance" still needs the author on the
 * tile (no explicit-coord form exists), so the script emits a single
 * "/goto <ex> <ey>" (an LQ-master command on area 20) before it; teleport is
 * instant. Facing is not settable by command (preview-only limitation); the
 * codegen backend (3b) takes an explicit facing.
 *
 * Usage:
 *   questgen <quest.json> [--out=<script.lqs>]
 *     Writes to stdout when --out is omitted.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ------------------------------------------------------------ JSON helpers - */

static const char *jstr(const cJSON *o, const char *k)
{
	cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
	return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

static int jint(const cJSON *o, const char *k, int def)
{
	cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
	return (it && cJSON_IsNumber(it)) ? it->valueint : def;
}

static int jhas(const cJSON *o, const char *k)
{
	return cJSON_GetObjectItemCaseSensitive(o, k) != NULL;
}

/* ------------------------------------------------------------ validation -- */
/* Non-fatal: warn on values the LQ engine would clamp or reject, but still emit
 * the command so the author can see and fix it in context. */

static int in_range(int v, int lo, int hi)
{
	return v >= lo && v <= hi;
}

static int known_npc_base(const char *b)
{
	return b && (!strcmp(b, "warrior") || !strcmp(b, "mage") || !strcmp(b, "seyan"));
}

static int known_item_base(const char *b)
{
	static const char *bases[] = {
	    "key", "torch", "bracelet", "ring", "necklace", "potion", "note", "apple", "flower", "mushroom", "berry"};
	size_t i;
	if (!b) {
		return 0;
	}
	for (i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
		if (!strcmp(b, bases[i])) {
			return 1;
		}
	}
	return 0;
}

/* Emit one command token, quoting it when it contains whitespace or is empty
 * (the LQ parser splits on spaces and unquotes "like this" strings). */
static void etok(FILE *f, const char *s)
{
	int q;
	const char *p;

	if (!s) {
		s = "";
	}
	q = (*s == 0);
	for (p = s; *p; p++) {
		if (*p == ' ' || *p == '\t') {
			q = 1;
			break;
		}
	}
	if (q) {
		fprintf(f, "\"%s\"", s);
	} else {
		fputs(s, f);
	}
}

/* Emit a "/npcitem" / "/npcrewarditem" line: <cmd> <nick> <base> <keyID> [name]
 * [desc]. keyID is always emitted (default 0) so an optional name/desc has a
 * slot to sit behind, matching the command grammar. */
static void emit_item(FILE *out, const char *cmd, const char *nick, const cJSON *item)
{
	const char *base = jstr(item, "base");
	const char *name, *desc;

	if (!base) {
		fprintf(stderr, "questgen: %s on %s has no \"base\", skipped\n", cmd, nick);
		return;
	}
	name = jstr(item, "name");
	desc = jstr(item, "description");

	if (!known_item_base(base)) {
		fprintf(stderr, "questgen: %s on %s: unknown item base \"%s\"\n", cmd, nick, base);
	}
	if (!in_range(jint(item, "keyID", 0), 0, 16777215)) {
		fprintf(stderr, "questgen: %s on %s: keyID out of range 0..16777215\n", cmd, nick);
	}

	fprintf(out, "%s %s %s %d", cmd, nick, base, jint(item, "keyID", 0));
	if (name || desc) {
		fputc(' ', out);
		etok(out, name ? name : "");
		fputc(' ', out);
		etok(out, desc ? desc : "");
	}
	fputc('\n', out);
}

/* ----------------------------------------------------------- LQ emitter --- */

static void emit_quest(FILE *out, const cJSON *root)
{
	cJSON *level, *ent, *npcs, *npc, *doors, *door, *rewards, *rw;
	const char *qname, *pw;

	/* Level range - required before /queststart. */
	level = cJSON_GetObjectItemCaseSensitive(root, "level");
	if (level) {
		int lmin = jint(level, "min", 1), lmax = jint(level, "max", 200);
		if (!in_range(lmin, 1, 200) || !in_range(lmax, 1, 200) || lmin > lmax) {
			fprintf(stderr, "questgen: warning: level range %d..%d invalid (want 1..200, min<=max)\n", lmin, lmax);
		}
		fprintf(out, "/questlevel %d %d\n", lmin, lmax);
	} else {
		fprintf(stderr, "questgen: warning: no \"level\"; /questlevel omitted (required before /queststart)\n");
	}

	/* Entrance - go to the tile, then stamp it. */
	ent = cJSON_GetObjectItemCaseSensitive(root, "entrance");
	if (ent) {
		fprintf(out, "/goto %d %d\n", jint(ent, "x", 0), jint(ent, "y", 0));
		fprintf(out, "/questentrance\n");
	} else {
		fprintf(stderr, "questgen: warning: no \"entrance\"; /questentrance omitted (required)\n");
	}

	/* NPC templates. */
	npcs = cJSON_GetObjectItemCaseSensitive(root, "npcs");
	cJSON_ArrayForEach(npc, npcs)
	{
		const char *nick = jstr(npc, "nick");
		const char *base = jstr(npc, "base");
		const char *mode = jstr(npc, "mode");
		const char *name, *desc, *greet;
		cJSON *pos, *replies, *rep, *item, *want, *rew;
		int rn;

		if (!nick || !base) {
			fprintf(stderr, "questgen: npc missing \"nick\" or \"base\", skipped\n");
			continue;
		}
		if (!mode) {
			mode = "p";
		}

		if (!known_npc_base(base)) {
			fprintf(stderr, "questgen: npc %s: unknown base \"%s\" (want warrior/mage/seyan)\n", nick, base);
		}
		if (mode[0] && mode[1]) {
			fprintf(stderr, "questgen: npc %s: mode \"%s\" should be a single char (a/n/p)\n", nick, mode);
		}
		if (!in_range(jint(npc, "level", 1), 1, 200)) {
			fprintf(stderr, "questgen: npc %s: level out of range 1..200\n", nick);
		}

		fprintf(out, "/npc %s %d %s %d %s\n", base, jint(npc, "level", 1), mode, jint(npc, "respawn", 0), nick);
		/* Position the template explicitly - independent of the author's tile. */
		pos = cJSON_GetObjectItemCaseSensitive(npc, "pos");
		if (pos) {
			fprintf(out, "/npcpos %s %d %d\n", nick, jint(pos, "x", 0), jint(pos, "y", 0));
		}

		name = jstr(npc, "name");
		if (name) {
			fprintf(out, "/npcname %s ", nick);
			etok(out, name);
			fputc('\n', out);
		}
		if (jhas(npc, "sprite")) {
			fprintf(out, "/npcsprite %s %d\n", nick, jint(npc, "sprite", 0));
		}
		desc = jstr(npc, "description");
		if (desc) {
			fprintf(out, "/npcdescription %s ", nick);
			etok(out, desc);
			fputc('\n', out);
		}
		if (jhas(npc, "gold")) {
			fprintf(out, "/npcgold %s %d\n", nick, jint(npc, "gold", 0));
		}
		greet = jstr(npc, "greeting");
		if (greet) {
			fprintf(out, "/npcgreeting %s ", nick);
			etok(out, greet);
			fputc('\n', out);
		}

		/* Replies: 5 slots, numbered 1-5. The server does nr-- then bounds-checks
		 * (lq.c cmd_npcreply), so slot 0 is rejected - the guide's "0-4" is wrong
		 * for this fork. */
		replies = cJSON_GetObjectItemCaseSensitive(npc, "replies");
		rn = 1;
		cJSON_ArrayForEach(rep, replies)
		{
			const char *trig = jstr(rep, "trigger");
			const char *text = jstr(rep, "reply");
			if (rn > 5) {
				fprintf(stderr, "questgen: npc %s has >5 replies; extras ignored\n", nick);
				break;
			}
			if (!trig || !text) {
				continue;
			}
			fprintf(out, "/npcreply %s %d ", nick, rn);
			etok(out, trig);
			fputc(' ', out);
			etok(out, text);
			fputc('\n', out);
			rn++;
		}

		/* Carried/fetch item, fetch-accept, and reward-in-exchange item. */
		item = cJSON_GetObjectItemCaseSensitive(npc, "item");
		if (item) {
			emit_item(out, "/npcitem", nick, item);
		}
		want = cJSON_GetObjectItemCaseSensitive(npc, "want_item");
		if (want) {
			fprintf(out, "/npcwantitem %s %d\n", nick, jint(want, "keyID", 0));
		}
		rew = cJSON_GetObjectItemCaseSensitive(npc, "reward_item");
		if (rew) {
			emit_item(out, "/npcrewarditem", nick, rew);
		}

		if (jhas(npc, "kill_mark")) {
			int m = jint(npc, "kill_mark", 0);
			if (!in_range(m, 1, 9)) {
				fprintf(stderr, "questgen: npc %s: kill_mark %d out of range 1..9\n", nick, m);
			}
			fprintf(out, "/npckillmark %s %d\n", nick, m);
		}
		if (jhas(npc, "hurt_mark")) {
			int m = jint(npc, "hurt_mark", 0);
			if (!in_range(m, 1, 9)) {
				fprintf(stderr, "questgen: npc %s: hurt_mark %d out of range 1..9\n", nick, m);
			}
			fprintf(out, "/npchurtmark %s %d\n", nick, m);
		}
	}

	/* Door locks. */
	doors = cJSON_GetObjectItemCaseSensitive(root, "doors");
	cJSON_ArrayForEach(door, doors)
	{
		const char *dn = jstr(door, "door");
		if (!dn) {
			continue;
		}
		fprintf(out, "/doorlock ");
		etok(out, dn);
		fprintf(out, " %d\n", jint(door, "keyID", 0));
	}

	/* Mark rewards. */
	rewards = cJSON_GetObjectItemCaseSensitive(root, "rewards");
	cJSON_ArrayForEach(rw, rewards)
	{
		const char *d = jstr(rw, "desc");
		int mark = jint(rw, "mark", 0), pct = jint(rw, "percent", 0);
		if (!in_range(mark, 1, 9)) {
			fprintf(stderr, "questgen: reward mark %d out of range 1..9\n", mark);
		}
		if (!in_range(pct, 1, 100)) {
			fprintf(stderr, "questgen: reward percent %d out of range 1..100\n", pct);
		}
		fprintf(out, "/questreward %d %d ", mark, pct);
		etok(out, d ? d : "");
		fputc('\n', out);
	}

	/* Bring the templates to life. */
	fprintf(out, "/nspawn all\n");

	/* Optional thrall waves: go to the template's tile, then drop copies. */
	cJSON_ArrayForEach(npc, npcs)
	{
		int tc = jint(npc, "thralls", 0);
		const char *nick;
		cJSON *pos;
		if (tc <= 0) {
			continue;
		}
		nick = jstr(npc, "nick");
		pos = cJSON_GetObjectItemCaseSensitive(npc, "pos");
		if (pos) {
			fprintf(out, "/goto %d %d\n", jint(pos, "x", 0), jint(pos, "y", 0));
		}
		fprintf(out, "/thrall %s %d\n", nick, tc);
	}

	/* Save (reusable later) then open the quest. */
	qname = jstr(root, "name");
	pw = jstr(root, "save_password");
	if (qname) {
		fprintf(out, "/questsave %s", qname);
		if (pw) {
			fprintf(out, " %s", pw);
		}
		fputc('\n', out);
	}
	fprintf(out, "/queststart\n");
}

/* --------------------------------------------------------------- main ---- */

int main(int argc, char **argv)
{
	const char *inpath = NULL, *outpath = NULL;
	const char *title;
	int i;
	FILE *f, *out;
	long sz;
	char *buf;
	cJSON *root;

	for (i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "--out=", 6)) {
			outpath = argv[i] + 6;
		} else if (argv[i][0] != '-') {
			inpath = argv[i];
		}
	}
	if (!inpath) {
		fprintf(stderr, "usage: questgen <quest.json> [--out=<script.lqs>]\n");
		return 2;
	}

	f = fopen(inpath, "rb");
	if (!f) {
		fprintf(stderr, "questgen: cannot open %s\n", inpath);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) {
		fclose(f);
		return 1;
	}
	buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return 1;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, "questgen: short read on %s\n", inpath);
		free(buf);
		fclose(f);
		return 1;
	}
	buf[sz] = 0;
	fclose(f);

	root = cJSON_Parse(buf);
	if (!root) {
		const char *e = cJSON_GetErrorPtr();
		fprintf(stderr, "questgen: JSON parse error near: %.40s\n", e ? e : "(unknown)");
		free(buf);
		return 1;
	}

	out = stdout;
	if (outpath) {
		out = fopen(outpath, "wb");
		if (!out) {
			fprintf(stderr, "questgen: cannot write %s\n", outpath);
			cJSON_Delete(root);
			free(buf);
			return 1;
		}
	}

	title = jstr(root, "title");
	if (!title) {
		title = jstr(root, "name");
	}
	fprintf(stderr, "questgen: LiveQuest script for \"%s\"\n", title ? title : "quest");
	fprintf(stderr, "  Run as GOD or LQ master on area 20 (or 35). Review the script before running.\n");

	emit_quest(out, root);

	if (out != stdout) {
		fprintf(stderr, "questgen: wrote %s\n", outpath);
		fclose(out);
	}
	cJSON_Delete(root);
	free(buf);
	return 0;
}
