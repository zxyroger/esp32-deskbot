/**
 * ESP32-S3 引脚积木的冒烟测试 (不需要 Scratch, 也不需要真的连板子)
 *
 * 用法:
 *     node tools\test_esp32s3_pins.js
 *
 * 盯两件事:
 *   1) GPIO10 / GPIO11 必须出现在「数字引脚」下拉框里, 而且数字读写 / PWM /
 *      舵机 / 超声波几块积木都吃这张表 (板子上空出来的就是这两个脚);
 *   2) 同一个脚换模式时, 积木要真的把 set_mode_* 发出去 —— 固件靠它把引脚
 *      从 PWM/舵机 (LEDC) 手里收回来当成普通 GPIO (见 tmx_io.c 的
 *      tmx_ledc_detach), 否则"先玩舵机、再当数字口"会写不动。
 *      最后再扫一遍固件源码, 确认这三处收回逻辑还在。
 */
'use strict';

const fs = require('fs');
const path = require('path');

const extFile = path.join(__dirname, '..', 'scratch', 'esp32s3.js');
const ioFile = path.join(__dirname, '..', 'firmware', 'main', 'tmx_io.c');
const ioHeader = path.join(__dirname, '..', 'firmware', 'main', 'tmx_io.h');
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

const fakeFetch = () => new Promise(() => {});
new Function('Scratch', 'WebSocket', 'fetch', code)(Scratch, FakeWebSocket, fakeFetch);

let failures = 0;
function check(name, actual, expected) {
    const ok = JSON.stringify(actual) === JSON.stringify(expected);
    if (!ok) { failures++; }
    console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}`);
    if (!ok) {
        console.log(`        实际: ${JSON.stringify(actual)}`);
        console.log(`        期望: ${JSON.stringify(expected)}`);
    }
}

/* 1) 下拉框: GPIO10 / GPIO11 在不在, 几张积木表用没用它 */
const info = extension.getInfo();
const digitalPins = info.menus.digitalPins.items;
check('下拉框有 GPIO10', digitalPins.includes('10'), true);
check('下拉框有 GPIO11', digitalPins.includes('11'), true);
check('下拉框有 GPIO43 (U0TXD)', digitalPins.includes('43'), true);
check('下拉框有 GPIO44 (U0RXD)', digitalPins.includes('44'), true);
check('下拉框第 4/5 项就是 10 11', digitalPins.slice(3, 5), ['10', '11']);
check('下拉框最后两项是 43 44', digitalPins.slice(-2), ['43', '44']);

const pinMenuOf = (opcode) => {
    const block = info.blocks.filter((b) => typeof b === 'object' && b.opcode === opcode)[0];
    return block && block.arguments && block.arguments.PIN
        ? block.arguments.PIN.menu : null;
};
['digitalWrite', 'pwmWrite', 'servoWrite', 'digitalRead'].forEach((opcode) => {
    check(`${opcode} 用数字引脚表`, pinMenuOf(opcode), 'digitalPins');
});
const sonar = info.blocks.filter((b) => typeof b === 'object' && b.opcode === 'sonarRead')[0];
check('sonarRead 触发脚用数字引脚表', sonar.arguments.TRIG.menu, 'digitalPins');
check('sonarRead 回波脚用数字引脚表', sonar.arguments.ECHO.menu, 'digitalPins');

/* 2) 连上板子 (和 test_esp32s3_status.js 一样的流程) */
extension.connect({ IP: '192.168.0.107' });
lastSocket.open();
lastSocket.message({ report: 'board_status', state: 'connected',
                     address: '192.168.0.107', firmware: '3.2.0' });
check('已连接板子?', extension.boardConnected(), true);
lastSocket.sent.length = 0;

/* 3) 同一个脚: 舵机 -> 数字输出, 两个 set_mode 都要发 */
extension.servoWrite({ PIN: 11, ANGLE: 90 });
check('舵机引脚 11', lastSocket.sent, [
    { command: 'set_mode_servo', pin: 11 },
    { command: 'servo_position', pin: 11, position: 90 }
]);

lastSocket.sent.length = 0;
extension.digitalWrite({ PIN: 11, VALUE: 1 });
check('11 从舵机改成数字输出(要重发 set_mode)', lastSocket.sent, [
    { command: 'set_mode_digital_output', pin: 11 },
    { command: 'digital_write', pin: 11, value: 1 }
]);

lastSocket.sent.length = 0;
extension.digitalWrite({ PIN: 11, VALUE: 0 });
check('11 已经是数字输出, 只发电平', lastSocket.sent, [
    { command: 'digital_write', pin: 11, value: 0 }
]);

/* 4) 数字输入 / PWM / 超声波 也都认这两个脚 */
lastSocket.sent.length = 0;
extension.digitalRead({ PIN: 10 });
check('10 当数字输入(内部上拉)', lastSocket.sent, [
    { command: 'set_mode_digital_input_pullup', pin: 10 }
]);
lastSocket.message({ report: 'digital_input', pin: 10, value: 1, timestamp: 1 });
check('10 上拉读到 1', extension.digitalRead({ PIN: 10 }), 1);

lastSocket.sent.length = 0;
extension.pwmWrite({ PIN: 10, VALUE: 50 });
check('10 从数字输入改成 PWM', lastSocket.sent, [
    { command: 'set_mode_pwm', pin: 10 },
    { command: 'pwm_write', pin: 10, value: 128 }
]);

lastSocket.sent.length = 0;
extension.sonarRead({ TRIG: 10, ECHO: 11 });
check('超声波用 10 触发 / 11 回波', lastSocket.sent, [
    { command: 'set_mode_sonar', trigger_pin: 10, echo_pin: 11 }
]);

/* 4b) U0TXD/U0RXD = GPIO43/44, 单独再走一遍数字输出和舵机 */
lastSocket.sent.length = 0;
extension.digitalWrite({ PIN: 43, VALUE: 1 });
check('43 (U0TXD) 当数字输出', lastSocket.sent, [
    { command: 'set_mode_digital_output', pin: 43 },
    { command: 'digital_write', pin: 43, value: 1 }
]);

lastSocket.sent.length = 0;
extension.servoWrite({ PIN: 44, ANGLE: 0 });
check('44 (U0RXD) 当舵机', lastSocket.sent, [
    { command: 'set_mode_servo', pin: 44 },
    { command: 'servo_position', pin: 44, position: 0 }
]);

/* 5) 固件侧: 三种"收回 LEDC"的入口 + 实现都在 */
const io = fs.readFileSync(ioFile, 'utf8');
const header = fs.readFileSync(ioHeader, 'utf8');
const patchScript = fs.readFileSync(path.join(__dirname, 'apply_local_patches.py'), 'utf8');
const countOf = (text, needle) => text.split(needle).length - 1;
check('tmx_io.h 声明 tmx_ledc_detach', header.includes('esp_err_t tmx_ledc_detach(int pin);'), true);
check('tmx_io.c 实现 tmx_ledc_detach', countOf(io, 'esp_err_t tmx_ledc_detach(int pin)'), 1);
check('数字输入/输出 + ADC 三处都收回 LEDC',
    (io.match(/^\s+tmx_ledc_detach\(pin\);/gm) || []).length, 3);
check('tmx_pwm_detach 走同一个实现',
    /esp_err_t tmx_pwm_detach\(int pin\)\s*\{\s*return tmx_ledc_detach\(pin\);/.test(io), true);
check('网关补丁默认放行 10 11 43 44',
    /extra_pins = args\.pins if args\.pins else \[10, 11, 43, 44\]/.test(patchScript), true);
check('日志走 UART0 时固件收回 43/44',
    /CONFIG_ESP_CONSOLE_UART_DEFAULT \|\| CONFIG_ESP_CONSOLE_UART_CUSTOM[\s\S]{0,400}?pin == 43 \|\| pin == 44/
        .test(io), true);

console.log(failures === 0 ? '\n全部通过' : `\n有 ${failures} 条不过`);
process.exit(failures === 0 ? 0 : 1);
