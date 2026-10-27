# Achievement packs

xemu Modern GUI keeps achievements for each game in small JSON files, because the original Xbox had no achievements of its own. xemu can't see inside a game, so you tick them off yourself in the Achievements tab.

## Where they go

In the `achievements` folder next to your game library (the Achievements tab has an "Open the achievements folder" button), then press **Reload**.

A file's name starts with the game's **title ID** in 8 hex digits, for example `4D530004.json` or `4D530004-my-pack.json`. You can have several files for a game; their achievements are combined. (If the name doesn't start with the title ID, the file must say it in `"title_id"`.)

Your own achievements, made with "Add an achievement", are kept in `<TITLEID>-mine.json`.

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

Files over 1 MB and more than 500 achievements per game are ignored, and anything that isn't valid is skipped. Packs are only text that is shown; they can't do anything else.
