#!/usr/bin/env python3
"""Read native source filelists without invoking an HDL simulator."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import tempfile


def _tokens(path):
    # Strip comments outside shell-style quoted/escaped paths. Retain line
    # breaks so malformed quote diagnostics retain the original structure.
    text = path.read_text()
    clean, quote, index = [], None, 0
    while index < len(text):
        char = text[index]
        if char == "\\" and quote != "'" and index + 1 < len(text):
            clean.append(text[index:index + 2])
            index += 2
            continue
        if quote:
            clean.append(char)
            if char == quote:
                quote = None
            index += 1
            continue
        if char in ("'", '"'):
            quote = char
            clean.append(char)
            index += 1
            continue
        if char == '#' or text.startswith('//', index):
            end = text.find('\n', index)
            index = len(text) if end < 0 else end
            clean.append(' ')
            continue
        if text.startswith('/*', index):
            end = text.find('*/', index + 2)
            if end < 0:
                raise ValueError(f'{path}: unterminated filelist comment')
            clean.append(' ' + '\n' * text[index:end + 2].count('\n'))
            index = end + 2
            continue
        clean.append(char)
        index += 1
    try:
        return iter(shlex.split(''.join(clean), comments=False))
    except ValueError as error:
        raise ValueError(f'{path}: {error}') from error


def source_paths(filelist, cwd=None):
    """Yield source paths in filelist order, expanding explicit -f/-F lists.

    The top-level list and -f contents use the invocation directory. -F
    contents use the referenced list's directory. A nested list's filename
    is resolved using its containing list's current path base. Options stay
    outside this source-only interface; compiler flags use their existing
    native frontend/build arguments. Repeated nonrecursive lists are valid.
    """
    working = Path.cwd() if cwd is None else Path(cwd).absolute()
    stack, active = [], set()

    def push(path, base):
        identity = path.resolve()
        if identity in active:
            chain = ' -> '.join(str(item[1]) for item in stack)
            raise ValueError(f'recursive native filelist: {chain} -> {path}')
        tokens = _tokens(path)
        active.add(identity)
        stack.append((tokens, path, base, identity))

    root = Path(filelist)
    if not root.is_absolute():
        root = working / root
    push(root, working)
    while stack:
        tokens, path, base, identity = stack[-1]
        try:
            token = next(tokens)
        except StopIteration:
            active.remove(identity)
            stack.pop()
            continue
        if token in ('-f', '-F'):
            try:
                name = next(tokens)
            except StopIteration as error:
                raise ValueError(f'{path}: {token} requires a filelist') from error
            child = Path(name)
            if not child.is_absolute():
                child = base / child
            push(child, child.parent if token == '-F' else working)
        elif token and not token.startswith(('-', '+')):
            entry = Path(token)
            yield entry if entry.is_absolute() else base / entry


def input_manifests(filelist, cwd=None):
    """Fingerprint selected inputs so nested edits invalidate native builds."""
    hdl, drivers = {}, {}
    for path in source_paths(filelist, cwd):
        suffix = path.suffix.lower()
        selected = (hdl if suffix in ('.v', '.sv', '.vh', '.svh') else
                    drivers if suffix in ('.c', '.cc', '.cpp', '.cxx',
                                          '.h', '.hh', '.hpp') else None)
        if selected is not None:
            selected[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    return hdl, drivers


def _update(path, values):
    content = (json.dumps(values, indent=2, sort_keys=True) + '\n').encode()
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--filelist', type=Path, required=True)
    parser.add_argument('--hdl-output', type=Path, required=True)
    parser.add_argument('--driver-output', type=Path, required=True)
    args = parser.parse_args()
    try:
        hdl, drivers = input_manifests(args.filelist)
        _update(args.hdl_output, hdl)
        _update(args.driver_output, drivers)
    except (OSError, ValueError) as error:
        parser.exit(1, f'native filelist: {error}\n')


if __name__ == '__main__':
    main()
