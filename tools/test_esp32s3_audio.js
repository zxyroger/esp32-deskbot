/**
 * esp32s3.js 音频积木的冒烟测试 (不需要 Scratch, 也不需要真的连板子)
 *
 * 用法:
 *     node tools\test_esp32s3_audio.js
 *
 * 做法: 用假的 Scratch / WebSocket 对象加载扩展, 手工把连接状态推到"已连接",
 * 然后点音频积木, 检查真的发出去的那条 Banyan 报文 (网关会把它翻成
 * Telemetrix 0x72/0x73/0x74, 见 tools/apply_local_patches.py 补丁 7)。
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
    const ok = actual === expected;
    if (!ok) { failures++; }
    console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}\n        实际: ${actual}${ok ? '' : `\n        期望: ${expected}`}`);
}

// 0) 积木已经注册
const opcodes = extension.getInfo().blocks
    .filter((b) => typeof b === 'object')
    .map((b) => b.opcode);
check('有 播放音调 积木', opcodes.includes('audioTone'), true);
check('有 停止播放音频 积木', opcodes.includes('audioStop'), true);
check('有 麦克风响度 积木', opcodes.includes('micLevel'), true);
check('有 麦克风检测 积木', opcodes.includes('micDetect'), true);
check('有 朗读文字 积木', opcodes.includes('speakText'), true);
check('有 停止朗读 积木', opcodes.includes('stopSpeaking'), true);

// 1) 建立连接 (和 test_esp32s3_status.js 一样的流程)
extension.connect({ IP: '192.168.0.107' });
lastSocket.open();
lastSocket.message({ report: 'board_status', state: 'connected',
                     address: '192.168.0.107', firmware: '3.2.0' });
check('已连接板子?', extension.boardConnected(), true);
lastSocket.sent.length = 0;      // 清掉 id / ip_address

// 2) 播放音调 -> 网关报文
extension.audioTone({ FREQ: 440, MS: 500, VOL: 60 });
check('播放音调报文',
    JSON.stringify(lastSocket.sent[0]),
    JSON.stringify({ command: 'audio_tone', frequency: 440, duration: 500, volume: 60 }));

// 3) 参数会被夹到固件能接受的范围
extension.audioTone({ FREQ: 99999, MS: 0, VOL: 250 });
check('频率/时长/音量夹取',
    JSON.stringify(lastSocket.sent[1]),
    JSON.stringify({ command: 'audio_tone', frequency: 20000, duration: 1, volume: 100 }));

extension.audioTone({ FREQ: 'abc', MS: '', VOL: '' });
check('非法参数用默认值',
    JSON.stringify(lastSocket.sent[2]),
    JSON.stringify({ command: 'audio_tone', frequency: 440, duration: 500, volume: 60 }));

// 4) 停止播放
extension.audioStop();
check('停止播放报文', JSON.stringify(lastSocket.sent[3]), JSON.stringify({ command: 'audio_stop' }));

// 5) 麦克风开关 + 响度上报
extension.micDetect({ STATE: '开' });
check('麦克风检测 开', JSON.stringify(lastSocket.sent[4]), JSON.stringify({ command: 'audio_mic', value: 1 }));
lastSocket.message({ report: 'audio_input', value: 42 });
check('麦克风响度读数', extension.micLevel(), 42);
extension.micDetect({ STATE: '关' });
check('麦克风检测 关', JSON.stringify(lastSocket.sent[5]), JSON.stringify({ command: 'audio_mic', value: 0 }));
check('关掉后响度归零', extension.micLevel(), 0);

// 6) 朗读文字 / 停止朗读 (板载 TTS): 文字原样交给网关, 由网关做 UTF-8 拆包
extension.speakText({ TEXT: '  你好，我是小乐  ' });
check('朗读文字报文', JSON.stringify(lastSocket.sent[6]),
    JSON.stringify({ command: 'audio_tts', text: '你好，我是小乐' }));
extension.speakText({ TEXT: '   ' });
check('空文字不发报文', lastSocket.sent.length, 7);
extension.stopSpeaking();
check('停止朗读报文', JSON.stringify(lastSocket.sent[7]),
    JSON.stringify({ command: 'audio_tts_stop' }));

console.log();
if (failures) {
    console.log(`有 ${failures} 项没通过`);
    process.exit(1);
}
console.log('全部通过');
