/**
 * 真机联调: 验证 "点积木 -> 自动拉起本机服务" 这条链路 (真 fetch / 真 WebSocket)
 *
 * 用法:
 *     node tools\test_autostart_live.js            # 服务没在跑才测冷启动
 *     node tools\test_autostart_live.js --restart  # 先停掉服务, 再测冷启动
 *
 * 它做的和 Scratch 里点积木完全一样:
 *   扩展 ──POST /start──► 启动器(8000) ──► start_s3extend.ps1 -Background
 *        ◄── WebSocket ws://127.0.0.1:9007 ── 连上就算成功
 *
 * 注意: --restart 会先把正在跑的 s3-extend 停掉 (这个脚本自己会再拉起来)。
 */
'use strict';

const fs = require('fs');
const path = require('path');

const LAUNCHER = 'http://127.0.0.1:8000';
const restart = process.argv.indexOf('--restart') !== -1;

const extFile = path.join(__dirname, '..', 'scratch', 'esp32s3.js');
const code = fs.readFileSync(extFile, 'utf8');

let extension = null;
const Scratch = {
    extensions: { register: (ext) => { extension = ext; } },
    BlockType: { COMMAND: 'command', REPORTER: 'reporter', BOOLEAN: 'Boolean', HAT: 'hat' },
    ArgumentType: { STRING: 'string', NUMBER: 'number', BOOLEAN: 'Boolean' }
};

// 真的用编辑器里那套 WebSocket / fetch 跑扩展
new Function('Scratch', 'WebSocket', 'fetch', code)(Scratch, WebSocket, fetch);

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function launcherStatus() {
    const res = await fetch(LAUNCHER + '/status', { cache: 'no-store' });
    return res.json();
}

async function main() {
    console.log('扩展脚本: ' + extFile);

    // ---- 启动器在不在 ----
    let status;
    try {
        status = await launcherStatus();
    } catch (err) {
        console.error('✗ 启动器没在运行 (http://127.0.0.1:8000/status 不通)');
        console.error('  先跑一次: D:\\esp\\onegpio\\tools\\start_launcher.ps1');
        process.exit(1);
    }
    console.log(`✓ 启动器在线: PID ${status.launcher.pid}, 端口 ${status.launcher.port}`);

    if (restart && status.service.running) {
        console.log('--restart: 先停掉正在跑的 s3-extend ...');
        await fetch(LAUNCHER + '/stop', { method: 'POST' });
        await sleep(1500);
        status = await launcherStatus();
    }

    const cold = !status.service.running;
    console.log(cold ? '场景: 服务没在跑 -> 点积木应当自动拉起'
                     : '场景: 服务已经在跑 -> 点积木不该打扰启动器');

    const boardIp = (status.service.board_ip || '').trim();
    const started = Date.now();

    if (cold) {
        // 点一块真的会用到服务的积木 (有记下板子 IP 就用"连接板子 IP")
        if (boardIp) {
            console.log(`点「连接板子 IP ${boardIp}」...`);
            extension.connect({ IP: boardIp });
        } else {
            console.log('点「启动本地服务」...');
            extension.startService();
        }
        console.log('  状态: ' + extension.boardStatus());

        // 每分钟的冷启动大约 10~20 秒, 这里给 60 秒
        let ok = false;
        while (Date.now() - started < 60000) {
            await sleep(1000);
            const now = await launcherStatus();
            if (now.service.running) {
                ok = true;
                console.log(`  ${((Date.now() - started) / 1000).toFixed(1)}s: 9007 已监听, 状态: ${extension.boardStatus()}`);
                break;
            }
        }
        if (!ok) {
            console.error('✗ 60 秒内服务没起来, 看 %LOCALAPPDATA%\\s3extend\\logs\\supervisor.log');
            process.exit(1);
        }
        console.log(`✓ 服务被积木自动拉起 (${((Date.now() - started) / 1000).toFixed(1)}s)`);
    } else {
        extension.connect({ IP: boardIp || '127.0.0.1' });
        await sleep(3000);
    }

    // ---- 等状态定下来: 要么连上板子, 要么明确连不上 ----
    // ("正在连接 …" 只是点完 IP 积木的瞬时状态, 不算结果, 所以不在这里退出)
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
        const text = extension.boardStatus();
        if (text.indexOf('已连接板子') === 0 || text.indexOf('网关已运行') === 0 ||
            text.indexOf('板子连接已断开') === 0 || text.indexOf('连接失败') === 0) {
            break;
        }
        await sleep(500);
    }
    console.log('扩展「连接状态」: ' + extension.boardStatus());
    console.log('扩展「本地服务状态」: ' + extension.launcherStatus());

    if (extension.boardStatus().indexOf('已连接板子') === 0) {
        console.log('✓ 板子也连上了 (Scratch -> 网关 -> 板子 全链路通)');
    } else {
        console.log('提示: 板子还没连上 (板子没上电? IP 变了?), 但"点积木拉起服务"这条已验证通过');
    }
    process.exit(0);
}

main().catch((err) => {
    console.error('测试失败:', err);
    process.exit(2);
});
