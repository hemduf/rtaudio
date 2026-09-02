#!/usr/bin/env python3
"""Serve an Emscripten WebAudio build with cross-origin isolation headers."""

from __future__ import annotations

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class WebAudioHandler(SimpleHTTPRequestHandler):
    def end_headers(self) -> None:
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Serve RtAudio WebAudio examples with COOP/COEP headers."
    )
    parser.add_argument(
        "directory",
        nargs="?",
        default="build-wasm/tests",
        help="directory containing generated HTML/JS/WASM files",
    )
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()

    directory = Path(args.directory).resolve()
    if not directory.is_dir():
        parser.error(f"directory does not exist: {directory}")

    handler = partial(WebAudioHandler, directory=str(directory))
    server = ThreadingHTTPServer((args.bind, args.port), handler)

    print(f"Serving {directory} at http://{args.bind}:{args.port}/")
    print("COOP/COEP enabled; Ctrl-C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
