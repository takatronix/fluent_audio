#!/usr/bin/env python3
"""Browser check: the real AudioWorklet in headless Chromium (OfflineAudioContext) must give the same bits as the
native core for every look, and report µs per 128-frame quantum. Needs Playwright (Kepler's venv has it).

  ~/.local/share/kepler/venv/bin/python tests/browser_check.py [build/fa_render]
"""
import functools
import http.server
import os
import shutil
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOOKS = ["kepler", "luna", "fluent", "fluent_metal", "kitt", "hal", "gargantua"]


def main():
    fa_render = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "fa_render")
    from playwright.sync_api import sync_playwright
    tmp = tempfile.mkdtemp()
    try:
        for f in ("fluent_audio.wasm", "fa-worklet.js", "fluent-audio.js", "looks.pack.yaml"):
            shutil.copy(os.path.join(ROOT, "wasm", "dist", f), tmp)
        shutil.copy(os.path.join(ROOT, "wasm", "test.html"), tmp)
        subprocess.run(["node", os.path.join(ROOT, "tests", "wasm_render.mjs"), "--make-input", os.path.join(tmp, "in.wav")], check=True)
        for look in LOOKS:
            subprocess.run([fa_render, os.path.join(ROOT, "presets", "looks.yaml"), look, os.path.join(tmp, "in.wav"),
                            os.path.join(tmp, f"native_{look}.wav"), "--no-trim", "--tail", "1"], check=True)
        class Quiet(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *a):
                pass
        handler = functools.partial(Quiet, directory=tmp)
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        url = f"http://127.0.0.1:{srv.server_address[1]}/test.html?looks={','.join(LOOKS)}"
        with sync_playwright() as p:
            b = p.chromium.launch(args=["--autoplay-policy=no-user-gesture-required"])
            page = b.new_page()
            page.on("console", lambda m: print("  console:", m.text) if m.type in ("error", "warning") else None)
            page.goto(url)
            page.wait_for_function("window.__results && window.__results.length >= 1 && (window.__results.some(r => r.error) || window.__results.length === %d)" % len(LOOKS), timeout=120000)
            res = page.evaluate("window.__results")
            print("chromium", b.version)
            b.close()
        srv.shutdown()
        bad = 0
        for r in res:
            if "error" in r:
                print("ERROR", r["error"]); bad += 1; continue
            print(f"{r['look']:13} {'bit-identical' if r['ok'] else str(r['diff']) + ' samples differ'}  "
                  f"{r['usPerQuantum']:.1f} µs / 128 frames (offline render, whole graph)")
            bad += 0 if r["ok"] else 1
        sys.exit(1 if bad else 0)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
