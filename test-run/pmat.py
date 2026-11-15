import hashlib
import json
import os
import sys
import time
import urllib.request

# The target scenario: several requests share a long text prefix A and then
# diverge. `probe.py` can only extend a prefix, which makes every request a
# strict prefix of a longer one - the same situation the RAM tier already
# covers. Here each request is A + a different tail, so no existing snapshot
# covers the other requests and A itself is the only shared part.

PORT = int(os.environ.get('LLAMA_TEST_PORT', '18090'))
CORPUS_PATH = os.environ.get('LLAMA_TEST_CORPUS', '')


def load_corpus():
    if not CORPUS_PATH:
        sys.exit('LLAMA_TEST_CORPUS is not set; point it at a large UTF-8 text file')
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
    # usage: pmat.py <shared_chars> <tail_start:tail_end> <n_predict> <label>
    if len(sys.argv) < 5:
        sys.exit('usage: pmat.py <shared_chars> <tail_start:tail_end> <n_predict> <label>')

    shared  = int(sys.argv[1])
    t0s, t1s = sys.argv[2].split(':')
    tail    = slice(int(t0s), int(t1s))
    n_pred  = int(sys.argv[3])
    label   = sys.argv[4]

    corpus = load_corpus()
    prompt = corpus[:shared] + corpus[tail]

    dt, data = post(prompt, n_pred)
    tm = data.get('timings', {})

    content = data.get('content', '')
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'logs')
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, f'out-{label}.txt'), 'w', encoding='utf-8') as f:
        f.write(content)

    digest = hashlib.sha256(content.encode('utf-8')).hexdigest()[:16]

    print(f"[{label}] wal={dt:6.2f}s prompt_n={tm.get('prompt_n')} cache_n={tm.get('cache_n')} "
          f"prompt_ms={tm.get('prompt_ms', 0):8.1f} predicted_n={tm.get('predicted_n')} "
          f"chars={len(prompt)} sha={digest}", flush=True)


if __name__ == '__main__':
    main()
