#!/usr/bin/env python3
# compare_o2r.py <console.o2r> <pc.o2r>: xsdk milestone 64's check (xsdk:docs/scope/64-starship-rom-extraction.md).
# Compares the sf64.o2r Torch made on the console with the PC's from the same ROM, entry by entry: the entries only one
# has, and those whose bytes differ, by folder and by the resource type in their header, with the first few named.
# Exits 0 only when every entry matches.
import collections
import struct
import sys
import zipfile

console, pc = (zipfile.ZipFile(path) for path in sys.argv[1:3])
console_names, pc_names = set(console.namelist()), set(pc.namelist())
only_console, only_pc = sorted(console_names - pc_names), sorted(pc_names - console_names)
differing = sorted(name for name in console_names & pc_names if console.read(name) != pc.read(name))


def kind(name):
    """The entry's folder and its resource type, from bytes 4 to 8 of a libultraship resource's header."""
    data = pc.read(name)
    resource_type = "%08X" % struct.unpack("<I", data[4:8])[0] if len(data) >= 8 else "short"
    return "%s %s" % (name.split("/", 1)[0], resource_type)


print("compare_o2r: %d entries on the console, %d on the PC; %d match" %
      (len(console_names), len(pc_names), len(console_names & pc_names) - len(differing)))
for label, names in (("only on the console", only_console), ("only on the PC", only_pc)):
    if names:
        print("compare_o2r: %d %s, such as %s" % (len(names), label, ", ".join(names[:5])))
if differing:
    print("compare_o2r: %d differ:" % len(differing))
    groups = collections.defaultdict(list)
    for name in differing:
        groups[kind(name)].append(name)
    for group, names in sorted(groups.items(), key=lambda item: -len(item[1])):
        print("  %5d  %s, such as %s" % (len(names), group, ", ".join(names[:3])))
sys.exit(0 if not (only_console or only_pc or differing) else 1)
