"""Sparse preparation of canonical text using native byte scans.

This is an input reader, not an IR parser: matching lines still pass through
the existing semantic rewriters. Whole untouched line ranges retain their
original text and append count. Inputs with CRs or non-line-aligned bounds
fall back to ordinary line iteration.
"""
import codecs
from contextlib import contextmanager
import mmap


class UnchangedLines(str):
    """An immediate-use view of unchanged complete lines in a live mapping."""

    def __new__(cls, mapping, start, end):
        value = super().__new__(cls, '')
        value.mapping, value.start, value.end = mapping, start, end
        return value

    def append_to(self, sink):
        decoder = codecs.getincrementaldecoder('utf-8')()
        count = 0
        for start in range(self.start, self.end, 1 << 20):
            chunk = self.mapping[start:min(start + (1 << 20), self.end)]
            count += chunk.count(b'\n')
            sink.stream.write(decoder.decode(chunk))
        sink.stream.write(decoder.decode(b'', final=True))
        sink.count += count


class SparseText:
    def __init__(self, mapping, start, end):
        self.mapping, self.start, self.end = mapping, start, end

    def ranges(self, pattern):
        """Yield complete matching lines once each, in original order."""
        after = self.start
        for match in pattern.finditer(self.mapping, self.start, self.end):
            if match.start() < after:
                continue
            start = self.mapping.rfind(b'\n', self.start, match.start()) + 1
            start = max(start, self.start)
            end = self.mapping.find(b'\n', match.end(), self.end) + 1
            # Mapping validation guarantees a newline at the range end.
            assert start >= self.start and end > start
            yield start, end
            after = end

    def candidates(self, pattern):
        for start, end in self.ranges(pattern):
            yield self.mapping[start:end].decode('utf-8').rstrip('\r\n')

    def events(self, pattern):
        after = self.start
        for start, end in self.ranges(pattern):
            if after < start:
                yield UnchangedLines(self.mapping, after, start)
            yield self.mapping[start:end].decode('utf-8').rstrip('\r\n')
            after = end
        if after < self.end:
            yield UnchangedLines(self.mapping, after, self.end)

    def reserved_prefixes(self, prefix):
        # The caller's reserved-name query only asks whether an SSA name
        # starts with this ASCII identifier prefix, adding underscores on a
        # collision. Its literal occurrence is exactly that necessary token
        # prefix, including occurrences inside opaque strings as before.
        reserved = []
        while self.mapping.find(prefix.encode('ascii'), self.start, self.end) >= 0:
            reserved.append(prefix)
            prefix += '_'
        return reserved


@contextmanager
def sparse_text(path, start=0, end=None):
    with path.open('rb') as stream:
        mapping = None
        try:
            try:
                mapping = mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ)
            except (OSError, ValueError):
                yield None
                return
            limit = len(mapping) if end is None else end
            if (start < 0 or limit <= start or limit > len(mapping)
                    or (start and mapping[start - 1:start] != b'\n')
                    or mapping[limit - 1:limit] != b'\n'
                    or mapping.find(b'\r', start, limit) >= 0):
                yield None
                return
            yield SparseText(mapping, start, limit)
        finally:
            if mapping is not None:
                mapping.close()
