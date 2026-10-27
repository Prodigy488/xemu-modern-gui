# Achievement packs

> **Experimental / work in progress:** achievements are still being developed, so the pack format and the tab may change between versions.

xemu Modern GUI keeps achievements for each game in small JSON files, because the original Xbox had no achievements of its own. xemu can't see inside a game, so you tick them off yourself in the Achievements tab.

## Where they go

In the `achievements` folder next to your game library (the Achievements tab has an "Open the achievements folder" button), then press **Reload**.

A file's name starts with the game's **title ID** in 8 hex digits, for example `4D530004.json` or `4D530004-my-pack.json`. You can have several files for a game; their achievements are combined. (If the name doesn't start with the title ID, the file must say it in `"title_id"`.)

Your own achievements, made with "Add an achievement", are kept in `<TITLEID>-mine.json`.

## Writing good achievements

"Add an achievement" in the Achievements tab walks you through it, with hints in every box, a preview and a tips section. The short version:

- **Name:** short and catchy, up to 80 characters.
- **What to do:** say exactly what to do so anyone can tell when it's done. "Finish the first mission without dying" works; "Play well" doesn't. Put the level, difficulty, time limit or count in the text.
- **Group:** optional heading to keep related ones together (Story, Collectibles, Challenges...).
- **Points:** only for fun. About 5 for something quick, 10 for a normal goal, 25 for a hard one, 50 or more for the toughest.
- Avoid spoilers, or put them in a group called Spoilers. Mix easy and hard ones.

## Format

See [achievement-pack-example.json](achievement-pack-example.json).

| Field | Meaning |
|---|---|
| `title_id` | The game's title ID, 8 hex digits. Optional if it's in the file name. |
| `game`, `author` | Shown in the tab. Optional. |
| `achievements[].id` | A short unique name, kept in your progress. Don't change it once people use the pack. |
| `achievements[].name` | Required, up to 80 characters. |
| `achievements[].description` | What to do, up to 300 characters. |
| `achievements[].points` | 0 to 1000 (default 10). |
| `achievements[].group` | Optional heading to keep them in sections. |
| `achievements[].secret` | `true` hides the name and text until you tick it off. |

Files over 1 MB and more than 500 achievements per game are ignored, and anything that isn't valid is skipped. Packs can't run anything. The only thing a pack can ask xemu to do is *read* numbers from the running game (see below); it can never change them.

## Unlocking by itself (experimental)

By default you tick achievements off yourself. An achievement can also have a **trigger**: conditions about numbers in the running game's memory, such as "lives is 0" or "level is at least 5". xemu checks them about ten times a second while that game runs and unlocks the achievement when they hold. You can still tick it off yourself, and if you untick it, it stays off until you restart the game.

This is experimental. It only works for games someone has found the right addresses in, and the addresses can change between versions of a game (see `builds`).

### Finding the addresses

Open the Achievements tab, pick the game, press **Address finder (experimental)...** and start the game. It works like a cheat search:

1. Type a number the game shows (lives, level, score) and press **Search**. Or press **Search without a value** if you don't know it.
2. Change it in the game, type the new value and press **Equals** (or use **Changed**, **Unchanged**, **Increased**, **Decreased**).
3. Repeat until a few addresses are left, press **Watch** on them and keep playing to see which one follows your number.
4. Press **Use as a trigger**, pick one of your achievements and say when it should unlock ("is at least 5", "has gone down"...).

The finder only reads memory. It writes the trigger into your `<TITLEID>-mine.json`, together with the game's build, so it won't fire on a different version.

### Writing one by hand

Add a `trigger` to an achievement:

```json
{
  "id": "level5",
  "name": "Getting somewhere",
  "description": "Reach level 5.",
  "trigger": {
    "all": [ { "addr": "0x0045A3C0", "size": "u32", "op": ">=", "value": 5 } ],
    "hold": 3
  }
}
```

| Field | Meaning |
|---|---|
| `all` | List of conditions that must all hold. A trigger can also be just a list, which means `all`. |
| `any` | Optional list: at least one of these must hold as well. |
| `hold` | How many checks in a row (0.1 s each) the conditions must hold. 1 to 100, default 3. |
| `builds` | Optional list of the game's build stamps (`"0x3E8F12A0"`) the addresses are for. The finder fills this in. A pack can also put `builds` at its top level. Empty means any build. |

A condition:

| Field | Meaning |
|---|---|
| `addr` | The game's own address, as hex text (`"0x0045A3C0"`). Addresses in the game's XBE sections stay the same each run; memory the game allocates may not. |
| `size` | `u8`, `u16`, `u32` (unsigned), `i8`, `i16`, `i32` (signed) or `f32` (decimal). Default `u32`. |
| `op` | `==` (default), `!=`, `<`, `<=`, `>`, `>=`. |
| `value` | A number, or `"prev"` to compare with the value at the previous check (`">"` with `"prev"` means "has gone up"). A `"prev"` condition counts as true for the rest of the game once it has happened. |
| `mask` | Optional bits to look at, for flag words (`"0x04"` looks at one bit). Not for `f32`. |

Up to 16 conditions in each of `all` and `any`. A trigger that isn't valid is ignored and the achievement is ticked off by hand as usual. Triggers start 3 seconds after a game starts, and only watch games whose title ID matches the pack.

## Sharing

Packs other people have shared are listed in the [community-packs](../community-packs/) folder. In the app, **Browse community packs...** in the Achievements tab downloads them. To share yours, see that folder's README.

