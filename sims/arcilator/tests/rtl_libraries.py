#!/usr/bin/env python3
"""Check library dependencies across edits, selection changes, and no-op builds."""
import importlib.util
from pathlib import Path
import tempfile
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
spec = importlib.util.spec_from_file_location(
    'chipyard_rtl_libraries', Path(__file__).resolve().parents[1] / 'rtl_libraries.py')
libraries = importlib.util.module_from_spec(spec)
spec.loader.exec_module(libraries)


def main():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        listing, output = root / 'blackboxes.f', root / 'libraries.json'
        listing.write_text('# ordinary selected design\nordinary.sv\n')
        empty = libraries.discover(listing, root)
        assert not empty['flags'] and not empty['source_sha256']
        assert libraries.update(output, empty)
        first_stat = output.stat()
        assert not libraries.update(output, libraries.discover(listing, root))
        assert output.stat().st_mtime_ns == first_stat.st_mtime_ns

        for variant in ('large', 'small'):
            source = root / f'nvdla_{variant}.preprocessed.v'
            source.write_text(f'module {variant}; endmodule\n')
            directory = root / f'generators/nvdla/src/main/resources/vsrc/{variant}/vmod'
            for part in ('rams/synth', 'rams/model', 'vlibs', 'include'):
                (directory / part).mkdir(parents=True)
            header = directory / 'include/geometry.vh'
            header.write_text('`define WIDTH 32\n')
            model = directory / 'rams/model/model.v'
            model.write_text('module model; endmodule\n')
            cell = directory / 'vlibs/cell.vlib'
            cell.write_text('module cell; endmodule\n')
            # The list may also contain native C++ support and comments.
            listing.write_text(f'"{source}"\nformat_strings.cpp # generated support\n')
            manifest = libraries.discover(listing, root)
            assert manifest['packages'][0]['variant'] == variant
            assert str(cell) in manifest['source_sha256']
            assert libraries.update(output, manifest)
            unchanged_time = output.stat().st_mtime_ns
            assert not libraries.update(output, libraries.discover(listing, root))
            assert output.stat().st_mtime_ns == unchanged_time
            # A header edit must invalidate import even when its mtime/size
            # are restored by a source checkout or cache materialization.
            import os
            stat = header.stat()
            header.write_text('`define WIDTH 64\n')
            os.utime(header, ns=(stat.st_atime_ns, stat.st_mtime_ns))
            assert libraries.update(output, libraries.discover(listing, root))
            added = directory / 'vlibs/new_cell.v'
            added.write_text('module new_cell; endmodule\n')
            assert libraries.update(output, libraries.discover(listing, root))
            added.unlink()
            assert libraries.update(output, libraries.discover(listing, root))
            source.write_text(f'module {variant}; wire changed; endmodule\n')
            assert libraries.update(output, libraries.discover(listing, root))
            model.unlink()
            assert libraries.update(output, libraries.discover(listing, root))
            (directory / 'rams/model').rmdir()
            try:
                libraries.discover(listing, root)
            except ValueError as error:
                assert 'requires original library directory' in str(error)
            else:
                raise AssertionError('missing selected library accepted')
        listing.write_text('ordinary.sv\n')
        assert libraries.update(output, libraries.discover(listing, root))
        assert not libraries.discover(listing, root)['flags']
    print('PASS native library dependency controls')


if __name__ == '__main__':
    main()
