"""Place HTIF mailboxes in CVA6Tile's uncached 0x80001000..0x80001080 window."""
from pathlib import Path
import subprocess


def linker_script(riscv, directory):
    compiler = Path(riscv) / 'bin/riscv64-unknown-elf-gcc'
    installed = Path(
        subprocess.check_output([compiler, '-print-file-name=htif.ld'],
                                text=True).strip())
    original = installed.read_text()
    text_header = '    .text : {\n        *(.text.init)'
    htif = '    .htif ALIGN(0x40) : {\n        *(.htif)\n    }'
    if original.count(text_header) != 1 or original.count(htif) != 1:
        raise ValueError('unrecognized installed HTIF linker layout')
    adjusted = original.replace(htif, '').replace(
        text_header, '''    .text.init : {
        *(.text.init)
    }
    ASSERT(. <= 0x80001000, "CVA6 startup overlaps uncached HTIF window")
    .htif 0x80001000 : {
        *(.htif)
    }
    ASSERT(SIZEOF(.htif) <= 0x80, "CVA6 HTIF exceeds uncached window")
    . = 0x80001080;
    .text : {''')
    output = Path(directory) / 'cva6-htif.ld'
    output.write_text(adjusted)
    return output


def check_mailboxes(riscv, binary):
    symbols = subprocess.check_output(
        [Path(riscv) / 'bin/riscv64-unknown-elf-nm', binary], text=True)
    addresses = {
        fields[2]: int(fields[0], 16)
        for line in symbols.splitlines()
        if len(fields := line.split()) == 3 and fields[2] in ('tohost',
                                                              'fromhost')
    }
    if set(addresses) != {'tohost', 'fromhost'} or any(
            not 0x80001000 <= address <= 0x80001078
            for address in addresses.values()):
        raise ValueError('CVA6 mailboxes are outside its uncached HTIF window')
    return addresses
