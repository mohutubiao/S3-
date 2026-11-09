#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GameBox 自建游戏商店（局域网）

用法：
    python tools/store_server.py --dir D:\\roms
    python tools/store_server.py --dir D:\\roms --port 8000

然后在 src/service_config.local.h 里写：
    #define DIJI_SERVICE_ORIGIN "http://<这台电脑的IP>:8000"

掌机进「游戏商店」就能看到 --dir 目录下所有 .nes 文件，按 A 下载到本地。

接口（与固件 src/game_store.cpp 对齐）：
    GET /api/v1/games?supported=true&page=1&page_size=25
        -> {"total": N, "items": [{id,title,filename,download_url,cover_url,sha256,size,supported}]}
    GET /files/<filename>           -> ROM 二进制
    GET /api/v1/devices/register    -> {"ok": true}（认证占位，不做校验）
    GET /                           -> 浏览器可看的调试列表
"""

import argparse
import hashlib
import json
import os
import socket
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROM_DIR = "."
CACHE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".store_sha256.json")
_sha_cache = {}


def load_cache():
    global _sha_cache
    try:
        with open(CACHE_PATH, "r", encoding="utf-8") as fp:
            _sha_cache = json.load(fp)
    except Exception:
        _sha_cache = {}


def save_cache():
    try:
        with open(CACHE_PATH, "w", encoding="utf-8") as fp:
            json.dump(_sha_cache, fp)
    except Exception:
        pass


def sha256_of(path, size, mtime):
    key = f"{path}|{size}|{int(mtime)}"
    if key in _sha_cache:
        return _sha_cache[key]
    digest = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 20), b""):
            digest.update(chunk)
    value = digest.hexdigest()
    _sha_cache[key] = value
    save_cache()
    return value


def scan_roms():
    """返回 [{filename, path, size, title}]，按文件名排序；同名文件保留第一个。"""
    items = []
    seen = set()
    for root, _dirs, files in os.walk(ROM_DIR):
        for name in sorted(files):
            if not name.lower().endswith(".nes"):
                continue
            if name in seen:
                continue
            seen.add(name)
            path = os.path.join(root, name)
            try:
                size = os.path.getsize(path)
            except OSError:
                continue
            title = os.path.splitext(name)[0]
            items.append({"filename": name, "path": path, "size": size, "title": title})
    items.sort(key=lambda x: x["filename"].lower())
    return items


def lan_ip():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        sock.close()


class Handler(BaseHTTPRequestHandler):
    server_version = "GameBoxStore/1.0"

    def log_message(self, fmt, *args):
        sys.stdout.write("[商店] %s - %s\n" % (self.address_string(), fmt % args))

    def _json(self, payload, code=200):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _host(self):
        return self.headers.get("Host") or f"{lan_ip()}:{self.server.server_port}"

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path in ("/api/v1/games", "/games"):
            self.handle_games(urllib.parse.parse_qs(parsed.query))
            return
        if path.startswith("/files/"):
            self.handle_file(urllib.parse.unquote(path[len("/files/"):]))
            return
        if path.startswith("/api/v1/devices/") or path in (
            "/api/v1/device/config",
            "/api/v1/usage/events",
            "/api/v1/ota/check",
        ):
            # 固件会带这些请求；自建环境一律回 ok，省得它反复超时重试
            self._json({"ok": True, "total": 0, "items": [], "version": "1.0.0"})
            return
        if path == "/":
            self.handle_index()
            return
        self._json({"ok": False, "message": "not found"}, 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        self._json({"ok": True})

    def handle_games(self, query):
        try:
            page = max(1, int(query.get("page", ["1"])[0]))
        except ValueError:
            page = 1
        try:
            page_size = max(1, min(100, int(query.get("page_size", ["25"])[0])))
        except ValueError:
            page_size = 25

        all_items = scan_roms()
        total = len(all_items)
        start = (page - 1) * page_size
        window = all_items[start:start + page_size]
        host = self._host()

        payload = {"total": total, "items": []}
        for item in window:
            payload["items"].append({
                "id": item["filename"],
                "title": item["title"],
                "filename": item["filename"],
                "download_url": f"http://{host}/files/{urllib.parse.quote(item['filename'])}",
                "cover_url": "",
                "sha256": sha256_of(item["path"], item["size"], os.path.getmtime(item["path"])),
                "size": item["size"],
                "supported": True,
            })
        self._json(payload)

    def handle_file(self, name):
        target = None
        for item in scan_roms():
            if item["filename"] == name:
                target = item
                break
        if not target:
            self._json({"ok": False, "message": "file not found"}, 404)
            return
        size = target["size"]
        with open(target["path"], "rb") as fp:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            for chunk in iter(lambda: fp.read(1 << 16), b""):
                self.wfile.write(chunk)

    def handle_index(self):
        items = scan_roms()
        rows = "".join(
            f"<li>{i['title']} &nbsp;<span style='color:#888'>{i['size'] // 1024} KB</span></li>"
            for i in items
        )
        html = f"""<!doctype html><meta charset="utf-8"><title>GameBox 商店</title>
<style>body{{background:#07111f;color:#dcecff;font-family:system-ui;padding:24px}}
h1{{color:#48d7ff}}li{{margin:6px 0}}code{{color:#62efb1}}</style>
<h1>GameBox 本地商店</h1>
<p>目录 <code>{os.path.abspath(ROM_DIR)}</code> 共 <b>{len(items)}</b> 个 ROM</p>
<p>掌机固件里填：<code>http://{lan_ip()}:{self.server.server_port}</code></p>
<ul>{rows or '<li>目录里还没有 .nes 文件</li>'}</ul>"""
        body = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    global ROM_DIR
    parser = argparse.ArgumentParser(description="GameBox 局域网游戏商店")
    parser.add_argument("--dir", required=True, help="放 .nes 文件的目录（会递归扫描）")
    parser.add_argument("--port", type=int, default=8000, help="监听端口，默认 8000")
    args = parser.parse_args()

    ROM_DIR = os.path.abspath(args.dir)
    if not os.path.isdir(ROM_DIR):
        print(f"目录不存在: {ROM_DIR}")
        return 1

    load_cache()
    count = len(scan_roms())
    ip = lan_ip()
    print("=" * 56)
    print(f" 目录      : {ROM_DIR}")
    print(f" ROM 数量  : {count}")
    print(f" 商店地址  : http://{ip}:{args.port}")
    print(f" 列表接口  : http://{ip}:{args.port}/api/v1/games?supported=true&page=1&page_size=25")
    print("=" * 56)
    print("固件里（src/service_config.local.h）：")
    print(f'  #define DIJI_SERVICE_ORIGIN "http://{ip}:{args.port}"')
    print("按 Ctrl+C 停止\n")

    httpd = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")
    return 0


if __name__ == "__main__":
    sys.exit(main())
