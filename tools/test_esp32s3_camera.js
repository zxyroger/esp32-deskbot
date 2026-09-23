/**
 * esp32s3.js 摄像头积木的冒烟测试 (不需要 Scratch, 也不需要真的连板子)
 *
 * 用法:
 *     node tools\test_esp32s3_camera.js
 *
 * 做法: 用假的 Scratch / WebSocket / canvas 加载扩展, 手工驱动 onmessage。分两段:
 *   1. 先不给 Scratch.vm —— 模拟"沙箱编辑器", 验证拍照/视频失败时会给出人话提示
 *      而不是把积木卡死;
 *   2. 再挂一个假的 scratch-vm, 验证流式播放的核心性质:
 *      **连续 N 帧只创建 1 个造型**, 之后每帧都走 renderer.updateBitmapSkin()
 *      原地换贴图 —— 否则 15fps 跑一分钟就是 900 个造型, 编辑器直接卡死。
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
    // 先不给 vm: 模拟"扩展拿不到编辑器内部接口"的编辑器
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

// ---- 最小可用的浏览器环境 (扩展要 canvas 把 JPEG 转成贴图) ----

let canvasCount = 0;
global.Image = class {
    constructor() { this.width = 640; this.height = 480; }
    set src(value) {
        this._src = value;
        if (this.onload) { this.onload(); }
    }
    get src() { return this._src; }
};
global.document = {
    createElement(tag) {
        canvasCount++;
        return {
            tag,
            width: 0,
            height: 0,
            getContext: () => ({ drawImage: () => {} }),
            toBlob: (callback) => callback({ size: 123 })
        };
    }
};
global.FileReader = class {
    readAsArrayBuffer() {
        this.result = new ArrayBuffer(16);
        if (this.onload) { this.onload(); }
    }
};

const fakeFetch = () => new Promise(() => {});

new Function('Scratch', 'WebSocket', 'fetch', code)(Scratch, FakeWebSocket, fakeFetch);

let failures = 0;
function check(name, actual, expected) {
    const ok = JSON.stringify(actual) === JSON.stringify(expected);
    if (!ok) { failures++; }
    console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}\n        实际: ${JSON.stringify(actual)}` +
                (ok ? '' : `\n        期望: ${JSON.stringify(expected)}`));
}

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// 一帧最小的 JPEG (SOI + 随便几个字节 + EOI), 只用来验证 base64 透传
const FRAME_B64 = Buffer.from([0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46,
                               0x49, 0x46, 0x00, 0x01, 0xFF, 0xD9]).toString('base64');

function frameMessage(index, width, height) {
    return {
        report: 'camera_frame',
        index: index,
        format: 4,
        width: width || 640,
        height: height || 480,
        length: 14,
        data: FRAME_B64
    };
}

function lastCommand() {
    return lastSocket.sent.length ? lastSocket.sent[lastSocket.sent.length - 1] : null;
}

// ---- 假 scratch-vm: 只实现扩展会用到的那几样 ----

let assetCount = 0;
const addedCostumes = [];
const skinUpdates = [];
const fakeVm = {
    runtime: {
        storage: {
            AssetType: { ImageBitmap: { name: 'ImageBitmap' } },
            DataFormat: { PNG: 'png' },
            createAsset: () => {
                assetCount++;
                return { assetId: 'asset' + assetCount };
            }
        },
        renderer: {
            updateBitmapSkin: function (skinId, canvas, resolution, center) {
                skinUpdates.push({ skinId: skinId, resolution: resolution, center: center });
            }
        },
        on: () => {}
    },
    editingTarget: { id: 'target1', sprite: { costumes: [] } },
    addCostume: function (md5ext, costume, targetId) {
        costume.skinId = 100 + addedCostumes.length;
        this.editingTarget.sprite.costumes.push(costume);
        addedCostumes.push(costume);
        return Promise.resolve();
    }
};

async function run() {
    // 0) 扩展注册 + 新积木都在
    check('扩展已注册', extension !== null, true);
    const opcodes = extension.getInfo().blocks
        .filter((b) => typeof b === 'object')
        .map((b) => b.opcode);
    ['cameraSize', 'cameraQuality', 'openVideo', 'closeVideo', 'takePhoto',
     'cameraState', 'photoData']
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

    // 2) 「摄像头尺寸」/「摄像头质量」-> camera_config
    extension.cameraSize({ SIZE: 'QVGA' });
    check('尺寸 QVGA -> size 0', lastCommand(), { command: 'camera_config', size: 0 });
    extension.cameraSize({ SIZE: 'SXGA' });
    check('尺寸 SXGA -> size 4', lastCommand(), { command: 'camera_config', size: 4 });
    extension.cameraSize({ SIZE: '不认识' });
    check('认不出的尺寸退回 VGA(1)', lastCommand(), { command: 'camera_config', size: 1 });
    extension.cameraSize({ SIZE: 'VGA' });

    extension.cameraQuality({ QUALITY: 35 });
    check('质量 35', lastCommand(), { command: 'camera_config', quality: 35 });
    extension.cameraQuality({ QUALITY: 999 });
    check('质量超过 63 会被夹住', lastCommand(), { command: 'camera_config', quality: 63 });
    extension.cameraQuality({ QUALITY: 20 });

    // 3) 没拍过/没开过时的状态
    check('初始状态', extension.cameraState(), '还没拍过（点「拍照」试试）');
    check('还没照片时数据 URL 是空的', extension.photoData(), '');

    // 4) 板子回 0x12 -> 「摄像头状态」
    lastSocket.message({
        report: 'camera_info', state: 0, width: 640, height: 480,
        quality: 20, size_index: 1, size_name: 'VGA', xclk_mhz: 24
    });
    check('摄像头状态文字', extension.cameraState(), '640x480（VGA） 质量 20 XCLK 24MHz');

    // 5) 板块回 0x11 出错 -> 状态里给出提示
    lastSocket.message({ report: 'camera_status', state: 2, state_name: 'error', value: 3 });
    check('摄像头出错时的提示', extension.cameraState(),
        '摄像头出错（连续失败 3 次），看串口日志');

    // 6) 没有 vm 时: 单张拍照要发对命令, 而且失败不能把积木卡死
    lastSocket.sent.length = 0;
    let photoDone = false;
    const photoPromise = extension.takePhoto();
    photoPromise.then(() => { photoDone = true; });
    check('拍照发的是 camera_snapshot(1 帧)',
        lastCommand(), { command: 'camera_snapshot', frames: 1, interval: 0 });
    check('照片还没回来, 积木没有往下走', photoDone, false);
    lastSocket.message(frameMessage(1));
    check('照片数据 URL', extension.photoData(), 'data:image/jpeg;base64,' + FRAME_B64);
    await delay(20);
    check('照片回来后积木才继续', photoDone, true);
    check('没有 vm 时给的是人话提示',
        /照片拿到了，但变成造型失败.*编辑器内部接口/.test(extension.cameraState()), true);

    // ---- 从这里开始挂上假 vm, 验证流式播放 ----
    Scratch.vm = fakeVm;

    // 7) 「打开摄像头」-> 连续拍 (帧数 0) + 按间隔节流
    lastSocket.sent.length = 0;
    extension.openVideo();
    check('打开摄像头发的是 camera_snapshot(帧数 0)',
        lastCommand(), { command: 'camera_snapshot', frames: 0, interval: 80 });

    // 8) 连续 3 帧: 只建 1 个造型, 后两帧原地换贴图
    lastSocket.message(frameMessage(11));
    await delay(10);
    lastSocket.message(frameMessage(12));
    await delay(10);
    lastSocket.message(frameMessage(13));
    await delay(10);
    check('3 帧只创建了 1 个造型', addedCostumes.length, 1);
    check('造型名字固定', addedCostumes[0] && addedCostumes[0].name, '摄像头画面');
    check('后 2 帧走的是原地换贴图', skinUpdates.length, 2);
    check('换贴图用的是同一个造型',
        skinUpdates.map((u) => u.skinId), [addedCostumes[0].skinId, addedCostumes[0].skinId]);
    check('贴图按双倍分辨率挂 (640x480 -> 舞台 320x240)',
        skinUpdates[0].resolution, 2);
    check('旋转中心在画面正中',
        skinUpdates[0].center, [640 / 2 / 2, 480 / 2 / 2]);
    check('画了 3 帧', extension.videoFrames, 3);

    // 9) 视频开着时「拍一张照片」= 截当前这一帧, 不再让板子拍
    lastSocket.sent.length = 0;
    const shotPromise = extension.takePhoto();
    await shotPromise;
    await delay(20);
    check('视频中截图没有再发拍照命令', lastSocket.sent.length, 0);
    check('截图变成第 2 个造型', addedCostumes.length, 2);
    // 前面第 6 步已经拍过一张「照片 1」, 所以这次截的是「照片 2」
    check('截图造型按序号命名', addedCostumes[1] && addedCostumes[1].name, '照片 2');

    // 10) 「关闭摄像头」
    const assetBeforeClose = assetCount;
    extension.closeVideo();
    await delay(20);
    check('关闭摄像头发的是 camera_stop', lastCommand(), { command: 'camera_stop' });
    check('关闭后不再接收视频帧', extension.videoOn, false);
    check('关闭时把造型资源刷成最后一帧', assetCount > assetBeforeClose, true);
    check('关闭后的状态文字', extension.cameraState(), '摄像头已关闭');

    // 11) 本地服务断了也要把视频状态收回来 (板子发不出去会自己停流)
    extension.openVideo();
    lastSocket.close();
    check('服务断开后视频状态复位', extension.videoOn, false);

    console.log(failures === 0 ? '\n全部通过' : `\n${failures} 项失败`);
    process.exit(failures === 0 ? 0 : 1);
}

run().catch((err) => {
    console.error('测试自己抛异常了:', err);
    process.exit(2);
});
