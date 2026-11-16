#!/usr/bin/env python3
"""Text/media alternation around the disk tier, and the crash regression.

The sequence that used to abort the server is the second case here: a text-only
snapshot restored from disk, then an image request. Every case prints the cache source
that actually served it, because "the answer was right" and "the cache did the right
thing" are separate claims.

    python test-run/cache_mixed_matrix.py alternation
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cache_functional_matrix as M
import cache_media_matrix as MM


def show(label, prompt_label, entry, stats):
    print(f'  {label:<22} {prompt_label:<14} source={stats.get("source", "?"):<5} '
          f'disk={stats.get("reused_disk", "?"):<6} ram={stats.get("reused_ram", "?"):<6} '
          f'slot={stats.get("reused_slot", "?"):<5} prefill={stats.get("prefill", "?"):<6} '
          f'sha={entry["sha"]}')


def text(doc, n_chars=3500, n_predict=2):
    e = M.complete(doc['text'][:n_chars], n_predict)
    return e, M.parse_stats(M.log_slice())


def img(path):
    r = MM.chat(path, MM.QUESTION)
    return r, M.parse_stats(M.log_slice())


def alternation():
    """The plan's mixed sequences, run against a warm disk cache.

    Order matters: the text snapshot has to be restored *before* the image request,
    because that is the transition that used to assert.
    """
    docs = M.usable(M.discover())

    if len(docs) < 3:
        sys.exit(f'need 3 documents, found {len(docs)}')

    a, b, c = docs[0], docs[1], docs[2]

    print('=== case 1: A(disk) -> A(disk) -> 1.png -> 2.png -> A(disk) ===')
    for label, kind in (('A restore', a), ('A restore 2', a)):
        e, st = text(kind)
        show(label, kind['name'], e, st)
        time.sleep(2)

    e1, st1 = img(MM.IMG1)
    show('1.png', 'image', e1, st1)
    time.sleep(2)

    e2, st2 = img(MM.IMG2)
    show('2.png', 'image', e2, st2)
    time.sleep(2)

    e3, st3 = text(a)
    show('A restore 3', a['name'], e3, st3)

    print('\n=== case 2: B(disk) -> 2.png -> 1.png -> B(disk) ===')
    for label, kind in (('B restore', b),):
        e, st = text(kind)
        show(label, kind['name'], e, st)
        time.sleep(2)

    e4, st4 = img(MM.IMG2)
    show('2.png', 'image', e4, st4)
    time.sleep(2)

    e5, st5 = img(MM.IMG1)
    show('1.png', 'image', e5, st5)
    time.sleep(2)

    e6, st6 = text(b)
    show('B restore 2', b['name'], e6, st6)

    print('\n=== case 3: A -> B -> C -> A (text only, cache source per step) ===')
    for label, kind in (('A', a), ('B', b), ('C', c), ('A again', a)):
        e, st = text(kind)
        show(label, kind['name'], e, st)
        time.sleep(2)

    print('\n=== case 4: same image twice, then the other image ===')
    for i in range(2):
        e, st = img(MM.IMG1)
        show(f'1.png #{i + 1}', 'image', e, st)
        time.sleep(2)

    e7, st7 = img(MM.IMG2)
    show('2.png', 'image', e7, st7)

    print('\n=== identity check (the answers, not the counters) ===')
    print(f'  1.png reproducible : {e1["sha"] == e5["sha"]}')
    print(f'  2.png reproducible : {e2["sha"] == e4["sha"]}')
    print(f'  1.png != 2.png     : {e1["sha"] != e2["sha"]}')


def mixed_prompt():
    """One prompt carrying both text and an image, repeated.

    The plan asks for this shape explicitly: text before and after the media chunk in
    a single request, so the placeholder lands in the middle of the token run rather
    than at the end.
    """
    docs = M.usable(M.discover())
    a = docs[0]

    head = a['text'][:1500]
    tail = a['text'][3000:3600]

    for i in range(2):
        body = {
            'model': 'localModel',
            'messages': [{
                'role': 'user',
                'content': [
                    {'type': 'text', 'text': head},
                    {'type': 'image_url', 'image_url': {'url': MM.data_url(MM.IMG1)}},
                    {'type': 'text', 'text': tail + '\n请用一句话说明这张图片是什么。'},
                ],
            }],
            'temperature': 0.0,
            'n_predict': 120,
            'cache_prompt': True,
            'stream': False,
        }

        t0 = time.time()
        data = M.api('/v1/chat/completions', body)
        wall = time.time() - t0

        msg = data['choices'][0]['message']
        produced = ((msg.get('reasoning_content') or '') + (msg.get('content') or '')).strip()

        print(f'  run {i + 1}: wall={wall:.2f}s produced={len(produced)} chars '
              f'cached={data["usage"]["prompt_tokens_details"].get("cached_tokens")}')
        print(f'           {produced[:90]!r}')

        time.sleep(3)


COMMANDS = {'alternation': alternation, 'mixed': mixed_prompt}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print('commands: ' + ', '.join(COMMANDS))
        sys.exit(1)
    COMMANDS[sys.argv[1]]()


if __name__ == '__main__':
    main()
