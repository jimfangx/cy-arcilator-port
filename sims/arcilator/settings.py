#!/usr/bin/env python3
"""Update a Make settings dependency only when its effective values change."""
import argparse
import json
import os
from pathlib import Path
import tempfile


def update(path, entries):
    settings = {}
    for entry in entries:
        name, separator, value = entry.partition('=')
        if not separator or not name or name in settings:
            raise ValueError('expected distinct NAME=VALUE settings: ' + entry)
        settings[name] = value
    content = (json.dumps(settings, indent=2, sort_keys=True) + '\n').encode()
    try:
        if path.read_bytes() == content:
            return False
    except FileNotFoundError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent,
                                         delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('settings', nargs='+', metavar='NAME=VALUE')
    args = parser.parse_args()
    try:
        changed = update(args.output, args.settings)
    except ValueError as error:
        parser.error(str(error))
    if changed:
        print('Updated Arcilator settings: ' + str(args.output))


if __name__ == '__main__':
    main()
