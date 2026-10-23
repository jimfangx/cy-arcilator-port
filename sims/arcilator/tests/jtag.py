"""Exercise the original SimJTAG remote-bitbang driver and native debug TAP."""
import re
import socket
import time


def exercise(proc, log, timeout):
    deadline = None if timeout is None else time.monotonic() + timeout
    while True:
        match = re.search(r'Listening on port (\d+)', log.read_text(errors='replace'))
        if match:
            break
        if proc.poll() is not None or (deadline is not None and time.monotonic() >= deadline):
            raise RuntimeError('SimJTAG did not open its remote-bitbang socket')
        time.sleep(0.01)
    with socket.create_connection(('127.0.0.1', int(match[1])), timeout=5) as client:
        client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        client.settimeout(timeout)

        def cycle(tms, tdi=0, read=False):
            remaining = None if deadline is None else deadline - time.monotonic()
            if remaining is not None and remaining <= 0:
                raise TimeoutError('JTAG transactions exceeded their time limit')
            client.settimeout(remaining)
            pins = (tms << 1) | tdi
            client.sendall(bytes([ord('0') + pins]))
            value = 0
            if read:
                client.sendall(b'R')
                response = client.recv(1)
                if response not in (b'0', b'1'):
                    raise RuntimeError(f'invalid remote-bitbang reply: {response!r}')
                value = int(response)
            client.sendall(bytes([ord('4') + pins]))
            return value

        def scan(bits, value=0, instruction=False):
            cycle(1)  # Select-DR
            if instruction:
                cycle(1)  # Select-IR
            cycle(0)  # Capture
            cycle(0)  # Shift
            result = 0
            for i in range(bits):
                result |= cycle(int(i == bits - 1), (value >> i) & 1, read=True) << i
            cycle(1)  # Update
            cycle(0)  # Run-Test/Idle
            return result

        for _ in range(6):
            cycle(1)  # Test-Logic-Reset
        cycle(0)
        idcode = scan(32)
        # RocketConfig uses JtagDTMKeyDefault (all ID fields zero).
        if idcode != 1:
            raise RuntimeError(f'JTAG IDCODE mismatch: {idcode:#010x}, expected 0x00000001')
        scan(5, 0x10, instruction=True)
        dtmcs = scan(32)
        abits = (dtmcs >> 4) & 0x3f
        if dtmcs & 0xf != 1 or abits < 7 or (dtmcs >> 10) & 3:
            raise RuntimeError(f'invalid JTAG DTMCS: {dtmcs:#010x}')
        scan(5, 0x11, instruction=True)

        def idle():
            for _ in range(16):
                cycle(0)

        # Activate the debug module, then read DMSTATUS over DMI. This crosses
        # from the JTAG clock into the native debug-module clock domain.
        scan(34 + abits, (0x10 << 34) | (1 << 2) | 2)
        idle()
        acknowledgement = scan(34 + abits)
        if acknowledgement & 3:
            raise RuntimeError(f'JTAG DMI write failed: {acknowledgement:#x}')
        idle()
        scan(34 + abits, (0x11 << 34) | 1)
        idle()
        response = scan(34 + abits)
        dmstatus = (response >> 2) & 0xffffffff
        if response & 3 or dmstatus & 0xf != 2 or not dmstatus & (1 << 7):
            raise RuntimeError(f'JTAG DMSTATUS read failed: response={response:#x}, data={dmstatus:#010x}')
        client.sendall(b'Q')
    return dict(idcode=idcode, dtmcs=dtmcs, dmstatus=dmstatus)
