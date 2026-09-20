/**
 * esp32s3.js "点积木 -> 自动拉起本地服务" 的冒烟测试
 * (不需要 Scratch, 也不需要真的连板子; 全程用假 fetch / 假 WebSocket)
 *
 * 用法:
 *     node tools\test_esp32s3_autostart.js
 *
 * 覆盖的场景:
 *   1. 服务没在跑: 点积木 -> 0.7 秒后 POST http://127.0.0.1:8000/start
 *   2. 服务起来后: 自动重连 9007, 并把排队等着的积木指令补发出去
 *   3. 启动器不在 (fetch 失败): 状态提示"先跑一次 start_launcher.ps1"
 *   4. 服务本来就在跑 (WebSocket 连得快): 根本不去打扰启动器
 *   5. 连点积木: 冷却时间内只请求一次
 *   6. 8000 被占: 自动换 8001
 */
'use strict';

const fs = require('fs');
const path = require('path');

const code = fs.readFileSync(path.join(__dirname, '..', 'scratch', 'esp32s3.js'), 'utf8');

let failures = 0;
function check(name, actual, expected) {
    const ok = actual === expected;
    if (!ok) { failures++; }
    console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}`);
    if (!ok) {
        console.log(`        实际: ${actual}`);
        console.log(`        期望: ${expected}`);
    }
}

function wait(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

// 造一个干净的运行环境: 假的 Scratch / WebSocket / fetch
function makeEnv(fetchImpl) {
    const env = { extension: null, sockets: [] };

    const Scratch = {
        extensions: { register: (ext) => { env.extension = ext; } },
        BlockType: {
            COMMAND: 'command',
            REPORTER: 'reporter',
            BOOLEAN: 'Boolean',
            HAT: 'hat'
        },
        ArgumentType: { STRING: 'string', NUMBER: 'number', BOOLEAN: 'Boolean' }
    };

    class FakeWebSocket {
        constructor(url) {
            this.url = url;
            this.readyState = 0;          // CONNECTING
            this.sent = [];
            env.sockets.push(this);
        }
        send(text) { this.sent.push(JSON.parse(text)); }
        open() {
            this.readyState = 1;
            if (this.onopen) { this.onopen(); }
        }
        message(obj) {
            if (this.onmessage) { this.onmessage({ data: JSON.stringify(obj) }); }
        }
        close() {
            this.readyState = 3;
            if (this.onclose) { this.onclose(); }
        }
    }

    new Function('Scratch', 'WebSocket', 'fetch', code)(Scratch, FakeWebSocket, fetchImpl);
    env.lastSocket = () => env.sockets[env.sockets.length - 1];
    return env;
}

// 启动器正常应答时的假 fetch
function okFetch(calls, okPorts) {
    return (url, options) => {
        calls.push({ url, method: (options || {}).method });
        const port = parseInt(url.split(':')[2].split('/')[0], 10);
        if (okPorts && okPorts.indexOf(port) === -1) {
            return Promise.reject(new TypeError('Failed to fetch'));
        }
        return Promise.resolve({
            ok: true,
            status: 200,
            json: () => Promise.resolve({ ok: true, message: '本地服务已拉起' })
        });
    };
}

(async function main() {
    // ---- 0) 新积木在不在 ----
    const env0 = makeEnv(() => new Promise(() => {}));
    const opcodes = env0.extension.getInfo().blocks
        .filter((b) => typeof b === 'object')
        .map((b) => b.opcode);
    check('有「启动本地服务」积木', opcodes.includes('startService'), true);
    check('有「本地服务状态」积木', opcodes.includes('launcherStatus'), true);
    check('原来的 9 块积木都在', opcodes.length >= 11, true);

    // ---- 1) 服务没在跑: 点积木 -> 自动拉起 -> 重连 -> 补发指令 ----
    const calls1 = [];
    const env1 = makeEnv(okFetch(calls1));
    const ext1 = env1.extension;
    ext1.digitalWrite({ PIN: 2, VALUE: 1 });
    check('点积木的瞬间不请求 (先给 WebSocket 几百毫秒)', calls1.length, 0);
    check('点积木后先排队, 不直接发出去', env1.lastSocket().sent.length, 0);

    await wait(900);
    check('0.7 秒后请启动器拉起服务', calls1.length, 1);
    check('请求落在启动器 /start 上', calls1[0].url, 'http://127.0.0.1:8000/start');
    check('用 POST 请求', calls1[0].method, 'POST');
    check('拉起过程中状态说明正在启动', ext1.boardStatus(), '正在启动本地服务…（点积木触发的自动拉起，约 10~20 秒）');
    check('启动器应答时老的 WebSocket 还在连, 不重复建', env1.sockets.length, 1);
    check('本地服务状态积木显示在启动中',
        ext1.launcherStatus().indexOf('正在启动本地服务…') === 0, true);

    env1.lastSocket().close();         // 老连接连的是还没起来的 9007, 先失败
    await wait(1800);                  // 等轮询 (1.5 秒) 自动重建
    check('轮询自动重建 WebSocket', env1.sockets.length, 2);

    env1.lastSocket().open();          // 服务起来了, 9007 连上
    check('连上本地服务后状态', ext1.boardStatus(), '已连上本地服务，等点「连接板子 IP」');
    check('本地服务状态积木显示已连接',
        ext1.launcherStatus(), '本地服务已连接（ws://127.0.0.1:9007）');
    check('排队等着的积木指令补发出去',
        JSON.stringify(env1.lastSocket().sent.map((m) => m.command || m.id)),
        JSON.stringify(['to_esp32_gateway', 'set_mode_digital_output', 'digital_write']));

    // ---- 2) 连点积木: 冷却时间内不重复请求 ----
    const calls2 = [];
    const env2 = makeEnv(okFetch(calls2));
    const ext2 = env2.extension;
    ext2.digitalWrite({ PIN: 2, VALUE: 1 });
    ext2.pwmWrite({ PIN: 5, VALUE: 50 });
    ext2.servoWrite({ PIN: 4, ANGLE: 90 });
    await wait(900);
    check('连点 3 块积木只请求一次', calls2.length, 1);
    ext2.digitalWrite({ PIN: 2, VALUE: 0 });   // 冷却期内再点
    await wait(900);
    check('冷却期内的点击不再重复请求', calls2.length, 1);

    // ---- 3) 启动器不在 (两个端口都没人应答) ----
    const calls3 = [];
    const env3 = makeEnv(okFetch(calls3, []));
    const ext3 = env3.extension;
    ext3.digitalWrite({ PIN: 2, VALUE: 1 });
    await wait(900);
    check('8000 不通时会去试 8001', calls3.length, 2);
    check('第二个候选端口是 8001', calls3[1].url, 'http://127.0.0.1:8001/start');
    check('启动器不在时的状态提示',
        ext3.boardStatus(), '本地服务未启动，启动器也没在运行（先跑一次 tools\\start_launcher.ps1）');
    check('启动器不在时的「本地服务状态」',
        ext3.launcherStatus(), '启动器未运行（先跑一次 tools\\start_launcher.ps1）');

    // ---- 4) 服务本来就在跑: 完全不打扰启动器 ----
    const calls4 = [];
    const env4 = makeEnv(okFetch(calls4));
    const ext4 = env4.extension;
    ext4.analogRead({ PIN: 32 });
    env4.lastSocket().open();          // WebSocket 立刻就通了
    await wait(900);
    check('服务在跑时不请求启动器', calls4.length, 0);
    check('服务在跑时状态是"已连上本地服务"', ext4.boardStatus(), '已连上本地服务，等点「连接板子 IP」');

    // ---- 5) 「连接板子 IP」积木: 服务没在跑时也会拉起, 起来后接着连板子 ----
    const calls5 = [];
    const env5 = makeEnv(okFetch(calls5));
    const ext5 = env5.extension;
    ext5.connect({ IP: '192.168.0.104' });
    check('点 IP 积木立刻显示"正在连接"', ext5.boardStatus(), '正在连接 192.168.0.104 …');
    await wait(900);
    check('IP 积木也会请启动器拉起服务', calls5.length, 1);
    env5.lastSocket().open();
    check('服务起来后接着连板子', ext5.boardStatus(), '正在连接 192.168.0.104 …');
    check('第一条报文是 id, 第二条是 ip_address',
        JSON.stringify(env5.lastSocket().sent.map((m) => m.command || m.id)),
        JSON.stringify(['to_esp32_gateway', 'ip_address']));

    // ---- 6) 8000 被占: 自动换 8001 ----
    const calls6 = [];
    const env6 = makeEnv(okFetch(calls6, [8001]));
    env6.extension.digitalWrite({ PIN: 2, VALUE: 1 });
    await wait(900);
    check('8000 失败后换 8001 成功', calls6.length, 2);
    check('记住能用的是 8001', env6.extension.launcherPort, 8001);

    // ---- 7) 启动器在线但拒绝了这个来源 (403): 状态要说清是"被拒绝", 不是"没运行" ----
    const calls7 = [];
    const rejectFetch = (url, options) => {
        calls7.push({ url, method: (options || {}).method });
        return Promise.resolve({
            ok: false,
            status: 403,
            text: () => Promise.resolve(JSON.stringify({
                ok: false,
                error: '拒绝未放行的来源: https://turbowarp.org'
            }))
        });
    };
    const env7 = makeEnv(rejectFetch);
    const ext7 = env7.extension;
    ext7.digitalWrite({ PIN: 2, VALUE: 1 });
    await wait(900);
    check('两个端口都试过 (403 不算"没运行")', calls7.length, 2);
    check('状态里带出 HTTP 403',
        ext7.boardStatus().indexOf('HTTP 403') >= 0, true);
    check('状态里带出启动器的原话',
        ext7.boardStatus().indexOf('拒绝未放行的来源') >= 0, true);
    check('「本地服务状态」也带出 403',
        ext7.launcherStatus().indexOf('HTTP 403') >= 0, true);

    console.log(failures === 0 ? '\n全部通过' : `\n${failures} 项失败`);
    process.exit(failures === 0 ? 0 : 1);
})().catch((err) => {
    console.error('测试自己炸了:', err);
    process.exit(2);
});
