import hashlib
import json
import os
import sys
import time
import urllib.request

PORT = int(os.environ.get('LLAMA_TEST_PORT', '18090'))

# The harness drives one long text corpus so that prompts of different lengths
# share a real prefix. Set LLAMA_TEST_CORPUS to any large UTF-8 text file; the
# corpus itself is not part of the repository.
CORPUS_PATH = os.environ.get('LLAMA_TEST_CORPUS', '')


def load_corpus():
    if not CORPUS_PATH:
        sys.exit('LLAMA_TEST_CORPUS is not set; point it at a large UTF-8 text file')
    if not os.path.exists(CORPUS_PATH):
        sys.exit(f'corpus not found: {CORPUS_PATH}')
    with open(CORPUS_PATH, encoding='utf-8', errors='replace') as f:
        return f.read()


def post(prompt, n_predict):
    body = json.dumps({
        'prompt': prompt,
        'n_predict': n_predict,
        'temperature': 0.0,
        'cache_prompt': True,
        'stream': False,
    }).encode('utf-8')
    req = urllib.request.Request(
        f'http://127.0.0.1:{PORT}/completion',
        data=body,
        headers={'Content-Type': 'application/json'},
    )
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=3600) as r:
        data = json.loads(r.read().decode('utf-8'))
    return time.time() - t0, data


def main():
    # usage: probe.py <chars> <n_predict> <label> [suffix]
    #        chars "start:end" slices the corpus instead of taking a prefix
    #        suffix "ext:N" extends the prompt with the corpus up to character N,
    #        so the prompt stays a strict extension of the shorter one
    if len(sys.argv) < 4:
        sys.exit('usage: probe.py <chars|start:end> <n_predict> <label> [ext:N]')

    spec      = sys.argv[1]
    chars     = int(spec.split(':')[0])
    n_predict = int(sys.argv[2])
    label     = sys.argv[3]
    suffix    = sys.argv[4] if len(sys.argv) > 4 else ''

    corpus = load_corpus()

    if suffix.startswith('ext:'):
        suffix = corpus[chars:int(suffix[4:])]

    if ':' in spec:
        start, end = spec.split(':')
        prompt = corpus[int(start):int(end)] + suffix
    else:
        prompt = corpus[:chars] + suffix

    dt, data = post(prompt, n_predict)
    tm = data.get('timings', {})

    # the continuation is written locally only: it is derived from the corpus and
    # is not part of the published repository (see .gitignore)
    content = data.get('content', '')
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'logs')
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, f'out-{label}.txt'), 'w', encoding='utf-8') as f:
        f.write(content)

    digest = hashlib.sha256(content.encode('utf-8')).hexdigest()[:16]

    print(f"[{label}] wall={dt:6.2f}s prompt_n={tm.get('prompt_n')} cache_n={tm.get('cache_n')} "
          f"prompt_ms={tm.get('prompt_ms', 0):8.1f} predicted_n={tm.get('predicted_n')} "
          f"predicted_ms={tm.get('predicted_ms', 0):7.1f} chars={len(prompt)} sha={digest}", flush=True)


if __name__ == '__main__':
    main()
