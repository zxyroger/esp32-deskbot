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
                skinUpdates.push({ skinId: skinId, resolution: resolution, center: center,
                                   reusable: canvas && canvas.reusable });
            }
        },
        on: () => {}
    },
    editingTarget: { id: 'target1', currentCostume: 0, sprite: { costumes: [] } },
    addCostume: function (md5ext, costume, targetId) {
        costume.skinId = 100 + addedCostumes.length;
        this.editingTarget.sprite.costumes.push(costume);
        addedCostumes.push(costume);
        this.editingTarget.currentCostume = this.editingTarget.sprite.costumes.length - 1;
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
    check('拍照质量 35', lastCommand(), { command: 'camera_config', quality: 35 });
    extension.cameraQuality({ QUALITY: 999 });
    check('质量超过 63 会被夹住', lastCommand(), { command: 'camera_config', quality: 63 });
    extension.cameraQuality({ QUALITY: 20 });

    // 2b) 「视频质量」: 只在开流时生效, 不碰拍照质量
    extension.streamQualityBlock({ QUALITY: 45 });
    check('设视频质量时没开流 -> 先不发命令', lastCommand(), { command: 'camera_config', quality: 20 });
    check('视频质量记住了', extension.streamQuality, 45);
    extension.streamQualityBlock({ QUALITY: 999 });
    check('视频质量也会被夹到 63', extension.streamQuality, 63);
    extension.streamQualityBlock({ QUALITY: 30 });
    check('视频质量 30', extension.streamQuality, 30);

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

    // 7) 「打开摄像头」-> 先把质量换成视频质量, 再连续拍 (帧数 0)
    lastSocket.sent.length = 0;
    extension.openVideo();
    check('开流时先切到视频质量',
        lastSocket.sent[0], { command: 'camera_config', quality: 30 });
    check('打开摄像头发的是 camera_snapshot(帧数 0)',
        lastCommand(), { command: 'camera_snapshot', frames: 0, interval: 30 });
    check('记下了开流前的拍照质量 (用来关流时还回去)', extension.qualityBeforeStream, 20);

    // 8) 连续 3 帧: 只建 1 个造型, 后两帧原地换贴图
    const canvasesBeforeVideo = canvasCount;
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
    check('画布标了 reusable=false (不让渲染器每帧再拷一份全画面)',
        skinUpdates[0].reusable, false);
    check('3 帧只用了 1 块画布 (复用, 不每帧新建 -> 不然显存一路涨)',
        canvasCount - canvasesBeforeVideo, 1);
    check('画了 3 帧', extension.videoFrames, 3);

    // 8b) 角色当前显示的是**别的**造型时, 扩展要把造型切回来
    //     (踩过的坑: 复制出来的「摄像头画面2」成了当前造型 —— 扩展一直在画
    //      「摄像头画面」, 舞台上却永远显示那张静止的复制品, 看着就是"画面不动")
    // 临时塞一个"别的造型"并让它成为当前造型, 模拟"复制出来的摄像头画面2"那种情况
    fakeVm.editingTarget.sprite.costumes.unshift({ name: '造型1', skinId: 999 });
    fakeVm.editingTarget.currentCostume = 0;
    lastSocket.message(frameMessage(14));
    await delay(10);
    check('扩展把造型切回「摄像头画面」',
        fakeVm.editingTarget.currentCostume,
        fakeVm.editingTarget.sprite.costumes.indexOf(addedCostumes[0]));
    check('切造型时给了人话提示', /已切回/.test(extension.cameraState()), true);
    fakeVm.editingTarget.sprite.costumes.shift();     // 收尾: 恢复原样
    fakeVm.editingTarget.currentCostume = 0;

    // 9) 视频开着时「拍一张照片」= 截当前这一帧, 不再让板子拍
    lastSocket.sent.length = 0;
    const shotPromise = extension.takePhoto();
    await shotPromise;
    await delay(20);
    check('视频中截图没有再发拍照命令', lastSocket.sent.length, 0);
    check('截图变成第 2 个造型', addedCostumes.length, 2);
    // 前面第 6 步已经拍过一张「照片 1」, 所以这次截的是「照片 2」
    check('截图造型按序号命名', addedCostumes[1] && addedCostumes[1].name, '照片 2');

    // 9b) 单帧画不出来只丢这一帧, 不许把整条流停掉
    //     (以前任意一帧出异常就 videoOn = false: 画面定格, 板子却还在出图发热)
    const renderOk = fakeVm.runtime.renderer.updateBitmapSkin;
    fakeVm.runtime.renderer.updateBitmapSkin = function () { throw new Error('假渲染错误'); };
    const skinsBeforeFail = skinUpdates.length;
    lastSocket.message(frameMessage(21));
    await delay(10);
    check('单帧渲染失败后视频没有停', extension.videoOn, true);
    check('单帧失败没有走换贴图', skinUpdates.length, skinsBeforeFail);
    check('单帧失败的提示是"继续"', /继续/.test(extension.cameraState()), true);
    fakeVm.runtime.renderer.updateBitmapSkin = renderOk;
    lastSocket.message(frameMessage(22));
    await delay(10);
    check('下一帧照常画出来', skinUpdates.length, skinsBeforeFail + 1);
    check('画成功一帧后失败计数清零', extension.videoErrors, 0);

    // 9c) 连续 5 帧都画不出来才算真坏了
    fakeVm.runtime.renderer.updateBitmapSkin = function () { throw new Error('假渲染错误'); };
    for (let i = 0; i < 5; i++) {
        lastSocket.message(frameMessage(30 + i));
        await delay(10);
    }
    check('连续 5 帧失败后才停流', extension.videoOn, false);
    check('停流时给出可操作提示',
        /连续 5 帧.*再点一次「打开摄像头」/.test(extension.cameraState()), true);
    fakeVm.runtime.renderer.updateBitmapSkin = renderOk;

    // 9d) 卡住之后「打开摄像头」再点一次 = 重开
    //     (以前 openVideo 开头 `if (this.videoOn) return;`, 点了完全没反应)
    lastSocket.sent.length = 0;
    extension.videoLastArrivalAt = 0;         // 模拟"卡住": 已经收不到帧了
    extension.openVideo();
    check('重开时重新发了 camera_snapshot',
        lastCommand(), { command: 'camera_snapshot', frames: 0, interval: 30 });
    check('重开后视频状态是开', extension.videoOn, true);
    check('重开没有覆盖"开流前的拍照质量"', extension.qualityBeforeStream, 20);

    // 9d-2) 画面正常时再点「打开摄像头」必须什么都不发
    //       (有人把它放进 forever 循环: 实测 20 秒发出去 27 万条命令, 链路直接堵死)
    extension.videoLastArrivalAt = Date.now();   // 帧刚刚还在来
    lastSocket.sent.length = 0;
    extension.openVideo();
    check('画面正常时重复点「打开摄像头」不发命令', lastSocket.sent.length, 0);

    lastSocket.message(frameMessage(40));
    await delay(10);

    // 9e) 看门狗: 帧一直在来, 但忙标记卡住了 -> 自动把最新一帧补画上去
    const skinsBeforeWatchdog = skinUpdates.length;
    extension.videoBusy = true;                       // 模拟"上一帧 promise 一直没回来"
    extension.videoLastArrivalAt = Date.now();
    extension.videoLastDrawAt = Date.now() - 5000;
    extension.videoWatchdog();
    await delay(10);
    check('看门狗清掉了卡住的忙标记', extension.videoBusy, false);
    check('看门狗补画了一帧', skinUpdates.length, skinsBeforeWatchdog + 1);
    check('看门狗记了一次恢复', extension.videoRecoveries, 1);

    // 9f) 本地服务断了 (比如守护进程重启 wsgw): 看门狗自己重连并重开摄像头
    lastSocket.close();                       // 模拟 wsgw 被杀掉
    await delay(5);
    check('服务断开后视频先复位', extension.videoOn, false);
    extension.videoWatchdog();                // 看门狗这一下应该发起重连
    await delay(5);
    const reconnectedSocket = lastSocket;
    check('看门狗新建了 WebSocket', reconnectedSocket.readyState, 0);
    reconnectedSocket.open();                 // 连上: 队列里的命令会被补发
    await delay(5);
    reconnectedSocket.message({ report: 'board_status', state: 'connected',
                                address: '192.168.0.103', firmware: '3.2.0' });
    await delay(5);
    check('重连后自动重发了 camera_snapshot',
        reconnectedSocket.sent.some((m) => m.command === 'camera_snapshot'), true);
    check('重连后视频状态自己恢复', extension.videoOn, true);

    // 9g) 调试上报: 连着的时候必须能发出来 (排障靠它)
    lastSocket.sent.length = 0;
    extension.videoDiag();
    const diagMsg = lastSocket.sent[lastSocket.sent.length - 1];
    check('调试上报发的是 video_diag', diagMsg && diagMsg.command, 'video_diag');
    check('调试上报带上关键字段',
        ['on', 'frames', 'current_costume', 'costumes', 'reports'].every((k) => k in diagMsg),
        true);

    // 9h) 流式播放中改「摄像头尺寸」: 固件会自己重建采集通路, 扩展只发命令
    lastSocket.sent.length = 0;
    extension.cameraSize({ SIZE: 'QVGA' });
    await delay(10);
    check('改尺寸先发 camera_config(size=0)',
        lastSocket.sent[0], { command: 'camera_config', size: 0 });
    check('改尺寸不会把流停掉 (固件现在支持在线换分辨率)',
        lastSocket.sent.map((m) => m.command).includes('camera_stop'), false);
    check('改尺寸后仍在流式播放', extension.videoOn, true);

    // 10) 「关闭摄像头」: 停流 + 把拍照质量还回去
    const assetBeforeClose = assetCount;
    extension.closeVideo();
    await delay(20);
    check('关闭摄像头发了 camera_stop',
        lastSocket.sent.some((m) => m.command === 'camera_stop'), true);
    check('关流时把拍照质量还回去',
        lastCommand(), { command: 'camera_config', quality: 20 });
    check('关闭后不再接收视频帧', extension.videoOn, false);
    check('关闭时把造型资源刷成最后一帧', assetCount > assetBeforeClose, true);
    check('关闭后的状态文字', extension.cameraState(), '摄像头已关闭');

    // 10b) 视频关着、也没在等照片时收到的帧: 既不能变成造型, 还要让板子停流
    //      (真实故障: 这种帧被当成"照片", 10 分钟攒了 934 个造型, 画面卡死)
    const photosBeforeStray = extension.photoCount;
    const costumesBeforeStray = addedCostumes.length;
    lastSocket.sent.length = 0;
    lastSocket.message(frameMessage(60));
    await delay(10);
    check('没人要的帧不会变成造型', addedCostumes.length, costumesBeforeStray);
    check('没人要的帧也不会算进拍照计数', extension.photoCount, photosBeforeStray);
    check('没人要的帧会让板子停流',
        lastSocket.sent.some((m) => m.command === 'camera_stop'), true);
    lastSocket.sent.length = 0;
    lastSocket.message(frameMessage(61));
    await delay(10);
    check('紧接着的垃圾帧不会再发 camera_stop (5 秒节流)', lastSocket.sent.length, 0);

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
