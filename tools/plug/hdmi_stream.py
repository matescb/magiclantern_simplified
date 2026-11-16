#!/usr/bin/env python3
"""Serve the HDMI grabber (/dev/video2, MACROSILICON USB capture) as an MJPEG stream viewable in a browser."""
import http.server
import shutil
import subprocess
import sys

DEVICE = "/dev/video2"
WIDTH, HEIGHT, FPS = 1280, 720, 30
PORT = 8090
BOUNDARY = "frame"

GST_CMD = [
    "gst-launch-1.0", "-q",
    "v4l2src", f"device={DEVICE}",
    "!", f"image/jpeg,width={WIDTH},height={HEIGHT},framerate={FPS}/1",
    "!", "multipartmux", f"boundary={BOUNDARY}",
    "!", "fdsink", "fd=1",
]

INDEX_HTML = f"""<!doctype html>
<html><head><title>HDMI Grabber</title>
<style>body{{margin:0;background:#111;display:flex;justify-content:center;align-items:center;height:100vh}}
img{{max-width:100%;max-height:100%}}</style>
</head><body><img src="/stream"></body></html>"""


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/":
            body = INDEX_HTML.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == "/stream":
            self.send_response(200)
            self.send_header(
                "Content-Type", f"multipart/x-mixed-replace; boundary={BOUNDARY}"
            )
            self.end_headers()
            proc = subprocess.Popen(GST_CMD, stdout=subprocess.PIPE)
            try:
                shutil.copyfileobj(proc.stdout, self.wfile, length=65536)
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                proc.terminate()
                proc.wait()
            return

        self.send_response(404)
        self.end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


if __name__ == "__main__":
    server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"Serving HDMI grabber stream on http://0.0.0.0:{PORT}/")
    server.serve_forever()
