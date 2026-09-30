"""The list of groups to pick from, and --select parsing, shared by import and export."""

from . import classify
from .ui import human_size

RESERVE_BYTES = 2 * 1000 ** 3
SMALL_APP_BYTES = 100 * 1000 ** 2


class SelectionError(ValueError):
    pass


def option_rows(inventory, package_plan, theme, free):
    """(id, label, default) rows for the selection list, defaults sized to fit."""
    rows = []
    budget = free - RESERVE_BYTES
    groups = inventory.ordered()
    for group in groups:
        if group.kind == classify.SETTINGS:
            budget -= group.bytes
    for group in groups:
        size = human_size(group.bytes)
        if group.kind == classify.SETTINGS:
            rows.append((group.id, f"Settings and customizations ({group.changed} files {size})",
                         True))
        elif group.kind == classify.FILES:
            default = group.bytes <= budget
            if default:
                budget -= group.bytes
            label = group.label if group.id == classify.LOOSE_FILES_GROUP else f"{group.label} folder"
            rows.append((group.id, f"{label} ({size})", default))
        elif group.kind == classify.APPS:
            default = group.bytes <= SMALL_APP_BYTES and group.bytes <= budget
            if default:
                budget -= group.bytes
            rows.append((group.id, f"{group.label} app data ({size})", default))
        elif group.kind == classify.BROWSER:
            rows.append((group.id, f"{group.label} profile with bookmarks and saved logins ({size})",
                         False))
        elif group.kind == classify.KEYS:
            rows.append((group.id, "Keys and sign-ins: SSH and GPG keys, keyring, command line "
                                   f"logins ({group.changed} files)", False))
    if package_plan is not None and not package_plan.empty():
        count = len(package_plan.repo) + len(package_plan.aur) + len(package_plan.flatpaks)
        rows.append(("packages", f"Apps you installed ({count} to install)", True))
    if theme:
        rows.append(("theme", f"Theme and background ({theme})", True))
    return rows


def resolve_selection(text, rows):
    known = {row[0] for row in rows}
    chosen = set()
    for token in (part.strip() for part in text.split(",")):
        if not token:
            continue
        if token == "all":
            chosen.update(known)
        elif token == "defaults":
            chosen.update(row[0] for row in rows if row[2])
        elif token == "none":
            continue
        elif token in ("settings", "keys", "packages", "theme"):
            chosen.update(row_id for row_id in known if row_id == token)
        elif token in ("files", "apps", "browser"):
            chosen.update(row_id for row_id in known if row_id.startswith(token + "/"))
        elif token in known:
            chosen.add(token)
        else:
            raise SelectionError(f"unknown group {token}; run with --dry-run --json to list them")
    return chosen
