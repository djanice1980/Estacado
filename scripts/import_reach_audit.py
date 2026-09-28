"""Static reach audit of the imports the runtime leaves as traps.

usage: import_reach_audit.py [--depth N] [--coverage FILE]

For every import trapped by runtime/generate_import_traps.ps1 (the game stops
with a report if it is called), walks the recompiled code's direct calls
backwards (generated/ppc) and prints the chains that lead into it, up to the
first callers outside the Xbox system libraries linked into the executable,
with how many functions call each step. Indirect calls (virtual functions,
callbacks) are not visible to this walk; --coverage (a list of guest function
addresses entered during a run, one hex address per line) marks the chain
steps that ran.
"""
import argparse
import collections
import glob
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRAPS = ROOT / "build" / "runtime" / "generated_import_traps.cpp"
# The system libraries (XNet, XAM/XMsg wrappers, content cache, CRT) are
# linked at the end of the title's code.
LIBRARY_START = 0x82870000


def load_graph():
    func_re = re.compile(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)")
    call_re = re.compile(r"\b(?:__imp__)?sub_([0-9A-F]{8})\(ctx, base\)")
    import_re = re.compile(r"\b__imp__([A-Za-z_][A-Za-z0-9_]*)\(ctx, base\)")
    callers = collections.defaultdict(set)
    import_callers = collections.defaultdict(set)
    for path in sorted(glob.glob(str(ROOT / "generated" / "ppc" / "ppc_recomp.*.cpp"))):
        current = None
        for line in open(path, encoding="utf-8", errors="replace"):
            m = func_re.search(line)
            if m:
                current = int(m.group(1), 16)
                continue
            if current is None:
                continue
            for callee in call_re.findall(line):
                target = int(callee, 16)
                if target != current:
                    callers[target].add(current)
            for name in import_re.findall(line):
                if not name.startswith("sub_"):
                    import_callers[name].add(current)
    return callers, import_callers


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--depth", type=int, default=8)
    ap.add_argument("--coverage", type=Path)
    ap.add_argument("--also", nargs="*", default=[],
                    help="implemented imports that stop the game on purpose")
    args = ap.parse_args()
    traps = re.findall(r"PPC_FUNC\(__imp__([_A-Za-z0-9]+)\)", TRAPS.read_text(encoding="utf-8"))
    traps += args.also
    covered = set()
    if args.coverage:
        for line in args.coverage.read_text().split():
            covered.add(int(line, 16))
    callers, import_callers = load_graph()
    for name in traps:
        first = import_callers.get(name, set())
        # Breadth-first up the callers until game code (below the libraries).
        game, seen, frontier, depth = set(), set(first), set(first), 0
        while frontier and depth < args.depth:
            nxt = set()
            for f in frontier:
                if f < LIBRARY_START:
                    game.add(f)
                    continue
                for c in callers.get(f, ()):
                    if c not in seen:
                        seen.add(c)
                        nxt.add(c)
            frontier, depth = nxt, depth + 1
        game |= {f for f in frontier if f < LIBRARY_START}
        ran = sorted(f for f in seen if f in covered)
        mark = f" ran:{len(ran)}" if args.coverage else ""
        print(f"{name}: wrappers {len(first)}, library functions {len(seen - game)}, "
              f"game callers {len(game)}{mark}"
              + (": " + " ".join(f"{g:08X}" for g in sorted(game)[:12]) if game else ""))


if __name__ == "__main__":
    main()
