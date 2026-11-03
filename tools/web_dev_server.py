"""Serve the radio's web page from the repo and pass /api/ requests on to a running radio.

Usage: python tools/web_dev_server.py <radio address> [port]
Then open http://localhost:<port>/ (default 8765). Edits to index.html only need a reload.
"""
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PAGE = Path(__file__).resolve().parent.parent / "components" / "radio_web" / "index.html"
RADIO = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8765


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            self.reply(200, "text/html; charset=utf-8", PAGE.read_bytes())
        else:
            self.forward()

    def do_POST(self):
        self.forward()

    def forward(self):
        """Pass the request on to the radio and its answer back."""
        if not self.path.startswith("/api/"):
            self.reply(404, "text/plain", b"not found")
            return
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))) if self.command == "POST" else None
        request = urllib.request.Request(f"http://{RADIO}{self.path}", data=body, method=self.command,
                                         headers={"Content-Type": "text/plain"})
        try:
            with urllib.request.urlopen(request, timeout=5) as answer:
                self.reply(answer.status, answer.headers.get("Content-Type", "text/plain"), answer.read())
        except urllib.error.HTTPError as error:
            self.reply(error.code, "text/plain", error.read())
        except OSError as error:
            self.reply(502, "text/plain", f"Radio not reachable: {error}".encode())

    def reply(self, code, content_type, data):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        # Status polls every second would flood the console; show the rest.
        if "/api/status" not in self.path:
            super().log_message(fmt, *args)


print(f"http://localhost:{PORT}/ -> radio {RADIO}")
ThreadingHTTPServer(("", PORT), Handler).serve_forever()
