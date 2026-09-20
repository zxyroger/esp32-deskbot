#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
onegpio 按需启动器 —— 极小常驻进程, 只为"点积木时自动把 s3-extend 拉起来"。

它只做四件事:

  1. GET/POST /start   拉起 s3-extend (backplane + wsgw:9007 + esp32gw)
  2. GET/POST /stop    停掉整套 s3-extend (启动器自己继续待命, 随时能再拉)
  3. GET      /status  返回 JSON 状态 (给 Scratch 扩展和调试用)
  4. GET      /esp32s3.js   顺便把 scratch/ 目录当静态站点托管
       —— 于是 http://127.0.0.1:8000/esp32s3.js 既能给 TurboWarp 当扩展 URL,
          也是扩展自己"叫醒服务"的入口 (同一个端口, 少一个要记的地址)

来源校验 (防止任意网页偷偷启动你本机的服务):
    默认放行 —— 没有 Origin (curl/脚本)、Origin: null (file:// / 沙箱 iframe)、
    localhost / 127.0.0.1 / [::1] 任意端口, 以及常见在线编辑器
    (turbowarp.org、scratch.mit.edu、penguinmod.com、adacraft.org),
    还有非 http(s) 的本地桌面端 (file://、app://、tw-editor:// 等)。
    别的来源会收到 403, 同时把来源写进 launcher.log 方便排查。
    要放行自己的编辑器:
        python onegpio_launcher.py --allow-origin https://my.editor
        set ONEGPIO_ALLOW_ORIGINS=https://my.editor,https://other.editor

为什么需要它:
    Scratch / TurboWarp 的扩展跑在浏览器沙箱里, 只能发 WebSocket 和 HTTP,
    不能启动本机进程。所以"点积木自动拉起服务"必须有一个本机的小进程接住
    这个请求 —— 就是这个启动器。
    它只用 Python 标准库, 不 import s3-extend, 不占 9007 / 43124 / 43125,
    内存十几 MB, 所以可以常驻 (登录自启); 真正重的三件套等到点积木时才起。

用法:
    D:\\esp\\onegpio\\tools\\start_launcher.ps1              # 平时用这个 (后台隐藏)
    D:\\esp\\onegpio\\tools\\start_launcher.ps1 -Check       # 看状态
    D:\\esp\\onegpio\\tools\\start_launcher.ps1 -Stop        # 停掉启动器
    python onegpio_launcher.py --port 8000                # 前台调试

接口:
    GET  /                 -> 状态页 (浏览器里直接看)
    GET  /status           -> JSON 状态
    GET  /start?wait=45    -> 拉起服务并等它就绪 (wait 秒, 默认 45, 上限 120)
    GET  /stop             -> 停掉服务 (启动器留着)
    GET  /restart          -> 重启服务
    GET  /logs?n=80        -> 各组件日志尾部 (排错用)
    GET  /esp32s3.js       -> Scratch 扩展脚本
"""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
import re
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

VERSION = '1.0'

# ---- 来源白名单 ----
# 本机来源 (http/https 的 host 精确匹配, 或它的子域)
LOCAL_ORIGIN_HOSTS = ('localhost', '127.0.0.1', '::1')
# 常见在线编辑器: 扩展跑在这些页面的沙箱 iframe 里, fetch 会带它们的 Origin
EDITOR_ORIGIN_HOSTS = (
    'turbowarp.org',        # TurboWarp 网页版 (含 packager 等子域)
    'scratch.mit.edu',      # 官方在线编辑器
    'penguinmod.com',       # PenguinMod 网页版
    'adacraft.org',         # Adacraft
)
# 非 http(s) 的本地来源, 按前缀放行
# tw-editor: 是 TurboWarp 桌面版 (Electron 打包的编辑器) 的页面来源,
# 实际发过来的值形如 "tw-editor://." 或 "tw-editor://index.html", 所以按 scheme 前缀匹配
LOCAL_ORIGIN_PREFIXES = ('file://', 'app://', 'tw-editor:',
                         'chrome-extension://', 'moz-extension://',
                         'safari-web-extension://')
# --allow-origin / ONEGPIO_ALLOW_ORIGINS 追加进来的 host, 以及"全放行"开关
EXTRA_ORIGIN_HOSTS: list[str] = []
ALLOW_ANY_ORIGIN = [False]

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TOOLS_DIR)
SCRATCH_DIR = os.path.join(REPO_ROOT, 'scratch')
START_SCRIPT = os.path.join(TOOLS_DIR, 'start_s3extend.ps1')
STOP_SCRIPT = os.path.join(TOOLS_DIR, 'stop_s3extend.ps1')
LOG_DIR = os.path.join(os.environ.get('LOCALAPPDATA') or os.path.expanduser('~'),
                       's3extend', 'logs')

# 9007 = wsgw (Scratch 扩展连的就是它), 43124/43125 = backplane
WS_PORT = 9007
BACKPLANE_PORTS = (43124, 43125)

CREATE_NO_WINDOW = 0x08000000 if os.name == 'nt' else 0

START_TIME = time.time()

_start_lock = threading.Lock()
_state = {
    'in_progress': False,
    'last_start_at': None,
    'last_elapsed_ms': None,
    'last_result': None,
    'last_error': None,
    'start_count': 0,
}


# --------------------------------------------------------------------------- #
# 小工具
# --------------------------------------------------------------------------- #

def log(message: str) -> None:
    line = '%s  %s' % (time.strftime('%Y-%m-%d %H:%M:%S'), message)
    print(line, flush=True)


def port_open(port: int, host: str = '127.0.0.1', timeout: float = 0.25) -> bool:
    """端口上有人监听就返回 True (只做本机探测, 不发业务数据)。"""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        return sock.connect_ex((host, port)) == 0
    except OSError:
        return False
    finally:
        try:
            sock.close()
        except OSError:
            pass


def read_text(path: str, limit: int | None = None) -> str:
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as handle:
            text = handle.read()
    except OSError:
        return ''
    if limit is not None and len(text) > limit:
        return text[-limit:]
    return text


def tail(path: str, lines: int = 80) -> str:
    text = read_text(path)
    if not text:
        return ''
    return '\n'.join(text.splitlines()[-lines:])


def service_running() -> bool:
    return port_open(WS_PORT)


def origin_host(origin: str) -> str:
    """从 Origin 头里抠出 host (小写, 不带端口); 不是 http(s) 就返回空串。"""
    try:
        parsed = urlparse(origin)
    except ValueError:
        return ''
    if parsed.scheme not in ('http', 'https'):
        return ''
    return (parsed.hostname or '').lower()


def normalize_allowed_origin(value: str) -> str:
    """接受 https://host、host、host:port 三种写法, 统一成 host。"""
    text = (value or '').strip().lower()
    if not text:
        return ''
    if '://' in text:
        return origin_host(text)
    return text.split('/')[0].rsplit(':', 1)[0] if not text.startswith('[') else text


def allowed_origin_hosts() -> tuple[str, ...]:
    return tuple(LOCAL_ORIGIN_HOSTS) + tuple(EDITOR_ORIGIN_HOSTS) + tuple(EXTRA_ORIGIN_HOSTS)


def run_powershell(script: str, args: list[str], timeout: float = 120.0) -> tuple[int, str]:
    """同步跑一个 .ps1, 输出按 UTF-8 解码 (脚本里先设了 OutputEncoding)。"""
    if not os.path.isfile(script):
        return 127, '找不到脚本: %s' % script

    # -Switch 这类开关必须原样传, 加引号会被 PowerShell 当成位置参数
    rendered = []
    for arg in args:
        if re.match(r'^-[A-Za-z][\w-]*$', arg):
            rendered.append(arg)
        else:
            rendered.append("'%s'" % arg.replace("'", "''"))
    quoted = ' '.join(rendered)
    command = ("[Console]::OutputEncoding=[System.Text.Encoding]::UTF8; "
               "& '%s' %s" % (script.replace("'", "''"), quoted))
    cmd = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
           '-WindowStyle', 'Hidden', '-Command', command]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=timeout,
                              creationflags=CREATE_NO_WINDOW)
    except subprocess.TimeoutExpired:
        return 124, '脚本超时 (%.0f 秒): %s' % (timeout, script)
    except OSError as exc:
        return 126, '启动 PowerShell 失败: %s' % exc

    out = (proc.stdout or b'').decode('utf-8', 'replace')
    err = (proc.stderr or b'').decode('utf-8', 'replace')
    return proc.returncode, (out + err).strip()


def wait_for_service(seconds: float) -> bool:
    deadline = time.time() + max(0.0, seconds)
    while True:
        if service_running():
            return True
        if time.time() >= deadline:
            return False
        time.sleep(0.3)


# --------------------------------------------------------------------------- #
# 业务动作
# --------------------------------------------------------------------------- #

def start_service(wait_seconds: float = 45.0) -> dict:
    began = time.time()
    if service_running():
        return {'ok': True, 'already_running': True, 'elapsed_ms': 0,
                'message': 's3-extend 已经在运行 (9007 已监听)'}

    with _start_lock:
        if service_running():      # 并发请求里前面的那个刚把它拉起来了
            return {'ok': True, 'already_running': True,
                    'elapsed_ms': int((time.time() - began) * 1000),
                    'message': '另一个请求刚把 s3-extend 拉起来了'}

        _state['in_progress'] = True
        _state['last_start_at'] = began
        _state['start_count'] += 1
        log('start: 调 %s -Background' % os.path.basename(START_SCRIPT))

        try:
            code, output = run_powershell(START_SCRIPT, ['-Background'],
                                          timeout=max(60.0, wait_seconds + 45.0))
        finally:
            _state['in_progress'] = False

        # start_s3extend.ps1 -Background 自己会等到 9007 监听 (最多 40 秒),
        # 这里再兜一层, 防止守护进程起得慢一点点。
        # 脚本本身报错 (比如参数写错) 就别等了, 直接把报错抛给调用方。
        if not service_running() and code == 0:
            wait_for_service(wait_seconds)
        ready = service_running()
        elapsed_ms = int((time.time() - began) * 1000)

        _state['last_elapsed_ms'] = elapsed_ms
        _state['last_result'] = 'ok' if ready else 'fail'
        _state['last_error'] = None if ready else (output[-500:] or '未知错误')

        if ready:
            log('start: 完成, %d ms' % elapsed_ms)
            return {'ok': True, 'started': True, 'elapsed_ms': elapsed_ms,
                    'message': '本地服务已拉起 (wsgw 已监听 9007)'}

        log('start: 失败 (退出码 %s)' % code)
        return {
            'ok': False,
            'started': False,
            'elapsed_ms': elapsed_ms,
            'exit_code': code,
            'message': '服务没能起来, 看 /logs 或 %s' % os.path.join(LOG_DIR, 'supervisor.log'),
            'detail': output[-500:],
        }


def stop_service() -> dict:
    code, output = run_powershell(STOP_SCRIPT, [], timeout=90.0)
    deadline = time.time() + 10.0
    while service_running() and time.time() < deadline:
        time.sleep(0.3)
    running = service_running()
    log('stop: 退出码 %s, 9007 %s' % (code, '仍在监听' if running else '已释放'))
    return {'ok': not running, 'still_running': running, 'exit_code': code,
            'detail': output[-500:]}


def status_payload() -> dict:
    board_ip = read_text(os.path.join(LOG_DIR, 'last_board_ip.txt')).strip()
    ws_running = service_running()
    return {
        'ok': True,
        'launcher': {
            'version': VERSION,
            'pid': os.getpid(),
            'port': HANDLER_PORT[0],
            'uptime_s': round(time.time() - START_TIME, 1),
        },
        'service': {
            'running': ws_running,
            'ws_port_9007': ws_running,
            'backplane_43124': port_open(BACKPLANE_PORTS[0]),
            'backplane_43125': port_open(BACKPLANE_PORTS[1]),
            'board_ip': board_ip,
        },
        'start': {
            'in_progress': _state['in_progress'],
            'last_start_at': _state['last_start_at'],
            'last_elapsed_ms': _state['last_elapsed_ms'],
            'last_result': _state['last_result'],
            'last_error': _state['last_error'],
            'start_count': _state['start_count'],
        },
        'extension_url': 'http://127.0.0.1:%d/esp32s3.js' % HANDLER_PORT[0],
        'log_dir': LOG_DIR,
        'allowed_origins': {
            'anything': ALLOW_ANY_ORIGIN[0],
            'hosts': list(allowed_origin_hosts()),
            'prefixes': list(LOCAL_ORIGIN_PREFIXES),
            'extra': list(EXTRA_ORIGIN_HOSTS),
        },
    }


# 端口要在 handler 里回显给调用方, 用个单元素列表当"全局变量"
HANDLER_PORT = [8000]


INDEX_HTML = """<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>onegpio 启动器</title>
<style>
 body {{ font-family: "Microsoft YaHei", system-ui, sans-serif; margin: 2rem; color: #123; }}
 h1 {{ font-size: 1.2rem; }}
 table {{ border-collapse: collapse; margin: 1rem 0; }}
 td {{ padding: 4px 14px 4px 0; }}
 .ok {{ color: #0a7d2c; }} .bad {{ color: #c0392b; }}
 code {{ background: #f2f5f8; padding: 2px 6px; border-radius: 4px; }}
 button {{ padding: 6px 14px; margin-right: 8px; cursor: pointer; }}
</style>
</head>
<body>
<h1>onegpio 按需启动器 (端口 {port})</h1>
<p id="summary">读取状态…</p>
<table id="detail"></table>
<p>
  <button onclick="act('start')">启动服务</button>
  <button onclick="act('stop')">停止服务</button>
</p>
<p>扩展地址: <code>http://127.0.0.1:{port}/esp32s3.js</code></p>
<p>日志: <code>{log_dir}</code></p>
<script>
async function refresh() {{
  const r = await fetch('status');
  const s = await r.json();
  const up = s.service.running;
  document.getElementById('summary').innerHTML = up
    ? '<span class="ok">s3-extend 正在运行</span>（点积木直接就能用）'
    : '<span class="bad">s3-extend 未运行</span>（点积木时会自动拉起）';
  document.getElementById('detail').innerHTML = [
    ['wsgw (9007)', s.service.ws_port_9007],
    ['backplane (43124)', s.service.backplane_43124],
    ['backplane (43125)', s.service.backplane_43125],
    ['板子 IP (上次发现)', s.service.board_ip || '(未知)'],
    ['启动次数', s.start.start_count],
    ['上次启动耗时', s.start.last_elapsed_ms === null ? '-' : s.start.last_elapsed_ms + ' ms'],
    ['启动器 PID', s.launcher.pid]
  ].map(([k, v]) => `<tr><td>${{k}}</td><td>${{v}}</td></tr>`).join('');
}}
async function act(what) {{
  document.getElementById('summary').textContent = what === 'start' ? '正在启动…' : '正在停止…';
  await fetch(what, {{ method: 'POST' }});
  refresh();
}}
refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
"""


# --------------------------------------------------------------------------- #
# HTTP
# --------------------------------------------------------------------------- #

class Handler(BaseHTTPRequestHandler):
    server_version = 'onegpio-launcher/%s' % VERSION
    protocol_version = 'HTTP/1.1'

    # ---- 基础输出 ----

    def _cors(self) -> None:
        # 扩展可能从 file:// / 沙箱 iframe (Origin: null) / 任意本机页面调用,
        # 一律放行; 服务只绑 127.0.0.1, 外网碰不到。
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', '*')
        self.send_header('Access-Control-Max-Age', '600')
        # Chromium 的 Private Network Access 预检
        self.send_header('Access-Control-Allow-Private-Network', 'true')

    def _send(self, status: int, body: bytes, content_type: str,
              extra: dict | None = None) -> None:
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        if status != 204:                       # 204 不能带 Content-Length
            self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self._cors()
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def _json(self, payload: dict, status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode('utf-8')
        self._send(status, body, 'application/json; charset=utf-8')

    def _text(self, status: int, text: str, content_type: str = 'text/plain; charset=utf-8') -> None:
        self._send(status, text.encode('utf-8'), content_type)

    def log_message(self, fmt: str, *args) -> None:      # 默认会往 stderr 打, 这里收进 launcher.log
        log('http: %s' % (fmt % args))

    # ---- 路由 ----

    def do_OPTIONS(self) -> None:                        # noqa: N802
        self.send_response(204)
        self.send_header('Content-Length', '0')
        self._cors()
        self.end_headers()

    def do_HEAD(self) -> None:                           # noqa: N802
        self.do_GET()

    def do_GET(self) -> None:                            # noqa: N802
        self._handle()

    def do_POST(self) -> None:                           # noqa: N802
        length = int(self.headers.get('Content-Length') or 0)
        if length:                                       # 请求体用不上, 但得读干净
            try:
                self.rfile.read(length)
            except OSError:
                pass
        self._handle()

    def _handle(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path.rstrip('/') or '/'
        query = parse_qs(parsed.query)

        if not self._origin_allowed():
            origin = (self.headers.get('Origin') or '').strip()
            log('拒绝来源: Origin=%s %s %s' % (origin or '(空)', self.command, self.path))
            self._json({
                'ok': False,
                'error': '拒绝未放行的来源: %s' % (origin or '(空)'),
                'hint': ('这是你的编辑器的话, 用 --allow-origin <来源> 启动启动器, '
                         '或设环境变量 ONEGPIO_ALLOW_ORIGINS=<来源>, 再重试'),
            }, 403)
            return

        try:
            if path in ('/', '/index.html'):
                html = INDEX_HTML.format(port=HANDLER_PORT[0], log_dir=LOG_DIR)
                self._text(200, html, 'text/html; charset=utf-8')
            elif path == '/status':
                self._json(status_payload())
            elif path == '/start':
                self._json(start_service(wait_seconds=_wait_param(query)))
            elif path == '/stop':
                self._json(stop_service())
            elif path == '/restart':
                stop_service()
                self._json(start_service(wait_seconds=_wait_param(query)))
            elif path == '/logs':
                count = int((query.get('n') or ['80'])[0])
                self._json({
                    'launcher': tail(os.path.join(LOG_DIR, 'launcher.log'), count),
                    'supervisor': tail(os.path.join(LOG_DIR, 'supervisor.log'), count),
                    'esp32gw': tail(os.path.join(LOG_DIR, 'esp32gw.log'), count),
                    'esp32gw_err': tail(os.path.join(LOG_DIR, 'esp32gw.err.log'), count),
                })
            elif path == '/favicon.ico':
                self._send(204, b'', 'image/x-icon')
            else:
                self._serve_static(unquote(path))
        except Exception as exc:                         # 任何异常都别把启动器带走
            log('error: %r' % exc)
            self._json({'ok': False, 'error': str(exc)}, 500)

    def _origin_allowed(self) -> bool:
        """放行: 无 Origin (curl/脚本)、null、本机来源、常见在线编辑器、--allow-origin 指定的。"""
        if ALLOW_ANY_ORIGIN[0]:
            return True
        origin = (self.headers.get('Origin') or '').strip().lower()
        if not origin or origin == 'null':
            return True
        if any(origin.startswith(prefix) for prefix in LOCAL_ORIGIN_PREFIXES):
            return True
        host = origin_host(origin)
        if not host:
            return False
        # 精确匹配 host, 或它的子域 (turbowarp.org 也放行 www.turbowarp.org),
        # 用 '.' + host 判断, 避免 turbowarp.org.evil.com 这种前缀骗过校验
        return any(host == allowed or host.endswith('.' + allowed)
                   for allowed in allowed_origin_hosts())

    def _serve_static(self, path: str) -> None:
        name = os.path.basename(path)
        target = os.path.join(SCRATCH_DIR, name)
        if not name or not os.path.isfile(target):
            self._json({'ok': False, 'error': '没有这个文件: %s' % path,
                        'hint': '可以访问 / 或 /status'}, 404)
            return
        guessed = mimetypes.guess_type(name)[0] or 'application/octet-stream'
        if guessed.startswith('text/') or guessed.endswith(('javascript', 'json')):
            guessed += '; charset=utf-8'
        with open(target, 'rb') as handle:
            body = handle.read()
        self._send(200, body, guessed)


def _wait_param(query: dict) -> float:
    try:
        seconds = float((query.get('wait') or ['45'])[0])
    except (TypeError, ValueError):
        seconds = 45.0
    return max(0.0, min(seconds, 120.0))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description='onegpio 按需启动器')
    parser.add_argument('--port', type=int, default=8000, help='监听端口 (默认 8000)')
    parser.add_argument('--host', default='127.0.0.1', help='监听地址 (默认只监听本机)')
    parser.add_argument('--allow-origin', action='append', default=[], metavar='ORIGIN',
                        help='额外放行的来源 (可重复), 例如 --allow-origin https://my.editor')
    parser.add_argument('--allow-any-origin', action='store_true',
                        help='放行任意来源 (只建议排错时临时用)')
    parser.add_argument('--check', action='store_true', help='只打印状态后退出')
    args = parser.parse_args(argv)

    HANDLER_PORT[0] = args.port

    extra = list(args.allow_origin)
    extra += re.split(r'[;,\s]+', os.environ.get('ONEGPIO_ALLOW_ORIGINS', ''))
    for item in extra:
        host = normalize_allowed_origin(item)
        if host and host not in EXTRA_ORIGIN_HOSTS:
            EXTRA_ORIGIN_HOSTS.append(host)
    ALLOW_ANY_ORIGIN[0] = bool(args.allow_any_origin)
    if EXTRA_ORIGIN_HOSTS:
        log('额外放行的来源: %s' % ', '.join(EXTRA_ORIGIN_HOSTS))
    if ALLOW_ANY_ORIGIN[0]:
        log('警告: --allow-any-origin 已开启, 任意网页都能调用本启动器')

    if args.check:
        print(json.dumps(status_payload(), ensure_ascii=False, indent=2))
        return 0

    try:
        httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    except OSError as exc:
        log('端口 %d 起不来: %s' % (args.port, exc))
        return 1

    httpd.daemon_threads = True
    log('启动器已就绪: http://%s:%d/  (扩展地址 /esp32s3.js, 服务当前%s)'
        % (args.host, args.port, '运行中' if service_running() else '未运行'))
    try:
        httpd.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        log('收到 Ctrl+C, 退出')
    finally:
        httpd.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
