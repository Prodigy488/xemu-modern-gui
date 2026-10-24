# Interface translations

The dashboard and settings are written in English and translated with
`tr("English text")` (see `ui/xui/i18n.hh`).

- `strings.json`: every marked string, with where it's used. Made by
  `scripts/xui-i18n.py extract`.
- `<code>.json`: one language, `{"English": "Translation"}`.
- `../i18n-tables.inc`: the translations compiled into xemu. Made by
  `scripts/xui-i18n.py gen`, which also checks them.

## Fixing or adding a translation

1. Edit `<code>.json`. Keep `%s`, `%d`, `%.1f`... in the same order as in the
   English, and keep line breaks (`\n`). Leave out a string to show the
   English one.
2. Run `scripts/xui-i18n.py gen` (it reports anything it had to leave out).
3. Rebuild xemu.

## Adding a language

Add it to `LANGS` in `scripts/xui-i18n.py` and to the list in `i18n.cc`
(code, name in the language itself, name in English). Japanese, Chinese and
Korean text uses a font Windows already has (see `XuiLanguageFontPath`).

The translations were made with AI help and may have mistakes; corrections
from native speakers are welcome.
