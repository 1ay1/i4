#!/usr/bin/env python3
"""select_protocols.py — choose one XML file per distinct interface set.

Takes one or more roots and walks all of them. Two reasons a protocol can
appear twice:

  * upstream wayland-protocols ships some of them as an `unstable/` copy AND
    a `stable/` or `staging/` successor declaring the SAME interface names;
  * a vendored tree (loom's proto/, for the wlroots, KDE and Hyprland
    extensions) may carry a copy of something the system has too.

Generating both would define the same types twice, so the build has to pick.
Ranking is stable > staging > unstable > vendored, then alphabetical for
determinism — so an installed protocol always beats a checked-in copy of
itself, and the vendored tree only ever ADDS what the system lacks.

Note that this de-duplicates by INTERFACE NAME, not by filename. Two
protocols that do a similar job under different names (wlr-data-control and
ext-data-control, say) are different interfaces, a client asks for one of
them by name, and both are generated.

Prints the chosen paths as a CMake list (semicolon separated).
"""
from __future__ import annotations

import glob
import os
import sys
import xml.etree.ElementTree as ET


def rank(path: str) -> int:
    if f"{os.sep}stable{os.sep}" in path:
        return 0
    if f"{os.sep}staging{os.sep}" in path:
        return 1
    if f"{os.sep}unstable{os.sep}" in path:
        return 2
    # A vendored file, or one in a tree with no stability directories.
    # Ranked last so the system's copy wins whenever there is one.
    return 3


def main() -> int:
    roots = sys.argv[1:] or ["/usr/share/wayland-protocols"]

    candidates = []
    for root in roots:
        if not root or not os.path.isdir(root):
            continue
        for path in glob.glob(os.path.join(root, "**", "*.xml"), recursive=True):
            try:
                tree = ET.parse(path)
            except ET.ParseError:
                continue
            ifaces = {i.get("name") for i in tree.getroot().findall("interface")}
            if not ifaces:
                continue
            candidates.append((rank(path), path, ifaces))

    candidates.sort(key=lambda c: (c[0], c[1]))

    chosen: list[str] = []
    claimed: set[str] = set()
    for _, path, ifaces in candidates:
        if ifaces & claimed:
            continue  # a better-ranked file already declares these interfaces
        claimed |= ifaces
        chosen.append(path)

    print(";".join(chosen), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
