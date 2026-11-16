#!/usr/bin/env python3
"""Multimodal cache matrix: two real PNGs, same image / different image / text split.

The plan's precondition is a vision-capable model plus a compatible mmproj. That is
checked first, and if the model cannot actually process an image the tests are reported
as *not run* rather than as cache failures - a text-only server rejecting an image says
nothing about the cache.

Images are sent as base64 data URLs through the chat endpoint, which is how the server
accepts them; a file path in the JSON is not an input the API understands.

The two failure modes this is looking for are the ones that matter for correctness:

  * the same image with the same question must be reusable
  * a *different* image must never be served from the first image's state
"""

import base64
import hashlib
import json
import os
import struct
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cache_functional_matrix as M

IMG1 = os.environ.get('LLAMA_TEST_IMG1', r'C:\Users\Administrator\Desktop\1.png')
IMG2 = os.environ.get('LLAMA_TEST_IMG2', r'C:\Users\Administrator\Desktop\2.png')

QUESTION = os.environ.get('LLAMA_TEST_QUESTION',
                          '请用一句话描述这张图片的主要内容。')


def check_png(path):
    """File-level validation, independent of any model.

    The format is verified from the signature and the IHDR chunk rather than trusted,
    and the content hash is recorded so two files being *different* is established from
    the bytes - not from the fact that they have different names.
    """
    if not os.path.exists(path):
        sys.exit(f'image not found: {path}')

    raw = open(path, 'rb').read()

    if raw[:8] != b'\x89PNG\r\n\x1a\n':
        sys.exit(f'not a PNG: {path}')

    if raw[12:16] != b'IHDR':
        sys.exit(f'no IHDR chunk: {path}')

    w, h = struct.unpack('>II', raw[16:24])
    depth, color = raw[24], raw[25]

    return {
        'path': path,
        'sha256': hashlib.sha256(raw).hexdigest(),
        'bytes': len(raw),
        'w': w,
        'h': h,
        'bit_depth': depth,
        'color_type': color,
    }


def data_url(path):
    raw = open(path, 'rb').read()
    return 'data:image/png;base64,' + base64.b64encode(raw).decode('ascii')


def chat(image_path, question, n_predict=160):
    """One image + one question through the chat endpoint."""
    body = {
        'model': 'localModel',
        'messages': [{
            'role': 'user',
            'content': [
                {'type': 'image_url', 'image_url': {'url': data_url(image_path)}},
                {'type': 'text', 'text': question},
            ],
        }],
        'temperature': 0.0,
        'n_predict': n_predict,
        'cache_prompt': True,
        'stream': False,
    }

    t0 = time.time()
    data = M.api('/v1/chat/completions', body)
    wall = time.time() - t0

    content, reasoning = '', ''
    try:
        msg = data['choices'][0]['message']
        content = msg.get('content') or ''
        # this model answers inside reasoning_content until it finishes thinking.
        # A small n_predict can be spent entirely on it, leaving `content` empty -
        # which would look like a vision failure when the model saw the image fine
        reasoning = msg.get('reasoning_content') or ''
    except (KeyError, IndexError, TypeError):
        content = json.dumps(data)[:200]

    # the answer for comparison purposes is whatever the model produced, wherever it
    # put it; hashing only `content` would report two different images as identical
    # whenever both were still thinking
    produced = (reasoning + chr(10) + content).strip()

    return {
        'wall': wall,
        'content': content,
        'reasoning': reasoning,
        'sha': hashlib.sha256(produced.encode('utf-8')).hexdigest()[:16],
        'produced_chars': len(produced),
        'usage': data.get('usage', {}),
        'timings': data.get('timings', {}),
        'raw': data,
    }


def capability_probe():
    """Does this model actually accept and answer an image request? """
    print('--- file-level validation')
    a, b = check_png(IMG1), check_png(IMG2)

    for tag, i in (('1.png', a), ('2.png', b)):
        print(f'  {tag:<8} {i["w"]}x{i["h"]} depth={i["bit_depth"]} color={i["color_type"]} '
              f'{i["bytes"]:>8} bytes sha={i["sha256"][:16]}')

    if a['sha256'] == b['sha256']:
        print('  NOTE: the two files have identical bytes; they are not two images')
    else:
        print('  the two files differ in bytes and dimensions')

    print('\n--- capability probe (cold, no cache)')
    try:
        r = chat(IMG1, QUESTION, n_predict=120)
    except urllib.error.HTTPError as e:
        print(f'  request failed: HTTP {e.code} {e.reason}')
        print('  => the server cannot process image input in this configuration')
        return False, a, b

    usage = r['usage']
    print(f'  answered in {r["wall"]:.2f}s, sha={r["sha"]}, produced {r["produced_chars"]} chars')
    print(f'  prompt_tokens={usage.get("prompt_tokens")} '
          f'cached={usage.get("prompt_tokens_details", {}).get("cached_tokens")}')

    if r['content'].strip():
        print(f'  content   : {r["content"][:110]!r}')
    if r['reasoning'].strip():
        print(f'  reasoning : {r["reasoning"][:110]!r}')

    if not (r['content'].strip() or r['reasoning'].strip()):
        print('  => the model produced nothing for the image')
        return False, a, b

    print('  => the model processed the image (vision path works)')
    return True, a, b


def m01():
    """Same image, same question, twice."""
    ok, a, b = capability_probe()
    if not ok:
        print('\nM01 NOT RUN: no working vision path')
        return

    print('\n--- M01: 1.png + Q  ->  1.png + Q')
    first = chat(IMG1, QUESTION)
    print(f'  first : sha={first["sha"]} wall={first["wall"]:.2f}s')
    time.sleep(3)

    second = chat(IMG1, QUESTION)
    print(f'  second: sha={second["sha"]} wall={second["wall"]:.2f}s')

    print(f'  identical output: {first["sha"] == second["sha"]}')


def m02():
    """1.png -> 2.png -> 1.png. The second image must not be served from the first."""
    ok, a, b = capability_probe()
    if not ok:
        print('\nM02 NOT RUN: no working vision path')
        return

    print('\n--- M02: 1.png -> 2.png -> 1.png')
    r1 = chat(IMG1, QUESTION)
    print(f'  1.png first : sha={r1["sha"]} wall={r1["wall"]:.2f}s')
    time.sleep(3)

    r2 = chat(IMG2, QUESTION)
    print(f'  2.png       : sha={r2["sha"]} wall={r2["wall"]:.2f}s')
    time.sleep(3)

    r3 = chat(IMG1, QUESTION)
    print(f'  1.png again : sha={r3["sha"]} wall={r3["wall"]:.2f}s')

    print(f'\n  1.png reproducible      : {r1["sha"] == r3["sha"]}')
    print(f'  2.png distinct from 1.png: {r2["sha"] != r1["sha"]}')

    if r2['sha'] == r1['sha']:
        print('  WARNING: both images produced the same answer - check whether the model')
        print('           actually saw different images before drawing a cache conclusion')


COMMANDS = {'probe': lambda: capability_probe(), 'm01': m01, 'm02': m02}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print('commands: ' + ', '.join(COMMANDS))
        sys.exit(1)
    COMMANDS[sys.argv[1]]()


if __name__ == '__main__':
    main()
