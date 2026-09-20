/**
 * esp32s3.js 连接状态逻辑的冒烟测试 (不需要 Scratch, 也不需要真的连板子)
 *
 * 用法:
 *     node tools\test_esp32s3_status.js
 *
 * 做法: 用假的 Scratch / WebSocket 对象加载扩展, 然后手工驱动
 * onopen / onmessage / onclose, 检查「连接状态」积木返回的文字。
 */
'use strict';

const fs = require('fs');
const path = require('path');

const extFile = path.join(__dirname, '..', 'scratch', 'esp32s3.js');
const code = fs.readFileSync(extFile, 'utf8');

let extension = null;
let lastSocket = null;

const Scratch = {
    extensions: { register: (ext) => { extension = ext; } },
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
        lastSocket = this;
        FakeWebSocket.instances.push(this);
    }
    send(text) {
        this.sent.push(JSON.parse(text));
    }
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
FakeWebSocket.instances = [];

// 假 fetch: 扩展的"按需启动"用它请本机启动器拉起服务。这里给一个永不落地的
// Promise —— 既不会真的发请求, 也不会改变状态, 测试就跟本机跑没跑启动器无关。
const fakeFetch = () => new Promise(() => {});

new Function('Scratch', 'WebSocket', 'fetch', code)(Scratch, FakeWebSocket, fakeFetch);

let failures = 0;
function check(name, actual, expected) {
    const ok = actual === expected;
    if (!ok) { failures++; }
    console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}\n        实际: ${actual}${ok ? '' : `\n        期望: ${expected}`}`);
}

// 0) 扩展注册 + 新积木存在
check('扩展已注册', extension !== null, true);
const opcodes = extension.getInfo().blocks
    .filter((b) => typeof b === 'object')
    .map((b) => b.opcode);
check('有 boardStatus 积木', opcodes.includes('boardStatus'), true);
check('有 boardConnected 积木', opcodes.includes('boardConnected'), true);
check('原有积木数量没变少', extension.getInfo().blocks.filter((b) => typeof b === 'object').length >= 9, true);

// 1) 还没连本地服务
check('初始状态', extension.boardStatus(), '本地服务未启动（点任意积木会自动拉起）');
check('初始 已连接板子?', extension.boardConnected(), false);

// 2) 点「连接板子 IP」-> 立刻变成"正在连接"
extension.connect({ IP: '192.168.0.107' });
check('点 IP 积木后状态', extension.boardStatus(), '正在连接 192.168.0.107 …');

// 3) WebSocket 连上 -> 先发 id, 再发排队的 ip_address
lastSocket.open();
check('连上本地服务后仍是"正在连接"', extension.boardStatus(), '正在连接 192.168.0.107 …');
check('第一条报文是 id', JSON.stringify(lastSocket.sent[0]), JSON.stringify({ id: 'to_esp32_gateway' }));
check('第二条报文是 ip_address', JSON.stringify(lastSocket.sent[1]), JSON.stringify({ command: 'ip_address', address: '192.168.0.107' }));

// 4) 网关上报"板子还没连上"时, 保持"正在连接"
lastSocket.message({ report: 'board_status', state: 'waiting', message: '点 Scratch 里的「连接板子 IP」' });
check('网关说 waiting 时保留"正在连接"', extension.boardStatus(), '正在连接 192.168.0.107 …');

// 5) 网关上报已连上板子
lastSocket.message({ report: 'board_status', state: 'connected', address: '192.168.0.107', firmware: '3.2.0' });
check('网关上报已连接', extension.boardStatus(), '已连接板子 192.168.0.107（固件 3.2.0）');
check('已连接板子? = true', extension.boardConnected(), true);

// 6) 板子掉线
lastSocket.message({ report: 'board_status', state: 'disconnected', address: '192.168.0.107', message: '板子连接断开, 网关已自动重启' });
check('掉线提示', extension.boardStatus(), '板子连接已断开（板子连接断开, 网关已自动重启），请再点一次「连接板子 IP」');
check('掉线后 已连接板子? = false', extension.boardConnected(), false);

// 7) 连不上板子
lastSocket.message({ report: 'board_status', state: 'error', message: 'ConnectionRefusedError: [WinError 10061]' });
check('连接失败提示', extension.boardStatus(), '连接失败：ConnectionRefusedError: [WinError 10061]');

// 8) 没有守护进程时, 靠"板子有数据回传"也能确认连接
extension.connect({ IP: '192.168.0.107' });
lastSocket.message({ report: 'analog_input', pin: 32, value: 4095 });
check('收到板子数据即认为已连接', extension.boardStatus(), '已连接板子 192.168.0.107');
check('模拟读的值可用', extension.analogRead({ PIN: 32 }), 4095);

// 9) 本地服务断开
lastSocket.close();
check('WS 断开后状态', extension.boardStatus(), '本地服务未启动（点任意积木会自动拉起）');
check('WS 断开后 已连接板子? = false', extension.boardConnected(), false);

// 10) 没给 IP 时功能积木仍然排队 (不直接发出去)
const sockBefore = FakeWebSocket.instances.length;
extension.digitalWrite({ PIN: 2, VALUE: 1 });
check('没连上时功能积木只排队', FakeWebSocket.instances.length - sockBefore <= 1, true);

// 11~12) 板子没连上时不把积木指令发给网关 (刚起来的 esp32gw 会因此自己退出),
//        等 board_status 说"已连接"再按顺序补发
extension.connect({ IP: '192.168.0.107' });     // 状态回到"正在连接"
lastSocket.open();                              // 本地 WS 连上
lastSocket.sent.length = 0;                     // 清掉 id / ip_address
extension.digitalWrite({ PIN: 2, VALUE: 1 });
check('板子没连上时积木指令不发出去', lastSocket.sent.length, 0);
lastSocket.message({ report: 'board_status', state: 'connected',
                     address: '192.168.0.107', firmware: '3.2.0' });
check('连上后按顺序补发积木指令',
    JSON.stringify(lastSocket.sent.map((m) => m.command)),
    JSON.stringify(['set_mode_digital_output', 'digital_write']));

// 13) IP 自动发现: 没点过「连接板子 IP」积木时, 用网关报上来的地址当目标
extension.ipAddress = '';
extension.statusState = 'waiting';
lastSocket.message({ report: 'board_status', state: 'connected',
                     address: '192.168.0.108', firmware: '3.2.0' });
check('未填 IP 时自动采用网关上报的地址', extension.ipAddress, '192.168.0.108');
check('自动采用地址后 已连接板子? = true', extension.boardConnected(), true);

console.log(failures === 0 ? '\n全部通过' : `\n${failures} 项失败`);
process.exit(failures === 0 ? 0 : 1);
