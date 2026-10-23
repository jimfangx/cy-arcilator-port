#!/usr/bin/env python3
"""Check optional packing codegen in an isolated MMU/resource-limit worker."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

sim = Path(__file__).resolve().parents[1]


def check(circt, cxx, build):
    build.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix='run-', dir=build))
    command = [
        sys.executable,
        str(Path(__file__).resolve()), '--worker',
        str(circt), cxx,
        str(run)
    ]
    child = subprocess.Popen(command, start_new_session=True)
    try:
        code = child.wait(timeout=180)
    except BaseException:
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
        raise
    if code:
        raise subprocess.CalledProcessError(code, command)
    result = json.loads((run / 'report.json').read_text())
    assert result['status'] == 'passed'
    return result


if __name__ == '__main__':
    if len(sys.argv) == 5 and sys.argv[1] == '--worker':
        from vector_packing_bitwise import check as bitwise
        from vector_packing_cases import check as packing
        root = Path(sys.argv[4])
        a = bitwise(Path(sys.argv[2]).resolve(), sys.argv[3], root)
        b = packing(Path(sys.argv[2]).resolve(), sys.argv[3], root)
        result = dict(
            status='passed',
            bitwise_checks=a['checks'],
            packing_checks=b['positive_byte_alias_callback_checks'],
            boundary_checks=a['guarded_page_checks'] +
            b['positive_guarded_page_checks'],
            targeted_definedness_checks=a['definedness_native_checks'] +
            b['targeted_definedness_checks'],
            masked_shift_fault_detected=b['masked_shift_fault_detected'],
            api_checks=sum(row['checks'] for row in b['api']),
            failed_build_preserves_outputs=b[
                'failed_vector_build_preserves_outputs'])
        (root / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    else:
        print(
            json.dumps(
                check(
                    Path(os.environ['CIRCT_BIN']).resolve(),
                    os.environ.get('CXX', 'c++'),
                    sim / 'generated-src/vector-packing-test')))
