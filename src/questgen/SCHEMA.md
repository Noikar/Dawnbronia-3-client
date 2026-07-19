# Quest schema (questgen)

The quest builder's own clean JSON — deliberately **not** the fragile `.qst` raw-struct dump. One
schema feeds two backends:

- **Phase 3a (built):** `questgen <quest.json> [--out=script.lqs]` emits a **LiveQuest slash-command
  script**. Run by a GOD or LQ master on **area 20** (or 35), it builds the quest live for instant
  playtest — zero server code, zero recompile.
- **Phase 3b (planned):** the same schema bakes into a permanent `caligar.c`-style server driver.

The schema is a **superset** of what each backend needs, so an event authored for LQ preview can be
promoted to a permanent quest by baking it.

## Top level

| Field | Type | Notes |
|---|---|---|
| `name` | string | Save name (letters only for `/questsave`). Emits `/questsave <name> [password]`. |
| `title` | string | Human label (stderr only; not sent to the game). |
| `save_password` | string | Optional `/questsave` password (plain text; guards overwrites only). |
| `level` | `{min,max}` | 1–200, `min<=max`. Emits `/questlevel`. **Required before `/queststart`.** |
| `entrance` | `{x,y}` | Player arrival tile. Emits `/goto x y` + `/questentrance`. **Required.** |
| `rewards` | array | Mark→XP payouts (below). |
| `doors` | array | Door locks (below). |
| `npcs` | array | NPC templates (below). |

## `rewards[]`

| Field | Type | Notes |
|---|---|---|
| `mark` | int 1–9 | The mark players earn from kill/hurt tags. |
| `percent` | int 1–100 | Percent of the level-scaled XP cap this mark pays. Total across marks ≤ 100. |
| `desc` | string | Shown in the reward summary. |

## `doors[]`

| Field | Type | Notes |
|---|---|---|
| `door` | string | Door nick from `/doorlist` (e.g. `"Palace, Throneroom"`) or its number. |
| `keyID` | int | Lock with this keyID; a matching-key item opens it. `0` unlocks. |

## `npcs[]`

| Field | Type | Notes |
|---|---|---|
| `nick` | string | Handle used by every other command. **Required.** |
| `base` | string | `warrior` \| `mage` \| `seyan`. **Required.** |
| `level` | int 1–200 | Auto-scaled to a real level-N foe. |
| `mode` | `"a"`\|`"n"`\|`"p"` | Aggressive / Non-combat (immortal giver) / Passive. Default `p`. |
| `respawn` | int | Seconds to auto-respawn after death; `0` = never. |
| `pos` | `{x,y}` | Template tile. Emits `/goto x y` before `/npc`. |
| `name` | string | Display name. |
| `sprite` | int | Appearance (see guide §8). |
| `description` | string | Look-at text. |
| `gold` | int 0–2000 | Loot gold, hundredths (2000 = 20G). |
| `greeting` | string | Said once per player within 12 tiles. |
| `replies` | array | Up to **5** `{trigger, reply}`. Reply fires when a player's speech (≤10 tiles) contains `trigger` (case-insensitive substring). |
| `item` | item obj | Item the NPC carries/drops (the fetch object or loot). |
| `want_item` | `{keyID}` | NPC accepts & consumes a handed-over LQ item with this keyID. |
| `reward_item` | item obj | Item handed back in exchange for the wanted item. |
| `kill_mark` | int 1–9 | Killing this NPC grants the mark. |
| `hurt_mark` | int 1–9 | Damaging this NPC grants the mark. |
| `thralls` | int 1–20 | Mass-spawn this many extra copies (fire-and-forget, no dialogue). |

### item object (`item`, `reward_item`)

| Field | Type | Notes |
|---|---|---|
| `base` | string | `key`,`torch`,`bracelet`,`ring`,`necklace`,`potion`,`note`,`apple`,`flower`,`mushroom`,`berry`. |
| `keyID` | int 0–16777215 | Ties fetch chains and door locks together. `0` = none. |
| `name` | string | Display name. |
| `description` | string | Look-at text (a `note`'s letter body goes here). |

## Notes & limitations

- **Positioning:** `/npc`, `/questentrance`, and `/thrall` act at the author's live position, so the
  script `/goto`s to each `pos` first — hence the reliance on `/goto` (an LQ-master command on
  **area 20**; on area 35 a non-god master lacks it). **NPC facing** cannot be set by command, so a
  previewed NPC faces whatever results after the goto. The 3b codegen backend takes an explicit
  facing.
- **Fetch turn-ins pay no mark** (LQ engine limitation): `want_item`/`reward_item` exchange items
  only. To reward XP for a delivery, tag a kill/hurt mark elsewhere.
- The emitted `.lqs` is **pure commands** (no comment lines): on area 20/35 an unrecognized `#`/`/`
  line prints "Unknown command", so comments would spam. Read the stderr summary for structure.

See `examples/ratquest.json` for the guide's worked example expressed in this schema.

## Binary `.qst` path (`gen_qst.py` / `verify_qst.py`)

Besides the `.lqs` command script, `gen_qst.py` writes a schema **directly into a `.qst` binary**
(the format `/questload` reads), so you can reload a whole quest at once instead of pasting commands
line by line. `verify_qst.py` dumps a `.qst` back for checking.

```
python gen_qst.py    examples/ratquest.json  <server>/quest/ratquest.qst   # write
python verify_qst.py <server>/quest/ratquest.qst                            # read back
```

`gen_qst.py` scaffolds off an existing save (keeps its `password` + `lq_door` section — so make one
real `/questsave <name> <pw>` first), then rewrites `lq_data` + `lq_npc[]`.

**Layout gotcha that cost a live debug session:** `struct character.description` is `LENDESC = 160`,
NOT 80 — every `lq_npc` field after `description` (nick, greeting, replies, marks, item structs) is
80 bytes later than a naive guess. `struct lq_item = base[40] name[40] description[80] uint keyID`,
and `create_lq_item` does `create_item("lq_%s", base)`, so **`base` must be a real lq template**
(`key`/`potion`/`torch`/`note`/…) and `name` is only the display label. Getting `base`/`name` swapped
fails **silently in the client** but logs `create_item(): could not find item "lq_<name>"` in
`docker logs astonia3-server`. Full offset crib is in each script's header. (This binary path is
fragile; the cleanest way to persist a working quest is an in-game `/questsave`, which the server
serializes with the correct layout. `gen_qst.py` exists for authoring from the schema before a live
session. Folding it into `questgen` as `--emit=qst` is a future 3a-Level-B nicety.)
