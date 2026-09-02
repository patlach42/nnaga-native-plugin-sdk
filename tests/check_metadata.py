import json
import pathlib
import sys

metadata = json.loads(pathlib.Path(sys.argv[1]).read_text())
expected = {
    "repository_id": "native.nnaga-filter",
    "descriptor_id": "com.vibes.dsp.filter",
    "name": "NNAGA Filter",
    "version": "1.0.1",
    "license": "MIT",
    "library": "libnnaga_plugin_filter.so",
}
for key, value in expected.items():
    assert metadata.get(key) == value, f"{key}: {metadata.get(key)!r} != {value!r}"
assert metadata["source"].startswith("https://")
assert metadata["description"]
assert metadata["tags"] == ["Filter", "Stereo", "Native"]
