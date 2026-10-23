#!/usr/bin/env python3
"""Select C compilation for C sources in a Verilator-generated makefile."""
import argparse
from pathlib import Path
import re


def generate(makefile, output):
    # Use Verilator's resolved object/source mapping, including nested filelists
    # and generated DPI headers. Its default rules use C++ for every source.
    objects = []
    for line in Path(makefile).read_text().splitlines():
        match = re.fullmatch(r'([^\s:]+\.o):\s+(.+\.c)\s*', line)
        if match:
            objects.append(match[1])
    text = '# Generated C language settings for foreign sources.\n'
    if objects:
        text += (' '.join(objects) + ': override CXXFLAGS := '
                 '$(filter-out -std=% -fcoroutines,$(CXXFLAGS)) '
                 '-x c -std=gnu11 $(ARC_CFLAGS)\n')
        text += (' '.join(objects) + ': override CPPFLAGS := '
                 '$(filter-out -std=% -fcoroutines -faligned-new,$(CPPFLAGS))\n')
    Path(output).write_text(text)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('makefile')
    parser.add_argument('output')
    args = parser.parse_args()
    generate(args.makefile, args.output)
