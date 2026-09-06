#!/usr/bin/env python3
"""A 40-line fake OpenAI-compatible /v1/embeddings server.

Exists so the embeddings backend picker can be exercised end-to-end without
Ollama, without a GPU, and without a corporate gateway: it returns
deterministic unit-norm vectors of whatever dimension you ask for.

    python3 scripts/fake_embed_server.py --port 8099 --dim 768

Then in agentty: Ctrl+K -> "Retrieval (RAG)" -> e
    Backend  = Custom endpoint
    Host     = 127.0.0.1
    Port     = 8099
    Model    = anything
    Enter on "Test connection"  ->  should report the dim you launched with.

Change --dim and re-test to watch the identity hash (and therefore the
persisted .ragdb filename) change with the vector space.
"""

import argparse, hashlib, json, math
from http.server import BaseHTTPRequestHandler, HTTPServer

DIM = 768


def embed(text: str, dim: int):
    """Deterministic pseudo-embedding: hash-seeded, then L2-normalised."""
    h = hashlib.sha256(text.encode("utf-8")).digest()
    vals = [((h[i % len(h)] + i * 31) % 251) / 251.0 - 0.5 for i in range(dim)]
    norm = math.sqrt(sum(v * v for v in vals)) or 1.0
    return [v / norm for v in vals]


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0) or 0))
        try:
            req = json.loads(body or b"{}")
        except json.JSONDecodeError:
            req = {}
        inp = req.get("input", "")
        texts = inp if isinstance(inp, list) else [inp]

        print(f"  -> {self.path}  {len(texts)} text(s), auth="
              f"{'yes' if self.headers.get('Authorization') else 'no'}")

        out = {
            "object": "list",
            "model": req.get("model", "fake-embed"),
            "data": [
                {"object": "embedding", "index": i, "embedding": embed(t, DIM)}
                for i, t in enumerate(texts)
            ],
            "usage": {"prompt_tokens": 0, "total_tokens": 0},
        }
        payload = json.dumps(out).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_):
        pass   # we print our own, quieter line


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--dim", type=int, default=768)
    args = ap.parse_args()
    DIM = args.dim
    print(f"fake embeddings server on http://127.0.0.1:{args.port}  dim={DIM}")
    print("point agentty at it:  Backend=Custom endpoint  Host=127.0.0.1  "
          f"Port={args.port}")
    HTTPServer(("127.0.0.1", args.port), Handler).serve_forever()
