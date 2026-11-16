#!/usr/bin/env python3
"""Targeted functional cases: prefix forks, thresholds, cost guard, identity.

Each command drives a specific scenario and prints what the cache actually did, using
the server's per-request cache_stats line rather than inferring from response timing.
"""

import hashlib
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cache_functional_matrix as M


def show(label, entry, stats):
    print(f'  {label:<14} prompt_n={entry["prompt_n"]:<6} cache_n={entry["cache_n"]:<6} '
          f'source={stats.get("source","?"):<6} disk={stats.get("reused_disk","?"):<6} '
          f'restored={stats.get("restored","?"):<6} sha={entry["sha"]}')


def t05():
    """Two requests share a prefix and diverge. The shared part should be reusable and
    the divergent tail must not be."""
    docs = M.usable(M.discover())
    a = docs[0]

    shared = a['text'][:3000]
    sx = shared + a['text'][20000:21000]
    sy = shared + a['text'][30000:31000]

    ts, tx, ty = M.tokenize(shared), M.tokenize(sx), M.tokenize(sy)

    print(f'shared={len(ts)} tok  X={len(tx)}  Y={len(ty)}')
    print(f'LCP(shared,X)={M.lcp(ts, tx)}  LCP(shared,Y)={M.lcp(ts, ty)}  LCP(X,Y)={M.lcp(tx, ty)}')
    print('note: LCP(shared,X) < len(shared) is the BPE boundary effect, not an error\n')

    seq = [('X', sx), ('Y', sy), ('X-again', sx)]

    for label, prompt in seq:
        e = M.complete(prompt, 4)
        stats = M.parse_stats(M.log_slice())
        show(label, e, stats)
        time.sleep(3)

    # X and its repeat must be identical; Y must differ
    print('\nX and X-again must match: compare shas above')


def t07():
    """Below the disk floor vs above it. A refusal below the floor is correct
    behaviour, not a cache defect."""
    docs = M.usable(M.discover())
    a = docs[0]

    # a short prompt well under --cache-disk-min-tokens 2048
    short = a['text'][:600]
    long_ = a['text'][:M.MEDIUM_CHARS]

    ts, tl = M.tokenize(short), M.tokenize(long_)
    print(f'short={len(ts)} tok (floor is 2048)  long={len(tl)} tok\n')

    for label, prompt, toks in (('short', short, ts), ('long', long_, tl),
                                ('short-again', short, ts)):
        e = M.complete(prompt, 2)
        stats = M.parse_stats(M.log_slice())
        show(label, e, stats)
        time.sleep(3)

    print(f'\nbelow-floor request had {len(ts)} tokens; it must not have produced a disk entry')


def t09():
    """A single changed character in the middle. The identity must change, and the
    state after the change must not be treated as the same prompt."""
    docs = M.usable(M.discover())
    a = docs[0]

    text = a['text'][:M.MEDIUM_CHARS]
    mid = len(text) // 2

    # change one character in the middle, then change it back
    mutated = text[:mid] + ('X' if text[mid] != 'X' else 'Y') + text[mid + 1:]

    t_orig, t_mut = M.tokenize(text), M.tokenize(mutated)
    print(f'original={len(t_orig)} tok  mutated={len(t_mut)} tok  '
          f'LCP={M.lcp(t_orig, t_mut)} (change index ~{mid} chars)\n')

    for label, prompt in (('orig', text), ('mutated', mutated), ('orig-again', text)):
        e = M.complete(prompt, 2)
        stats = M.parse_stats(M.log_slice())
        show(label, e, stats)
        time.sleep(3)


def t04():
    """A short prefix, then a longer one, then the short one again."""
    docs = M.usable(M.discover())
    a = docs[0]

    short = a['text'][:1800]
    long_ = a['text'][:M.MEDIUM_CHARS]

    ts, tl = M.tokenize(short), M.tokenize(long_)
    print(f'short={len(ts)} tok  long={len(tl)} tok  LCP={M.lcp(ts, tl)}\n')

    for label, prompt in (('short', short), ('long', long_), ('short-again', short)):
        e = M.complete(prompt, 2)
        stats = M.parse_stats(M.log_slice())
        show(label, e, stats)
        time.sleep(3)


COMMANDS = {'t04': t04, 't05': t05, 't07': t07, 't09': t09}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print('commands: ' + ', '.join(COMMANDS))
        sys.exit(1)
    COMMANDS[sys.argv[1]]()


if __name__ == '__main__':
    main()
