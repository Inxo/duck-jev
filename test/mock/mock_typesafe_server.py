#!/usr/bin/env python3
"""Deterministic mock of the TypeSafe System One API used by the integration tests.

Answers are derived from the request so tests can assert exact values:
  * noul    -> 0.9 if the state mentions "refund", else 0.1
  * choice  -> the first label that appears in the state (case-insensitive), else the last label
  * score   -> number of "!" characters in the state, capped at the top rubric level
Special states:
  * "__retry__"  -> the first request with this body fails with 503 (Retry-After: 0), the retry succeeds
  * "__invalid__" -> 422 with a FastAPI-style validation error
Usage: mock_typesafe_server.py [port]   (prints "READY <port>" once listening)
"""

import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API_KEY = "test-key"
LOCK = threading.Lock()
SEEN_RETRY = set()
STATS = {"systemone": 0}


def state_text(state):
    return state if isinstance(state, str) else json.dumps(state, sort_keys=True)


def answer(question, text):
    lowered = text.lower()
    kind = question.get("type")
    if kind == "noul":
        return {"type": "noul", "noul": 0.9 if "refund" in lowered else 0.1}
    if kind == "choice":
        labels = list(question["criteria"].keys())
        chosen = next((label for label in labels if label.lower() in lowered), labels[-1])
        rest = (0.2 / (len(labels) - 1)) if len(labels) > 1 else 0.0
        probabilities = {label: (0.8 if label == chosen else rest) for label in labels}
        if len(labels) == 1:
            probabilities[chosen] = 1.0
        return {"type": "choice", "choice": chosen, "confidence": 0.8, "probabilities": probabilities}
    if kind == "score":
        levels = question["criteria"]
        score = min(len(levels) - 1, text.count("!"))
        return {
            "type": "score",
            "score": float(score),
            "confidence": 0.9,
            "legend": {str(i): d for i, d in enumerate(levels)},
            "probabilities": {str(i): (1.0 if i == score else 0.0) for i in range(len(levels))},
        }
    raise ValueError(f"unknown question type {kind!r}")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send_json(self, status, payload, headers=None):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("x-typesafe-request-id", "req_mock")
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def authorized(self):
        if self.headers.get("Authorization") != f"Bearer {API_KEY}":
            self.send_json(401, {"error": {"message": "Invalid API key."}})
            return False
        return True

    def do_GET(self):
        if self.path == "/__stats":
            with LOCK:
                return self.send_json(200, dict(STATS))
        if not self.authorized():
            return
        if self.path.endswith("/v1/models"):
            return self.send_json(200, {"models": [
                {"name": "jev-1.13", "description": "Jev System One model", "release_date": "2026-09-01"},
                {"name": "jev-latest", "description": "Alias of the newest Jev", "release_date": "2026-09-01"},
            ]})
        self.send_json(404, {"detail": "Not Found"})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        if not self.authorized():
            return
        if not self.path.endswith("/v1/systemone"):
            return self.send_json(404, {"detail": "Not Found"})
        with LOCK:
            STATS["systemone"] += 1
        request = json.loads(raw)
        text = state_text(request.get("state"))
        if text == "__invalid__":
            return self.send_json(422, {"detail": [{"loc": ["body", "state"], "msg": "state is not allowed"}]})
        if text == "__retry__":
            with LOCK:
                first = raw not in SEEN_RETRY
                SEEN_RETRY.add(raw)
            if first:
                return self.send_json(503, {"error": "temporarily unavailable"}, {"Retry-After": "0"})
        questions = request.get("questions") or {}
        if not questions:
            return self.send_json(400, {"error": "At least one question is required."})
        try:
            answers = {name: answer(q, text) for name, q in questions.items()}
        except (ValueError, KeyError, TypeError) as exc:
            return self.send_json(400, {"error": str(exc)})
        self.send_json(200, {
            "model": request.get("model"),
            "answers": answers,
            "usage": {"input_tokens": len(text), "output_tokens": len(answers)},
        })


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"READY {server.server_address[1]}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
