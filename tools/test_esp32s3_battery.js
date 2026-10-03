/**
 * esp32s3.js 电池低电保护逻辑的冒烟测试 (不需要 Scratch, 也不需要真的连板子)
 *
 * 用法:
 *     node tools\test_esp32s3_battery.js
 *
 * 测的是「电池状态」「电池电量低?」两块积木对固件 0x14 上报 flags 的处理:
 *   bit4 = 低电告警 (电量低于低电阈值)
 *   bit5 = 严重低电 (固件已经停推流 + 拒绝开摄像头, 挡在电池过放保护前面)
 * 网关把这两位转成 low / critical 两个字段。
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
        this.readyState = 0;
        this.sent = [];
        lastSocket = this;
        FakeWebSocket.instances.push(this);
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
FakeWebSocket.instances = [];

const fakeFetch = () => new Promise(() => {});

new Function('Scratch', 'WebSocket', 'fetch', code)(Scratch, FakeWebSocket, fakeFetch);

let failures = 0;
function check(name, actual, expected) {
    const ok = actual === expected;
    if (!ok) { failures++; }
    console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}\n        实际: ${actual}${ok ? '' : `\n        期望: ${expected}`}`);
}

// 一块电池上报的底稿, 各用例只改关心的字段
function battery(over) {
    return Object.assign({
        report: 'battery',
        present: true,
        external_power: false,
        charging: false,
        read_error: false,
        millivolts: 3900,
        percent: 62,
        vbus_millivolts: 0,
        current_ma: -120,
        rate_pph_x10: -120,
        low: false,
        critical: false
    }, over || {});
}

// 0) 积木存在
const opcodes = extension.getInfo().blocks
    .filter((b) => typeof b === 'object')
    .map((b) => b.opcode);
check('有「电池电量低?」积木', opcodes.includes('batteryLow'), true);
check('原来的电池积木还在', opcodes.includes('batteryPercent') && opcodes.includes('batteryState'), true);

// 连上板子, 后面才好收上报
extension.connect({ IP: '192.168.0.103' });
lastSocket.open();
lastSocket.message({ report: 'board_status', state: 'connected',
                     address: '192.168.0.103', firmware: '3.2.0' });

// 1) 正常电量: 不报警
lastSocket.message(battery());
check('正常: 电池状态', extension.batteryState(), '电池供电');
check('正常: 电量低?', extension.batteryLow(), false);

// 2) 低电 (固件 flags bit4)
lastSocket.message(battery({ percent: 15, low: true }));
check('低电: 电池状态', extension.batteryState(), '电量低，请充电');
check('低电: 电量低?', extension.batteryLow(), true);

// 3) 严重低电 (bit4 + bit5 同时置位, 固件已停推流)
lastSocket.message(battery({ percent: 5, low: true, critical: true }));
check('严重低电: 电池状态', extension.batteryState(), '电量极低，已停推流，请立即充电');
check('严重低电: 电量低?', extension.batteryLow(), true);

// 4) 充电中: 电量低不算问题, 状态优先说"充电中"
lastSocket.message(battery({ percent: 10, charging: true, low: true }));
check('充电中: 电池状态', extension.batteryState(), '充电中');

// 5) 老网关没有 low / critical 字段 -> 不能误报低电
lastSocket.message(battery({ percent: 15, low: undefined, critical: undefined }));
check('老网关(无字段): 电量低?', extension.batteryLow(), false);
check('老网关(无字段): 电池状态', extension.batteryState(), '电池供电');

// 6) PMIC 读不到时不猜: 状态说"读不到", 也不报低电
lastSocket.message(battery({ read_error: true, percent: -1, low: true, critical: true }));
check('读不到: 电池状态', extension.batteryState(), '读不到（检查电池接线）');
check('读不到: 电量低?', extension.batteryLow(), false);

// 7) 没接电池同理
lastSocket.message(battery({ present: false, percent: -1, low: true, critical: true }));
check('未接电池: 电池状态', extension.batteryState(), '未接电池');
check('未接电池: 电量低?', extension.batteryLow(), false);

console.log(failures === 0 ? '\n全部通过' : `\n${failures} 项失败`);
process.exit(failures === 0 ? 0 : 1);
