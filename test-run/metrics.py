import sys
import urllib.request

PORT = 18090
PREFIXES = ('llamacpp:cache_', 'llamacpp:prompt_tokens', 'llamacpp:cache_disk_reject')


def main():
    # usage: metrics.py            -> print the cache related series
    #        metrics.py all        -> print every series
    want_all = len(sys.argv) > 1 and sys.argv[1] == 'all'

    with urllib.request.urlopen(f'http://127.0.0.1:{PORT}/metrics', timeout=60) as r:
        text = r.read().decode('utf-8')

    for line in text.splitlines():
        if line.startswith('#') or not line.strip():
            continue
        if want_all or any(line.startswith(p) for p in PREFIXES):
            print(line, flush=True)


if __name__ == '__main__':
    main()
