"""Independent complete waveform oracle for the native run-loop fixture."""
from pathlib import Path


def expected_waveform(mode):
    scale = 1000 if mode & 4 else 1
    lines = [f'$timescale 1{"fs" if scale == 1000 else "ps"} $end',
             '$scope module ArcModel $end']
    names = ('clock', 'reset', 'print', 'stop', 'success')
    lines += [f'$var wire 1 s{i} \\{name} $end' for i, name in enumerate(names)]
    lines += ['$upscope $end', '$enddefinitions $end']
    # Root half-period is 500ps, reset falls between two root edges, success
    # arrives at 900500ps and is observed at the following rising root edge.
    # Mode16 lacks io_success at its first read; mode17 never asserts success.
    end = 778000 if mode == 16 else 1000500 if mode == 17 else 901500
    times = sorted({*range(0, end + 1, 500), 777700})
    previous = None
    for time in times:
        values = (time // 500 % 2, int(time < 777700),
                  int(bool(mode & 1) and time >= 777700), int(time >= 777700),
                  int(mode < 16 and time >= 900500))
        lines.append(f'#{time * scale}')
        lines += [f'b{value} s{i}' for i, value in enumerate(values)
                  if previous is None or value != previous[i]]
        previous = values
    return ('\n'.join(lines) + '\n').encode()


def check(directory):
    directory = Path(directory)
    for mode in range(18):
        actual = (directory / f'mode{mode}.vcd').read_bytes()
        expected = expected_waveform(mode)
        if actual != expected:
            raise RuntimeError(f'run-loop mode{mode} differs from the independent waveform oracle')
    print('PASS: all18 complete run-loop waveforms match independent clock/reset/control expectations', flush=True)


if __name__ == '__main__':
    import sys
    check(sys.argv[1])
