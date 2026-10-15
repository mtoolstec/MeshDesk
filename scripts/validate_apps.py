#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[1]
for manifest in sorted((root / "apps").glob("*/app.json")):
    data = json.loads(manifest.read_text())
    assert 0 < manifest.stat().st_size <= 4096, manifest
    assert data.get("name"), manifest
    if data.get("type") == "pocketjs":
        entry = manifest.parent / data.get("entry", "app.js")
        assert entry.is_file(), entry
        subprocess.run(["node", "--check", str(entry)], check=True)
print("App manifests and scripts validated")
