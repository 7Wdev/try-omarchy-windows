"""Human and JSON summaries of plans and results."""

import json

from .ui import by_count, human_size

DESCRIPTIONS = {
    "create": "copy",
    "replace": "replace",
    "merge": "merge",
    "conflict": "changed here too",
    "mkdir": "new folder",
    "replace-profile": "replace profile",
}


def plan_summary(plan):
    counts = plan.counts()
    lines = []
    copies = counts.get("create", 0)
    replaced = counts.get("replace", 0)
    merged = counts.get("merge", 0)
    if copies:
        lines.append(by_count(copies, "1 new file or link will be copied",
                              "{n} new files and links will be copied"))
    if replaced:
        lines.append(by_count(replaced, "1 of Omarchy's defaults will be replaced with your version",
                              "{n} of Omarchy's defaults will be replaced with your versions"))
    if merged:
        lines.append(by_count(merged, "1 file will combine your changes with this computer's",
                              "{n} files will combine your changes with this computer's"))
    conflicts = plan.conflicts()
    if conflicts:
        if plan.resolution == "trial":
            lines.append(by_count(
                len(conflicts),
                "1 file you already changed on this computer will be replaced by the trial's "
                "(this computer's copy goes to the backup)",
                "{n} files you already changed on this computer will be replaced by the trial's "
                "(this computer's copies go to the backup)"))
        else:
            lines.append(by_count(
                len(conflicts),
                "1 file you already changed on this computer is kept; the trial's copy is saved "
                "next to it",
                "{n} files you already changed on this computer are kept; the trial's copies are "
                "saved next to them"))
    already = counts.get("same", 0) + counts.get("imported", 0)
    if already:
        lines.append(by_count(already, "1 is already the same here", "{n} are already the same here"))
    unchanged = counts.get("default", 0)
    if unchanged:
        lines.append(by_count(
            unchanged,
            "1 file is still Omarchy's default, so this computer's newer one stays",
            "{n} files are still Omarchy's defaults, so this computer's newer ones stay"))
    left = counts.get("skip", 0) + len(plan.skipped)
    if left:
        lines.append(by_count(left, "1 Try-only, cache or unsafe item stays behind",
                              "{n} Try-only, cache or unsafe items stay behind"))
    if not (copies or replaced or merged or conflicts or counts.get("mkdir")
            or counts.get("replace-profile")):
        lines.append("Nothing new to bring over")
    return lines


def plan_details(plan):
    lines = []
    order = ("replace-profile", "create", "replace", "merge", "conflict", "mkdir", "skip")
    for kind in order:
        for action in plan.actions:
            if action.action != kind:
                continue
            reason = f" ({action.reason})" if action.reason and kind != "mkdir" else ""
            target = f" -> {action.sibling}" if action.sibling else ""
            lines.append(f"{DESCRIPTIONS.get(kind, kind):>16}  {action.relative}{target}{reason}")
    for relative, reason in plan.skipped:
        lines.append(f"{'left behind':>16}  {relative} ({reason})")
    return "\n".join(lines) + "\n"


def plan_json(plan, inventory=None, extra=None):
    document = {
        "resolution": plan.resolution,
        "groups": plan.groups,
        "counts": plan.counts(),
        "bytesNeeded": plan.bytes_needed(),
        "actions": [
            {"path": action.relative, "action": action.action, "group": action.group,
             "kind": action.kind, "reason": action.reason, "sibling": action.sibling}
            for action in plan.actions
        ],
        "skipped": [{"path": relative, "reason": reason} for relative, reason in plan.skipped],
    }
    if inventory is not None:
        document["available"] = [
            {"id": group.id, "kind": group.kind, "label": group.label, "files": group.files,
             "changed": group.changed, "bytes": group.bytes}
            for group in inventory.ordered()
        ]
    if extra:
        document.update(extra)
    return json.dumps(document, indent=2, sort_keys=True)


def result_summary(report, steps):
    lines = []
    done = [result for result in report.results if result.status == "done"]
    skipped = [result for result in report.results if result.status == "skipped"]
    failed = report.failures()
    files = sum(1 for result in done if result.action not in ("mkdir", "replace-profile"))
    lines.append(by_count(files, "Imported 1 file", "Imported {n} files")
                 + f" ({human_size(report.written_bytes)}).")
    if any(result.detail.startswith("backup in") or result.action == "replace-profile"
           for result in done):
        lines.append(f"Anything replaced was backed up to {report.backup_directory}")
    siblings = [result for result in done if result.detail.startswith("kept this computer's")]
    if siblings:
        lines.append(by_count(len(siblings), "1 file from the trial was saved next to your newer "
                              "version", "{n} files from the trial were saved next to your newer "
                              "versions") + " (look for 'from-try-omarchy' or '(from Try Omarchy)').")
    for result in skipped[:20]:
        lines.append(f"Skipped {result.relative}: {result.detail}")
    if len(skipped) > 20:
        lines.append(f"Skipped {len(skipped) - 20} more; run again with --dry-run to see them.")
    for result in failed:
        lines.append(f"Could not import {result.relative}: {result.detail}")
    for step in steps:
        if step is None:
            continue
        if step.ok and step.detail:
            lines.append(step.detail[0].upper() + step.detail[1:] + ".")
        elif not step.ok:
            lines.append(f"Not finished: {step.detail}")
    return lines
