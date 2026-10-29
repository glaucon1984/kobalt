"""Register Kobalt on the PocketBook home screen and keep the library clean. Idempotent.

  python pocketbook-desktop.py <mount, e.g. L:> [--reset-handlers]

- copies the icons from platforms/pocketbook/device/ to applications/icons/
- adds "@kobalt=Kobalt" to system/language/en.txt (display name of applications/kobalt.app)
- adds a "U_kobalt" block inside the "applications" object of system/config/desktop/view.json and
  "U_kobalt" to the @General group (backup saved next to it), as done for KOReader
- removes files the library would index as books from Kobalt's folders (*.txt logs, the
  dictionaries' README.txt)
- with --reset-handlers: entries in system/config/handlers.cfg that point at kobalt.app go back to
  koreader.app (the default handler stays the first app of each extensions.cfg line)
"""
import json
import os
import shutil
import sys

mount = sys.argv[1] if len(sys.argv) > 1 else "L:"
if mount.endswith(":"):
    mount += "\\"  # "L:" alone would be drive-relative on Windows
reset_handlers = "--reset-handlers" in sys.argv
here = os.path.dirname(os.path.abspath(__file__))
icons_src = os.path.join(here, "..", "device")

# 1. icons
icons_dst = os.path.join(mount, "applications", "icons")
os.makedirs(icons_dst, exist_ok=True)
for name in ("desktop_app_kobalt.bmp", "desktop_app_kobalt_f.bmp"):
    shutil.copyfile(os.path.join(icons_src, name), os.path.join(icons_dst, name))
print("icons:", icons_dst)

# 2. display name
lang = os.path.join(mount, "system", "language", "en.txt")
lines = open(lang, encoding="utf-8").read().splitlines() if os.path.exists(lang) else []
if not any(l.startswith("@kobalt=") for l in lines):
    lines.append("@kobalt=Kobalt")
    open(lang, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("name: added @kobalt=Kobalt to", lang)
else:
    print("name: already in", lang)

# 3. view.json
view_path = os.path.join(mount, "system", "config", "desktop", "view.json")
view = json.load(open(view_path, encoding="utf-8"))
apps = view.setdefault("applications", {})
block = {
    "path": "/mnt/ext1/applications/kobalt.app",
    "title": "Kobalt",
    "icon": "/mnt/ext1/applications/icons/desktop_app_kobalt.bmp",
    "focused_icon": "/mnt/ext1/applications/icons/desktop_app_kobalt_f.bmp",
}
changed = False
if apps.get("U_kobalt") != block:
    # keep the firmware's _comment last, as in the KOReader setup
    comment = apps.pop("_comment", None)
    apps["U_kobalt"] = block
    if comment is not None:
        apps["_comment"] = comment
    changed = True
def general_groups(node):
    # the @General group may sit at any depth of the "view" tree
    if isinstance(node, dict):
        if node.get("title") == "@General" and isinstance(node.get("apps"), list):
            yield node
        for value in node.values():
            yield from general_groups(value)
    elif isinstance(node, list):
        for item in node:
            yield from general_groups(item)
for group in general_groups(view.get("view")):
    if "U_kobalt" not in group["apps"]:
        group["apps"].append("U_kobalt")
        changed = True
if changed:
    shutil.copyfile(view_path, view_path + ".before-kobalt")
    json.dump(view, open(view_path, "w", encoding="utf-8", newline="\n"), indent=2, ensure_ascii=False)
    print("view.json: U_kobalt added (backup view.json.before-kobalt)")
else:
    print("view.json: already registered")

# 4. files the library would show as books
removed = []
kobalt = os.path.join(mount, "applications", "kobalt")
for rel in ("log.txt", "open.txt"):
    p = os.path.join(kobalt, rel)
    if os.path.exists(p):
        os.remove(p)
        removed.append(p)
pbqt_log = os.path.join(mount, "applications", "pbqt", "log.txt")
if os.path.exists(pbqt_log):
    os.remove(pbqt_log)
    removed.append(pbqt_log)
dicts = os.path.join(kobalt, "profile", "config", "dictionaries")
if os.path.isdir(dicts):
    for entry in os.listdir(dicts):
        p = os.path.join(dicts, entry, "README.txt")
        if os.path.exists(p):
            os.remove(p)
            removed.append(p)
print("removed:", removed or "nothing")

# 5. handlers
if reset_handlers:
    handlers = os.path.join(mount, "system", "config", "handlers.cfg")
    if os.path.exists(handlers):
        text = open(handlers, encoding="utf-8").read()
        new = text.replace(":kobalt.app", ":koreader.app")
        if new != text:
            open(handlers, "w", encoding="utf-8", newline="\n").write(new)
            print("handlers.cfg: kobalt.app entries reset to koreader.app")
        else:
            print("handlers.cfg: nothing to reset")
