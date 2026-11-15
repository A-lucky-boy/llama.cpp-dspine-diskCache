import json, os, sys, time, urllib.request

PORT = int(os.environ.get('LLAMA_TEST_PORT', '18090'))

# see probe.py: the corpus is any large local UTF-8 text file and is not shipped
CORPUS_PATH = os.environ.get('LLAMA_TEST_CORPUS', '')

if not CORPUS_PATH:
    sys.exit('LLAMA_TEST_CORPUS is not set; point it at a large UTF-8 text file')
if not os.path.exists(CORPUS_PATH):
    sys.exit('corpus not found: ' + CORPUS_PATH)

with open(CORPUS_PATH, encoding='utf-8', errors='replace') as f:
    corpus = f.read()

def post(prompt, n_predict=4, label=''):
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
    with urllib.request.urlopen(req, timeout=1800) as r:
        data = json.loads(r.read().decode('utf-8'))
    dt = time.time() - t0
    tm = data.get('timings', {})
    print(f"[{label}] wall={dt:6.2f}s prompt_n={tm.get('prompt_n')} cache_n={tm.get('cache_n')} "
          f"prompt_ms={tm.get('prompt_ms', 0):8.1f} predicted_n={tm.get('predicted_n')} "
          f"predicted_ms={tm.get('predicted_ms', 0):7.1f}")
    print(f"    text={data.get('content','')[:40]!r}")
    return tm

if __name__ == '__main__':
    nchars = int(sys.argv[1])
    label = sys.argv[2] if len(sys.argv) > 2 else 'req'
    n_predict = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    prompt = corpus[:nchars]
    print(f"prompt chars={len(prompt)}")
    post(prompt, n_predict=n_predict, label=label)
