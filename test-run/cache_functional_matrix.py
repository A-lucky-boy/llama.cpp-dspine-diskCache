#!/usr/bin/env python3
"""Functional cache matrix: several real documents, alternating requests, restarts.

The point of this script is to exercise the cache the way the plan describes - real
files, real requests, real restarts - and to record *how* each reuse happened, because
the tiers mean different things:

    reused_slot   the server kept the state in the slot from the previous request
    reused_ram    the RAM tier held a snapshot
    reused_disk   a file was read back and its state restored

Only the last one is evidence about the disk tier. A second identical request in the
same process is normally a slot hit, so it is not reported as a disk hit here.

Token prefixes are measured with the server's own tokenizer rather than counted in
characters. Chinese text does not tokenize proportionally to its length and prefix
characters are not prefix token ids, so a character-based estimate would be wrong in
both directions.

Usage:

    python test-run/cache_functional_matrix.py docs     # list discovered documents
    python test-run/cache_functional_matrix.py tokens   # token counts and LCPs
    python test-run/cache_functional_matrix.py t01 ...
"""

import hashlib
import json
import os
import sys
import time
import urllib.request
import urllib.error

PORT = int(os.environ.get('LLAMA_TEST_PORT', '18090'))
# the user's deployment runs with --api-key sk-0; without the header every request is
# rejected with 401 before it reaches the cache at all
API_KEY = os.environ.get('LLAMA_TEST_API_KEY', 'sk-0')
DOC_DIR = os.environ.get('LLAMA_TEST_DOC_DIR', r'E:\小说')
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'logs')

# characters of each document used for the "medium" request: enough to exceed the disk
# floor without filling the context window. Chosen per document, not globally, because
# a character count maps to very different token counts across languages
MEDIUM_CHARS = int(os.environ.get('LLAMA_TEST_MEDIUM_CHARS', '3500'))
LONG_CHARS   = int(os.environ.get('LLAMA_TEST_LONG_CHARS', '11000'))


def api(path, body=None, timeout=3600):
    url = f'http://127.0.0.1:{PORT}{path}'

    headers = {}
    if API_KEY:
        headers['Authorization'] = f'Bearer {API_KEY}'

    if body is None:
        req = urllib.request.Request(url, headers=headers)
    else:
        headers['Content-Type'] = 'application/json'
        req = urllib.request.Request(url, data=json.dumps(body).encode('utf-8'), headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read().decode('utf-8')
    return json.loads(raw) if raw.strip().startswith(('{', '[')) else raw


def tokenize(text):
    """Token ids for `text` from the server's own tokenizer."""
    r = api('/tokenize', {'content': text})
    return r['tokens']


def lcp(a, b):
    n = 0
    for x, y in zip(a, b):
        if x != y:
            break
        n += 1
    return n


# ---------------------------------------------------------------- documents

def discover():
    """Real documents in the directory, with their hashes and encodings.

    The plan asks not to hard-code the names seen in a screenshot, so the list comes
    from the directory itself.
    """
    import pathlib

    root = pathlib.Path(DOC_DIR)
    if not root.is_dir():
        sys.exit(f'document directory not found: {DOC_DIR}')

    found = []

    for p in sorted(root.rglob('*')):
        if not p.is_file():
            continue
        if p.suffix.lower() not in ('.txt', '.md', '.text'):
            continue

        raw = p.read_bytes()
        enc, text = None, None

        # utf-8 first, then the encodings a Chinese corpus realistically uses. the
        # decode has to succeed *strictly*: a lossy decode would change the byte
        # sequence the tokenizer sees and make the token stream untraceable
        for candidate in ('utf-8-sig', 'utf-8', 'utf-16', 'gb18030'):
            try:
                text = raw.decode(candidate)
                enc = candidate
                break
            except (UnicodeDecodeError, UnicodeError):
                continue

        found.append({
            'path': str(p),
            'name': p.name,
            'bytes': len(raw),
            'sha256': hashlib.sha256(raw).hexdigest(),
            'encoding': enc,
            'text': text,
        })

    return found


def usable(docs, min_chars=MEDIUM_CHARS):
    """Documents large enough to be interesting, largest first."""
    ok = [d for d in docs if d['text'] and len(d['text']) >= min_chars]
    ok.sort(key=lambda d: -len(d['text']))
    return ok


# ---------------------------------------------------------------- requests

def complete(prompt, n_predict, cache_prompt=True, seed=None):
    body = {
        'prompt': prompt,
        'n_predict': n_predict,
        'temperature': 0.0,
        'cache_prompt': cache_prompt,
        'stream': False,
    }
    if seed is not None:
        body['seed'] = seed

    t0 = time.time()
    data = api('/completion', body)
    wall = time.time() - t0

    tm = data.get('timings', {})
    content = data.get('content', '')

    return {
        'wall': wall,
        'prompt_n': tm.get('prompt_n'),
        'cache_n': tm.get('cache_n'),
        'prompt_ms': tm.get('prompt_ms'),
        'predicted_n': tm.get('predicted_n'),
        'content': content,
        'sha': hashlib.sha256(content.encode('utf-8')).hexdigest()[:16],
    }


def metrics():
    """Per-request reuse counters, read as deltas by the caller."""
    raw = api('/metrics')
    out = {}

    for line in raw.splitlines():
        if line.startswith('#') or not line.strip():
            continue
        try:
            key, value = line.rsplit(' ', 1)
        except ValueError:
            continue
        key = key.strip()
        if key.startswith('cache_disk') or key.startswith('llamacpp:') or key.startswith('llama_'):
            try:
                out[key] = float(value)
            except ValueError:
                pass

    return out


def log_slice(marker='cache_stats: prompt'):
    """The most recent per-request cache line, if the server logged one."""
    log = os.environ.get('LLAMA_TEST_LOG', '')
    if not log or not os.path.exists(log):
        return None

    last = None
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            if marker in line:
                last = line.strip()

    return last


def record(label, entry, extra=None):
    os.makedirs(OUT_DIR, exist_ok=True)

    row = {'label': label, **{k: v for k, v in entry.items() if k != 'content'}}
    if extra:
        row.update(extra)

    with open(os.path.join(OUT_DIR, 'matrix.jsonl'), 'a', encoding='utf-8') as f:
        f.write(json.dumps(row, ensure_ascii=False) + '\n')

    print(f"[{label}] prompt_n={entry.get('prompt_n')} cache_n={entry.get('cache_n')} "
          f"predicted={entry.get('predicted_n')} sha={entry.get('sha')} "
          f"wall={entry.get('wall', 0):.2f}s", flush=True)

    return row


def parse_stats(line):
    """Pull the structured fields out of a `cache_stats: prompt = ...` log line."""
    if not line:
        return {}

    out = {}
    tail = line.split('cache_stats:', 1)[-1]

    for part in tail.split(','):
        if '=' not in part:
            continue
        k, v = part.split('=', 1)
        out[k.strip()] = v.strip()

    return out


# ---------------------------------------------------------------- commands

def cmd_docs():
    docs = discover()

    print(f'directory: {DOC_DIR}')
    print(f'{"name":<28} {"bytes":>10}  {"encoding":<10} sha256')
    for d in docs:
        print(f'{d["name"]:<28} {d["bytes"]:>10}  {str(d["encoding"]):<10} {d["sha256"][:16]}')

    ok = usable(docs)
    print(f'\n{len(ok)} document(s) large enough to use (>={MEDIUM_CHARS} chars):')
    for d in ok:
        print(f'  {d["name"]}  {len(d["text"])} chars')

    # distinct contents: two paths with the same hash are the same document
    by_hash = {}
    for d in ok:
        by_hash.setdefault(d['sha256'], []).append(d['name'])

    print(f'\n{len(by_hash)} distinct content(s) among them')
    for h, names in by_hash.items():
        if len(names) > 1:
            print(f'  note: {names} share sha256 {h[:16]}')


def cmd_tokens():
    docs = usable(discover())

    if len(docs) < 2:
        sys.exit(f'need at least 2 usable documents, found {len(docs)}')

    picks = docs[:3]

    for d in picks:
        text = d['text']
        short  = tokenize(text[:MEDIUM_CHARS])
        long_  = tokenize(text[:LONG_CHARS])

        prefix = lcp(short, long_)

        print(f'{d["name"][:24]:<24} med_chars={MEDIUM_CHARS} med_tok={len(short):>6} '
              f'long_chars={LONG_CHARS} long_tok={len(long_):>6} prefix_common={prefix:>6}')

    # cross-document: two documents that open with the same template would share tokens
    if len(picks) >= 2:
        a = tokenize(picks[0]['text'][:MEDIUM_CHARS])
        b = tokenize(picks[1]['text'][:MEDIUM_CHARS])
        print(f'\ncross-document common prefix: {lcp(a, b)} tokens '
              f'(distinct documents should be small)')


def cmd_t01():
    """T01: A -> A in one process. Expect a slot hit, not a disk hit."""
    docs = usable(discover())
    d = docs[0]

    prompt = d['text'][:MEDIUM_CHARS]
    toks = tokenize(prompt)

    stats_before = metrics()
    first = complete(prompt, 8)
    stats_after_first = metrics()
    record('t01-first', first, {'tokens': len(toks), 'doc': d['name'],
                                'stats': parse_stats(log_slice(first and None))})

    time.sleep(2)

    second = complete(prompt, 8)
    record('t01-second', second, {'tokens': len(toks), 'doc': d['name']})

    same = first['sha'] == second['sha']
    print(f'\nT01 output identical: {same}')
    print(f'T01 first cache_n={first["cache_n"]}  second cache_n={second["cache_n"]}')

    # the interesting assertion: the second request reused something locally
    if second['cache_n'] and second['cache_n'] > 0:
        print('T01 slot/cache reuse confirmed')
    else:
        print('T01 WARNING: second request reported no cache reuse')


def cmd_t02():
    """T02: A -> B -> C -> A -> B. Identity must not leak between documents."""
    docs = usable(discover())

    if len(docs) < 3:
        sys.exit(f'T02 needs 3 documents, found {len(docs)}')

    a, b, c = docs[0], docs[1], docs[2]

    pa, pb, pc = (d['text'][:MEDIUM_CHARS] for d in (a, b, c))
    ta, tb, tc = tokenize(pa), tokenize(pb), tokenize(pc)

    print(f'A={a["name"]} {len(ta)} tok   B={b["name"]} {len(tb)} tok   C={c["name"]} {len(tc)} tok')
    print(f'A/B common prefix {lcp(ta, tb)}   A/C {lcp(ta, tc)}   B/C {lcp(tb, tc)}')

    for label, prompt, toks in (('A1', pa, ta), ('B1', pb, tb), ('C1', pc, tc),
                                ('A2', pa, ta), ('B2', pb, tb)):
        e = complete(prompt, 4)
        record(f't02-{label}', e, {'tokens': len(toks), 'prompt_sha': hashlib.sha256(prompt.encode()).hexdigest()[:16]})
        time.sleep(1)

    # the same input twice must give the same output, whatever the cache did
    print('\nA1 vs A2 and B1 vs B2 must match; differs only if the cache changed semantics')


def cmd_t03():
    """T03: after a restart, a request must be served from disk.

    Run the two halves as separate processes: `t03-warm` then restart the server then
    `t03-cold`. The restart is what makes this about the disk tier rather than the RAM
    or slot tier.
    """
    docs = usable(discover())

    if len(docs) < 3:
        sys.exit(f'T03 needs 3 documents, found {len(docs)}')

    a, b, c = docs[0], docs[1], docs[2]
    prompts = [d['text'][:MEDIUM_CHARS] for d in (a, b, c)]
    labels  = ['A', 'B', 'C']

    for prompt, label in zip(prompts, labels):
        e = complete(prompt, 4)
        stats = parse_stats(log_slice(None))
        record(f't03-{label}', e, {'prompt_sha': hashlib.sha256(prompt.encode()).hexdigest()[:16],
                                   'stats': stats})
        print(f'  {label}: disk_reused={stats.get("reused_disk")} source={stats.get("source")}')
        time.sleep(2)

    print('\nwaiting for the background writer to flush...')
    time.sleep(20)

    import glob
    cache_dir = os.environ.get('LLAMA_TEST_CACHE', '')
    if cache_dir:
        bins = glob.glob(os.path.join(cache_dir, '*.bin'))
        print(f'disk entries: {len(bins)}')
        for p in bins:
            print(f'  {os.path.basename(p)}  {os.path.getsize(p)} bytes')


def cmd_t03cold():
    """T03 second half: same documents, fresh process. Look for source=disk."""
    docs = usable(discover())

    if len(docs) < 3:
        sys.exit(f'T03 needs 3 documents, found {len(docs)}')

    a, b, c = docs[0], docs[1], docs[2]
    by_label = {'C': c, 'A': a, 'B': b}

    for label in ('C', 'A', 'B'):
        prompt = by_label[label]['text'][:MEDIUM_CHARS]
        e = complete(prompt, 4)
        stats = parse_stats(log_slice(None))
        record(f't03cold-{label}', e, {'stats': stats,
                                       'prompt_sha': hashlib.sha256(prompt.encode()).hexdigest()[:16]})

        disk = stats.get('reused_disk', '?')
        src  = stats.get('source', '?')
        print(f'  {label}: source={src} reused_disk={disk} restored={stats.get("restored")}')


COMMANDS = {
    'docs': cmd_docs,
    'tokens': cmd_tokens,
    't01': cmd_t01,
    't02': cmd_t02,
    't03': cmd_t03,
    't03cold': cmd_t03cold,
}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        print('commands: ' + ', '.join(COMMANDS))
        sys.exit(1)

    COMMANDS[sys.argv[1]]()


if __name__ == '__main__':
    main()
