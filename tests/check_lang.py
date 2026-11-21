"""Check the language files against the example YAML.

ESPHome rejects a YAML file with a duplicate key, while most YAML readers silently
keep the last one - so check explicitly. Run: python3 tests/check_lang.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
used = set(re.findall(r"\$\{(\w+)\}", (ROOT / "truma-inetx.yaml").read_text(encoding="utf-8")))
failed = False
keysets = {}
for lang in sorted((ROOT / "lang").glob("*.yaml")):
    keys = re.findall(r"^  (\w+):", lang.read_text(encoding="utf-8"), re.M)
    dupes = sorted({k for k in keys if keys.count(k) > 1})
    missing = sorted(used - set(keys))
    keysets[lang.name] = set(keys)
    for what, items in (("duplicate keys", dupes), ("missing keys", missing)):
        if items:
            failed = True
            print(f"{lang.name}: {what}: {', '.join(items)}")
names = list(keysets)
for other in names[1:]:
    diff = keysets[names[0]] ^ keysets[other]
    if diff:
        failed = True
        print(f"{names[0]} and {other} differ: {', '.join(sorted(diff))}")
print("language files FAILED" if failed else f"language files OK ({', '.join(names)})")
sys.exit(1 if failed else 0)
