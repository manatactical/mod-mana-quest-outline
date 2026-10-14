# mod-mana-quest-outline

Server-side companion to the client's **wxl-mana-unit-outline** module.

That module draws a silhouette outline around a unit, but it can only select a unit the player
pointed at or targeted. It cannot tell that the NPC two steps away has a quest for you: the stock
3.3.5a UI exposes no quest state for a unit that is merely nearby (there is no
`C_QuestLog.GetUnitQuestInfo`), and the client-side API that does exist, `GetGossipAvailableQuests`
and friends, only answers for the NPC whose gossip window is already open.

The server already knows. `Player::GetQuestDialogStatus` returns a `QuestGiverStatus` per visible
object, and `SendQuestGiverStatusMultiple` walks the visible objects to feed the minimap. This module
computes the same answer and pushes it to the owning player over the addon channel — the only channel
a native client module can read.

## Protocol

An addon whisper, prefix **`WXLQ`**, always server → client. There is no request half.

```
server -> client:  WXLQ<TAB>S:<hi>:<lo>:<status>,<hi>:<lo>:<status>,...
                   WXLQ<TAB>S:                      (no givers in view at all)
```

| Field | Meaning |
|---|---|
| `S:` | the whole visible set follows; the client **replaces** its set with this, never merges |
| `<hi>:<lo>` | one GUID, `ObjectGuid::GetRawValue()` split into two 32-bit halves, hex, no `0x` |
| `<status>` | the `DIALOG_STATUS_*` value, decimal (`QuestDef.h`) |

Notes on the shape:

- **Two hex halves, not one 64-bit number.** The client-side script engine is Lua 5.1, where every
  number is a double; a packed 64-bit GUID cannot survive it. The halves are sent separately and the
  client joins them, exactly as it reads a GUID from the client's own object header.
- **`S:` only ever means replace.** The module re-resolves the set on movement and on the idle timer
  and never sends a delta, so a sync the client's chat layer drops or limits is corrected by the next
  resend and there is no client-side accumulation to keep consistent.
- **Append only.** Fields may be appended over time; an older client that cannot parse the tail keeps
  every field it does understand, so the two halves are only ever extended, never reordered or
  renumbered.
- The body is bounded by `ManaQuestOutline.MaxPerSync`, so it stays inside one addon message and is
  never split across packets.

### Only givers with something to do are sent

The statuses filtered through are the available and turn-in ones:

| Sent | `DIALOG_STATUS_*` |
|---|---|
| available | `AVAILABLE` (8), `AVAILABLE_REP` (7) |
| turn-in | `REWARD` (10), `REWARD2` (9), `REWARD_REP` (6) |

`UNAVAILABLE` (1), `NONE` (0) and `INCOMPLETE` (5) are dropped. This is both the requested behaviour
and what keeps the body small, because the client outlines everything it is told about.

The three **_LOW_LEVEL_ variants (2, 3, 4) are dropped too.** The server sets one when every quest the
giver has is far below the player: `GetQuestDialogStatus` keeps the highest status over all of them,
so a giver with one ordinary quest still reports `AVAILABLE`/`REWARD` and still gets an outline. A
giver whose only quests are low-level ones therefore gets none, which is the wanted behaviour and
needs no client-side setting.

## Settings

All in `conf/mod-mana-quest-outline.conf.dist`:

| Key | Default | Meaning |
|---|---|---|
| `ManaQuestOutline.Enable` | `1` | master switch |
| `ManaQuestOutline.PollIntervalMs` | `250` | floor between two movement-driven re-resolves of one player's set; clamped to 100 |
| `ManaQuestOutline.IdleIntervalMs` | `3000` | unconditional resend cadence, the safety net for changes no movement reports |
| `ManaQuestOutline.MoveDistance` | `5.0` | yards a player must move before movement polls the set again |
| `ManaQuestOutline.MaxPerSync` | `48` | givers per sync; the walk stops collecting at the cap |

## Install

Drop the folder into AzerothCore's `modules/` and build — the module is picked up by the core's own
CMake, there is no `CMakeLists.txt` here. Copy `mod-mana-quest-outline.conf.dist` to
`mod-mana-quest-outline.conf` next to `worldserver.conf` and edit it; every key has a working default,
so the module also runs with the file absent.

## Cost

The sync is change-driven, not a per-player timer:

- An **idle** player is never walked: `OnPlayerBeforeUpdate` compares their position against the one
  recorded with the last resolve and does nothing until they have covered `MoveDistance`. That is two
  ms subtractions and two float compares per update.
- A **moving** player is resolved at most once per `PollIntervalMs` (250 ms), and only then does the
  visibility walk and one `GetQuestDialogStatus` per visible quest giver run.
- The resulting set is compared with the last one sent and **not sent when unchanged**, so walking
  around a hub that is already outlined costs no traffic at all.
- `IdleIntervalMs` (3 s) forces a resend anyway, which covers the changes that no movement reports:
  handing a quest in, crossing a level or reputation threshold. A dropped message is corrected by that
  same cadence.
- A **zone change or teleport** resolves immediately, since the movement poll's window would otherwise
  hide it.

The work is per player and only for players in the world, so an empty or idle realm costs one clock
read per world tick. The walk stops collecting at `MaxPerSync` givers; note that it still visits every
visible object to find out which ones are givers, so the cap bounds the packet, not the iteration.

## Paired with

`wxl-mana-unit-outline` (client). That half registers the `WXLQ` prefix, parses the set, and outlines
the resulting units with its own `ColorQuestGiver` color. The two are versioned together: the protocol
above is the contract between them.