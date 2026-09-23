/**
 * esp32s3.js 摄像头积木的冒烟测试 (不需要 Scratch, 也不需要真的连板子)
 *
 * 用法:
 *     node tools\test_esp32s3_camera.js
 *
 * 做法: 用假的 Scratch / WebSocket 加载扩展, 手工驱动 onmessage, 检查:
 *   - 四块命令积木发出去的 command / 参数对不对;
 *   - 板子回 0x12 (camera_info) / 0x11 (camera_status) 时「摄像头状态」怎么显示;
 *   - 收到一整帧 (camera_frame) 后「照片（数据 URL）」拿到的内容;
 *   - 「拍照」积木会等照片回来才继续往下走。
 *
 * 注意: 这里没有编辑器的 scratch-vm, 所以"变成造型"那一步必然失败 ——
 * 正好用来验证失败时会给出人话提示, 而不是把积木卡死。
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
    // 故意不给 Scratch.vm: 模拟"扩展拿不到编辑器内部接口"的编辑器
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
    console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}\n        实际: ${JSON.stringify(actual)}` +
                (ok ? '' : `\n        期望: ${JSON.stringify(expected)}`));
}

// 一帧最小的 JPEG (SOI + 随便几个字节 + EOI), 只用来验证 base64 透传
const FRAME_B64 = Buffer.from([0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46,
                               0x49, 0x46, 0x00, 0x01, 0xFF, 0xD9]).toString('base64');

function lastCommand() {
    return lastSocket.sent.length ? lastSocket.sent[lastSocket.sent.length - 1] : null;
}

// 0) 扩展注册 + 新积木都在
check('扩展已注册', extension !== null, true);
const blocks = extension.getInfo().blocks.filter((b) => typeof b === 'object');
const opcodes = blocks.map((b) => b.opcode);
['cameraSize', 'cameraQuality', 'takePhoto', 'cameraStop', 'cameraState', 'photoData']
    .forEach((op) => check(`有 ${op} 积木`, opcodes.includes(op), true));
check('菜单里有分辨率选项',
    extension.getInfo().menus.cameraSizes.items,
    ['QVGA', 'VGA', 'SVGA', 'XGA', 'SXGA', 'UXGA']);

// 1) 连上本地服务 + 板子
extension.connect({ IP: '192.168.0.103' });
lastSocket.open();
lastSocket.message({ report: 'board_status', state: 'connected',
                     address: '192.168.0.103', firmware: '3.2.0' });
check('板子已连接', extension.boardConnected(), true);
lastSocket.sent.length = 0;

// 2) 「拍照尺寸」/「拍照质量」-> camera_config
extension.cameraSize({ SIZE: 'QVGA' });
check('拍照尺寸 QVGA -> size 0', lastCommand(),
    { command: 'camera_config', size: 0 });
extension.cameraSize({ SIZE: 'SXGA' });
check('拍照尺寸 SXGA -> size 4', lastCommand(),
    { command: 'camera_config', size: 4 });
extension.cameraSize({ SIZE: '不认识' });
check('认不出的尺寸退回 VGA(1)', lastCommand(),
    { command: 'camera_config', size: 1 });

extension.cameraQuality({ QUALITY: 35 });
check('拍照质量 35', lastCommand(), { command: 'camera_config', quality: 35 });
extension.cameraQuality({ QUALITY: 999 });
check('质量超过 63 会被夹住', lastCommand(), { command: 'camera_config', quality: 63 });
extension.cameraQuality({ QUALITY: -5 });
check('质量小于 0 会被夹住', lastCommand(), { command: 'camera_config', quality: 0 });

// 3) 没拍过的时候状态积木说什么
check('没拍过时的状态', extension.cameraState(), '还没拍过（点「拍照」试试）');
check('没拍过的照片数据是空的', extension.photoData(), '');

// 4) 板子回 0x12 -> 「摄像头状态」
lastSocket.message({
    report: 'camera_info', state: 0, width: 640, height: 480,
    quality: 20, size_index: 1, size_name: 'VGA', xclk_mhz: 24
});
check('摄像头状态文字', extension.cameraState(),
    '640x480（VGA） 质量 20 XCLK 24MHz');

// 5) 板子回 0x11 出错 -> 状态里给出提示
lastSocket.message({ report: 'camera_status', state: 2, state_name: 'error', value: 3 });
check('摄像头出错时的提示', extension.cameraState(),
    '摄像头出错（连续失败 3 次），看串口日志');
lastSocket.message({ report: 'camera_info', state: 0, width: 640, height: 480,
                     quality: 20, size_index: 1, size_name: 'VGA', xclk_mhz: 24 });

// 6) 「拍照」发的是 camera_snapshot, 而且要等照片回来才继续
lastSocket.sent.length = 0;
let photoDone = false;
const photoPromise = extension.takePhoto();
photoPromise.then(() => { photoDone = true; });
check('拍照发的是 camera_snapshot', lastCommand(),
    { command: 'camera_snapshot', frames: 1, interval: 0 });
check('照片还没回来, 积木没有往下走', photoDone, false);

// 7) 一整帧回来 -> 存成 data URL; 没有 vm 时给出人话提示, 但积木要放行
lastSocket.message({
    report: 'camera_frame', index: 1, format: 4, width: 640, height: 480,
    length: 14, data: FRAME_B64
});
check('照片数据 URL', extension.photoData(), 'data:image/jpeg;base64,' + FRAME_B64);

// 8) 「停止拍照」
lastSocket.sent.length = 0;
extension.cameraStop();
check('停止拍照发的是 camera_stop', lastCommand(), { command: 'camera_stop' });

// photoPromise 的 then 要走好几层微任务, 让它跑完再核对 (takePhoto 里那个
// 20 秒超时定时器还挂着, 所以最后直接 process.exit, 不让测试干等)
setTimeout(() => {
    check('照片回来后积木才继续', photoDone, true);
    check('造型失败时给的是人话提示',
        /变成造型失败.*编辑器内部接口/.test(extension.cameraState()), true);
    console.log(failures === 0 ? '\n全部通过' : `\n${failures} 项失败`);
    // takePhoto 里那个 20 秒超时定时器还挂着, 直接退, 别让测试干等
    process.exit(failures === 0 ? 0 : 1);
});
