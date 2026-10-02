# Launcher translations

[Issue #127](https://github.com/omacom/try-omarchy-windows/issues/127) tracks translation of the Windows launcher's own interface. The Linux guest already receives the Windows locale, keyboard layout, and time zone; that does not translate the launcher or Omarchy's own menus. The [Try Omarchy website](https://tryomarchy.com/) and its guides are separate translation work.

The launcher ships English, Simplified Chinese (`zh-Hans`) and Korean (`ko`). `app/ui-locales/en.json` is the source catalog, and every window, dialog, menu, status line and message the launcher shows comes from it. A missing translation falls back to English, so a language can be finished over several pull requests. The launcher picks its language from Windows' preferred **UI languages**, which can differ from the regional-format setting.

## How the work is split

Maintainers move launcher text into `en.json` and say so in #127 when a batch lands. Translators only edit their language's file. A translation pull request changes `app/ui-locales/<language-tag>.json` and, in this file, which screens a fluent speaker checked.

## Translating

Run these from the `app/` folder. The first lists what your language still needs, in the same order as `en.json`, as a file you can translate:

```
go run ./cmd/translate missing ko > ko-todo.json
```

Translate the values in `ko-todo.json` and leave the keys alone. Leave a value empty to skip it for now. Then merge it:

```
go run ./cmd/translate merge ko ko-todo.json
```

`merge` checks every key and placeholder, writes `ui-locales/ko.json` in `en.json` order and prints how many messages are translated. It creates the file for a new language. Delete `ko-todo.json` afterwards; it is not part of the pull request. Without Go, editing the JSON by hand works too: add any key from `en.json` with your translation, in any order, and the pull request's checks report mistakes.

Language tags follow Windows: `ko`, `de`, `pt-BR`, `zh-Hans`. Traditional Chinese (`zh-Hant`) needs its own catalog; do not use one script as a fallback for the other.

### Keep these as they are

- Keys, and placeholders such as `{path}`, `{error}` and `{count}`. You can move a placeholder anywhere in the sentence.
- Product names: Try Omarchy, Omarchy, Windows, WINQ-EMU.
- File and folder names, because the launcher creates them in English: `Start Omarchy.cmd`, `Settings.cmd`, `TryOmarchy`, `Omarchy Shared`.
- Commands, flags, URLs and key names: `try-omarchy-export`, `-dir`, `tcp:2222:22`, `Super+Enter`, `Ctrl+Alt+F`.
- Sizes and numbers: sizes use GiB and MiB with a dot, such as `26.0 GiB`.

### Use the names Windows shows in your language

When a message names part of Windows, use the name Windows itself shows in your language: Control Panel > Power Options, Settings > Accounts > Passkeys, Disk Management, BitLocker, Windows Features > Windows Hypervisor Platform, Apps & features, File Explorer. The same goes for message box buttons. Windows draws Yes, No and Cancel in its own language, so a message such as "Yes: choose a backup. No: skip the full backup." should use the words on those buttons.

### Messages to know about

- Keys that start with `error.` are reasons shown under another sentence, such as "Try Omarchy could not finish this operation." In English they start with a lowercase letter. Write them as a reason that reads well after that sentence.
- Keys that start with `fatal.` appear when Try Omarchy cannot start. They often end with `{error}`, a technical detail from Windows or the launcher that may stay in English.
- Messages with a number come in variants where English needs them, such as `snapshots.status.none`, `.one` and `.many`. If your language does not change words for plurals, the variants can read the same. If it has more plural forms than English, phrase it so any number works, for example "Snapshots: {count}".
- Some menus inside Omarchy keep their English names. Leave those names as they are.
- Right-to-left languages are not supported yet; the windows lay out left to right.

### Where each message appears

| Key prefix | Where it shows |
| --- | --- |
| `setup`, `status`, `location`, `launcher`, `brand` | The setup window, first-launch questions, storage location, starter keys and the Launch Omarchy window |
| `settings`, `help` | Settings and its Help text |
| `about`, `update` | About and launcher updates |
| `tray`, `reclaim`, `camera`, `shutdown`, `control` | The tray menu and the messages it opens |
| `recovery`, `move`, `uninstall`, `preferences`, `snapshots`, `progress` | Backup, restore, snapshots, portable copy, reset, move and uninstall |
| `drop`, `transfer` | File transfers between Windows and Omarchy |
| `usb` | USB devices |
| `lan` | Add LAN forward |
| `install` | Install Omarchy on this PC |
| `picker`, `shortcut`, `shortcuts`, `share`, `startup`, `diagnostics` | File pickers, shortcuts, the shared folder, startup and diagnostics |
| `fatal`, `arch` | When Try Omarchy cannot start |
| `error` | Reasons shown under other messages |

## Checking a translation

The pull request's checks reject unknown keys and changed placeholders. To see your translation without changing Windows' display language, set `TRY_OMARCHY_UI_LANGUAGE` in PowerShell before starting the launcher:

```
$env:TRY_OMARCHY_UI_LANGUAGE = 'ko'
.\TryOmarchy.exe -settings
```

These open the other windows without starting Omarchy: `-about`, `-usb-selection -dir <data folder>`, and `-recovery snapshots`, `-recovery install-omarchy` or `-recovery uninstall` (Cancel at the first question changes nothing). The setup window appears when Omarchy starts; the tray menu, USB devices and file transfers need Omarchy running.

Check the windows at the normal text size and at a larger one (Settings > Accessibility > Text size), and look for text that is cut off.

Simplified Chinese came from a fluent contributor in [#246](https://github.com/omacom/try-omarchy-windows/pull/246); the Settings and About windows were checked on a real Windows machine at the normal text size. Korean came from a native speaker with AI drafting help in [#256](https://github.com/omacom/try-omarchy-windows/pull/256) and [#265](https://github.com/omacom/try-omarchy-windows/pull/265); the General page of Settings was checked on a real Windows machine at the normal text size. AI can draft text, but ask a fluent contributor to review installation, update, backup, reset and removal messages before presenting a language as supported.

## Moving text into the catalog

For maintainers and code contributors:

- Ask for messages with `uiText("key")`, or `uiTextWith("key", map[string]string{...})` when they have placeholders. Keys are literal strings so `go test` can check them: every key must be in `en.json`, `uiTextWith` must fill in exactly the placeholders its message has, and every message in `en.json` must still be used. `TestLauncherTextComesFromTheCatalog` fails when text shown to users is written into the code.
- Keep whole sentences in one message, with named placeholders for the parts that change. Do not build a sentence from fragments; word order differs between languages. Two versions of a sentence are better as two messages.
- Errors meant for users come from the catalog through `uiError(uiTextWith(...), cause)`, which still unwraps to the cause. Internal failures stay English and appear as `{error}` under a translated sentence; never show a bare `err.Error()`.
- When the meaning of an English message changes, give it a new key. The old translations then fall back to English instead of saying something outdated.
- Size controls from their text, not only for English. `measureText` and `buttonWidthFor` keep the English layout as the minimum and grow for longer translations.
- Check a screen with `TRY_OMARCHY_UI_LANGUAGE=qps-ploc`. Every catalog message then shows accented and about a third longer, so plain English text is text outside the catalog, and anything cut off has no room for a longer translation.
