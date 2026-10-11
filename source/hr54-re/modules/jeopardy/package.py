"""Stage and package Jeopardy with ReBox's deterministic .rbox builder."""
from pathlib import Path
import importlib.util
import shutil
import sys
import tempfile

root = Path(__file__).resolve().parent
builder_file = root.parents[3] / "tools/build-module.py"
spec = importlib.util.spec_from_file_location("rebox_module_builder", builder_file)
builder = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
spec.loader.exec_module(builder)

with tempfile.TemporaryDirectory(prefix="rebox-jeopardy-package-") as name:
    stage = Path(name)
    files = {
        "module.json": root / "module.json",
        "icon.png": root / "assets/icon.png",
        "bin/module": root / "build/bin/module",
        "bin/hr54-jeopardy-native": root / "build/bin/hr54-jeopardy-native",
        "default-data/questions.jdb": root / "build/demo.jdb",
        "OFL.txt": root / "assets/OFL.txt",
    }
    for relative, source in files.items():
        dest = stage / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, dest)
        dest.chmod(0o700 if relative.startswith("bin/") else 0o600)
    output = root / "build/jeopardy.rbox"
    print(builder.package(stage, output), output)
