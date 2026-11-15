#!/usr/bin/env python3
"""Decode/prefill benchmark for the local llama-server deployment.

Every number comes from the server's own `timings` block, so all variants are
measured identically and no log parsing is involved.

Context accounting: prompts are prefixes of one corpus, so a later request
reuses the earlier prefix. The real context length of a request is therefore
`prompt_n + cache_n`, not `prompt_n` alone.

Speculative-decode accounting:
  rounds      = draft_n / n_max          (one draft block per round)
  mean_len    = predicted_n / rounds     (tokens produced per verification)
  per_round   = predicted_ms / rounds
The break-even point of speculative decoding is mean_len > per_round / t_nospec.
"""

import argparse
import json
import os
import sys
import time
import urllib.request

# the benchmark corpus designated for this deployment (natural-language prose)
_DEFAULT_CORPUS = r"E:\小说\正文.md"


def load_corpus(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


USE_CACHE = True


def call(base_url, api_key, prompt, n_predict, timeout):
    body = json.dumps({
        "prompt": prompt,
        "n_predict": n_predict,
        "temperature": 0.0,
        "top_k": 1,
        "cache_prompt": USE_CACHE,
        "stream": False,
    }).encode("utf-8")
    req = urllib.request.Request(
        base_url.rstrip("/") + "/completion",
        data=body,
        headers={"Content-Type": "application/json", "Authorization": "Bearer " + api_key},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.load(resp)


def summarize(t):
    pn, pms = t.get("prompt_n", 0), t.get("prompt_ms", 0.0) or 0.0
    dn, dms = t.get("predicted_n", 0), t.get("predicted_ms", 0.0) or 0.0
    return {
        "prompt_n": pn,
        "cache_n": t.get("cache_n", 0),
        "prompt_ms": round(pms, 1),
        "prefill_ts": round(pn * 1000.0 / pms, 1) if pms > 0 else None,
        "predicted_n": dn,
        "predicted_ms": round(dms, 1),
        "decode_ts": round(dn * 1000.0 / dms, 1) if dms > 0 else None,
        "ms_per_token": round(dms / dn, 2) if dn else None,
        "draft_n": t.get("draft_n", 0),
        "draft_accepted": t.get("draft_n_accepted", 0),
    }


def wait_health(base_url, api_key, timeout_s):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        try:
            req = urllib.request.Request(base_url.rstrip("/") + "/health",
                                         headers={"Authorization": "Bearer " + api_key})
            with urllib.request.urlopen(req, timeout=5) as resp:
                if json.load(resp).get("status") == "ok":
                    return True
        except Exception:
            pass
        time.sleep(2)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--corpus", default=_DEFAULT_CORPUS)
    ap.add_argument("--base-url", default="http://127.0.0.1:18080")
    ap.add_argument("--api-key", default="sk-0")
    ap.add_argument("--sizes", default="6000,36000,100000", help="target context sizes in tokens")
    ap.add_argument("--n-predict", type=int, default=128)
    ap.add_argument("--n-max", type=int, default=15, help="drafted tokens per round")
    ap.add_argument("--phases", default="both", choices=["both", "prefill", "decode"])
    ap.add_argument("--timeout", type=float, default=1200.0)
    ap.add_argument("--out", default="results.jsonl")
    ap.add_argument("--notes", default="")
    ap.add_argument("--no-cache", action="store_true", help="always prefill the whole prompt")
    args = ap.parse_args()

    global USE_CACHE
    USE_CACHE = not args.no_cache

    if not wait_health(args.base_url, args.api_key, 300):
        print("ERROR: server not healthy", file=sys.stderr)
        return 2

    text = load_corpus(args.corpus)
    # warm-up request, discarded: pays for CUDA graph construction
    call(args.base_url, args.api_key, "warmup " * 32, 16, args.timeout)

    # calibrate chars-per-token from a short prefix (cache is empty for it)
    probe = summarize(call(args.base_url, args.api_key, text[:4000], 8, args.timeout)["timings"])
    cpt = 4000.0 / max(1, probe["prompt_n"] + probe["cache_n"])
    print("corpus: %s  chars=%d  calibration: %.3f chars/token" % (os.path.basename(args.corpus), len(text), cpt))

    print("%-8s %8s %10s %9s %9s %10s %8s %8s" %
          ("target", "ctx", "prefill_ts", "decode_ts", "ms/tok", "per_round", "accept", "mean_len"))
    rows = []
    for size in [int(x) for x in args.sizes.split(",") if x.strip()]:
        target_chars = min(len(text), int(size * cpt))
        prompt = text[:target_chars]
        row = {"tag": args.tag, "target_tokens": size, "target_chars": target_chars, "notes": args.notes}

        first = summarize(call(args.base_url, args.api_key, prompt, args.n_predict, args.timeout)["timings"])
        row["first"] = first
        ctx = first["prompt_n"] + first["cache_n"] + first["predicted_n"]

        decode_ts = ms_tok = accept = mean_len = per_round = None
        if args.phases in ("both", "decode"):
            s = summarize(call(args.base_url, args.api_key, prompt, args.n_predict, args.timeout)["timings"])
            row["second"] = s
            ctx = s["prompt_n"] + s["cache_n"] + s["predicted_n"]
            decode_ts, ms_tok = s["decode_ts"], s["ms_per_token"]
            if s["draft_n"]:
                rounds = s["draft_n"] / float(args.n_max)
                accept = round(s["draft_accepted"] / s["draft_n"], 3)
                mean_len = round(s["predicted_n"] / max(1.0, rounds), 2)
                per_round = round(s["predicted_ms"] / max(1.0, rounds), 1)
                row["rounds"] = round(rounds, 1)
                row["accept_rate"] = accept
                row["mean_len"] = mean_len
                row["per_round_ms"] = per_round

        row["ctx_tokens"] = ctx
        print("%-8d %8d %10s %9s %9s %10s %8s %8s" %
              (size, ctx, first["prefill_ts"], decode_ts, ms_tok, per_round, accept, mean_len))
        rows.append(row)

    with open(args.out, "a", encoding="utf-8") as fh:
        for r in rows:
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")
    print("appended %d rows to %s" % (len(rows), os.path.abspath(args.out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
