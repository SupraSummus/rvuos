#!/usr/bin/env python3
"""Hold one connection to a camera's MJPEG stream open, and serve its frames on this machine to as many as ask.

A phone's camera app serves each viewer a stream of its own, and slows for every one more,
so probe.py, a snapshot and a person's browser each asking the phone would have it send each a few frames a second;
through this, the phone sends one stream, and each asks here instead, where a connection costs nothing.
The frames pass through as the camera made them, cut from the stream at each JPEG's start and end markers.

  /video.mjpeg  the stream, each frame as it comes, which probe.py's --camera and a browser take
  /frame.jpg    the latest frame
  /             a page that shows the stream

Usage: relay.py [--port 8090] url
"""

import argparse
import http.server
import ssl
import sys
import threading
import time
import urllib.request

SOI, EOI = b"\xff\xd8", b"\xff\xd9"
BOUNDARY = b"frame"


class Latest:
    """The latest frame, its count, and a condition every viewer waits on for the next."""

    def __init__(self):
        self.jpeg = None
        self.count = 0
        self.cond = threading.Condition()

    def put(self, jpeg):
        with self.cond:
            self.jpeg = jpeg
            self.count += 1
            self.cond.notify_all()

    def next(self, after, timeout=5.0):
        """The first frame past count after, and its count; None if none came in time."""
        with self.cond:
            if not self.cond.wait_for(lambda: self.count > after, timeout):
                return None, after
            return self.jpeg, self.count


def pull(url, latest):
    """Read the camera's stream for good, reconnecting when it drops, and cut each JPEG out of it."""
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    while True:
        try:
            with urllib.request.urlopen(url, context=context, timeout=10) as stream:
                print(f"relay: connected to {url}", flush=True)
                buffer = b""
                while True:
                    chunk = stream.read1(65536)
                    if not chunk:
                        break
                    buffer += chunk
                    while True:
                        start = buffer.find(SOI)
                        end = buffer.find(EOI, start + 2) if start >= 0 else -1
                        if start < 0 or end < 0:
                            buffer = buffer[start:] if start >= 0 else buffer[-1:]
                            break
                        latest.put(buffer[start:end + 2])
                        buffer = buffer[end + 2:]
        except OSError as e:
            print(f"relay: {url}: {e}; connecting again", flush=True)
        time.sleep(1)


PAGE = b"""<!doctype html><title>camera</title>
<body style="margin:0;background:#111"><img src="/video.mjpeg" style="width:100%;height:100vh;object-fit:contain"></body>"""


def handler(latest):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            if self.path == "/":
                self.reply(200, "text/html", PAGE)
            elif self.path == "/frame.jpg":
                jpeg, _ = latest.next(latest.count - 1)
                if jpeg is None:
                    self.reply(503, "text/plain", b"no frame yet\n")
                else:
                    self.reply(200, "image/jpeg", jpeg)
            elif self.path == "/video.mjpeg":
                self.send_response(200)
                self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=" + BOUNDARY.decode())
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                seen = latest.count
                try:
                    while True:
                        jpeg, seen = latest.next(seen)
                        if jpeg is None:
                            continue
                        self.wfile.write(b"--" + BOUNDARY + b"\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                                         str(len(jpeg)).encode() + b"\r\n\r\n" + jpeg + b"\r\n")
                        self.wfile.flush()
                except OSError:
                    pass
            else:
                self.reply(404, "text/plain", b"no such page\n")

        def reply(self, code, kind, body):
            self.send_response(code)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("url")
    args = parser.parse_args()
    latest = Latest()
    threading.Thread(target=pull, args=(args.url, latest), daemon=True).start()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler(latest))
    server.daemon_threads = True
    print(f"relay: serving on http://127.0.0.1:{args.port}/", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
