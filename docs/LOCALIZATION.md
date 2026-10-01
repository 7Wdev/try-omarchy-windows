# Launcher translations

[Issue #127](https://github.com/omacom/try-omarchy-windows/issues/127) tracks translation of the Windows launcher's own interface. The Linux guest already receives the Windows locale, keyboard layout, and time zone; that does not translate the launcher or Omarchy's own menus. The [Try Omarchy website](https://tryomarchy.com/) and its guides are separate translation work.

The launcher ships English, Simplified Chinese (`zh-Hans`) and Korean (`ko`). `app/ui-locales/en.json` is the source catalog. These come from it today: About and launcher updates, the first-launch questions, the setup window's buttons, every page of Settings with its messages, and the Help text. Other dialogs (recovery, backup, move, uninstall, transfers, USB, LAN forwarding), the tray menu, the setup window's progress text and most error messages are still English in the code. A missing translation falls back to English, and the launcher selects from Windows' preferred **UI languages**, which can differ from its regional-format setting.

## How the work is split

Maintainers move launcher text into `en.json`, a screen at a time, and say so in #127 when a batch lands. Translators then only edit their language's file. A translation pull request should change `app/ui-locales/<language-tag>.json` and, in this file, which screens a fluent speaker checked. Help moving text into the catalog is welcome as its own pull request without translations, so it can be reviewed as code.

## Translating

To add a language, copy the English catalog to `app/ui-locales/<language-tag>.json`, then translate the values. Keep keys, placeholders such as `{version}`, product names, commands, URLs, and the actual names of untranslated Omarchy menus intact. The catalog loader rejects unknown keys and changed placeholders. A key that is missing or empty uses English, so a language can be finished over several pull requests.

To see what a language still needs, run this from `app/`:

```
go test -run TestLauncherTranslationCoverage -v
```

To check a translation without changing Windows' display language, set `TRY_OMARCHY_UI_LANGUAGE` before starting the launcher:

```
$env:TRY_OMARCHY_UI_LANGUAGE = 'ko'
.\TryOmarchy.exe -settings
```

Check the windows on a real Windows machine at normal and enlarged text sizes. Simplified Chinese came from a fluent contributor in [#246](https://github.com/omacom/try-omarchy-windows/pull/246). The Settings and About windows were checked on a real Windows machine at the normal text size; enlarged text sizes and the first-launch questions were not. Korean came from a native speaker with AI drafting help in [#256](https://github.com/omacom/try-omarchy-windows/pull/256); it has not yet been checked on a real Windows machine. Traditional Chinese (`zh-Hant`) needs its own catalog; do not use one script as a fallback for the other. AI can draft text, but ask a fluent contributor to review installation, update, backup, reset, and removal messages before presenting a language as supported. Record which screens were reviewed and tested.

## Moving text into the catalog

- Ask for messages with `uiText("key")`, or `uiTextWith("key", map[string]string{...})` when they have placeholders. Keys are literal strings so `go test` can check them: every key the code asks for must be in `en.json`, `uiTextWith` must fill in exactly the placeholders its message has, and every message in `en.json` must still be used.
- Keep whole sentences in one message, with named placeholders for the parts that change. Do not build a sentence from fragments; word order differs between languages. Two versions of a sentence are better as two messages.
- Leave error details from Windows or the guest as they are; put the sentence around them in the catalog with an `{error}` placeholder.
- Size controls from their text, not only for English. Settings measures text with `measureText`: labels widen their column, paragraphs and checkboxes grow taller and move the rows under them down, and buttons widen.
- Check a screen with `TRY_OMARCHY_UI_LANGUAGE=qps-ploc`. Every catalog message then shows accented and about a third longer, so plain English text is text still outside the catalog, and anything cut off has no room for a longer translation.
