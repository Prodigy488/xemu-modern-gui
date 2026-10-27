# Achievement packs

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

Files over 1 MB and more than 500 achievements per game are ignored, and anything that isn't valid is skipped. Packs are only text that is shown; they can't do anything else.

## Sharing

Packs other people have shared are listed in the [community-packs](../community-packs/) folder. In the app, **Browse community packs...** in the Achievements tab downloads them. To share yours, see that folder's README.

