# Community achievement packs

> **Experimental / work in progress:** achievements are still being developed, so the pack format and the tab may change between versions.

Achievement packs that people have shared for xemu Modern GUI. In the app, open the **Achievements** tab and press **Browse community packs...** to download them.

The original Xbox had no achievements of its own, so these are fan-made lists. xemu can't see inside a game: players tick them off themselves.

## Adding yours

1. Make a pack (see [docs/achievement-packs.md](../docs/achievement-packs.md)). The easiest way is "Add an achievement" in the app, which writes `<TITLEID>-mine.json` in your achievements folder.
2. Copy it to this folder and **rename it** so it starts with the game's title ID and does not end in `-mine`, for example `4D530004-halo-ce.json`. File names may only use letters, digits, `.`, `_` and `-`.
3. Add an entry to [index.json](index.json):

   ```json
   { "file": "4D530004-halo-ce.json", "title_id": "4D530004", "game": "Halo: Combat Evolved",
     "author": "Your name", "count": 12, "note": "Optional, one line." }
   ```

4. Open a pull request.

## Please

- Write your own achievements. Don't copy lists from Microsoft, TrueAchievements, RetroAchievements or other sites unless you have the right to share them.
- Keep names and descriptions short and spoiler-light, and mark spoilers `"secret": true`.
- No links, ads or anything unrelated to the game.

Packs are plain text that xemu shows; they can't run anything. The app also limits their size and checks every value before saving a download.
