"""System function names by NID, for the functions this game imports (import_names.txt)."""
from pathlib import Path


def load():
    """Return {(module, nid): name}."""
    names = {}
    for line in (Path(__file__).parent / "import_names.txt").read_text().splitlines():
        if line and not line.startswith("#"):
            mod, n, name = line.split()
            names[(mod, int(n, 16))] = name
    return names
