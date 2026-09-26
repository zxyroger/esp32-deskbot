/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * 本文件改自 MrYsLab 的 OneGPIO / s3onegpio Scratch 扩展 (上游项目及其协议见
 * docs/THIRD_PARTY_NOTICES.md)。上游同一套工具链 (s3-extend / Telemetrix4Esp32 /
 * telemetrix-esp32) 使用 AGPL-3.0, 因此本文件与整个仓库都按 AGPL-3.0 发布。
 *
 * 本仓库改动部分: Copyright (c) 2026 zxyroger
 */
/**
 * ESP32-S3 (s3-extend / OneGPIO) Scratch 3 扩展
 *
 * 与上游 s3onegpio 的 OneGpio ESP32 扩展等价，但做了两处修正：
 *   1. Banyan 主题名用正确的 to_esp32_gateway / from_esp32_gateway
 *      （上游 master 里写成了 to_esp8232_gateway，多一个 2，导致积木没反应）
 *   2. 只使用 Worker 环境里可用的 API（WebSocket / Scratch.*），
 *      因此可以通过 URL 加载，不依赖 window / alert / sweetalert
 *
 * 用法:
 *   1. PC 上跑一次 tools\start_launcher.ps1  (微型启动器, 常驻 127.0.0.1:8000)
 *      —— 之后点任意积木, 扩展会自己请启动器把 s3-extend 拉起来, 不用手工启动;
 *         想开机后就常驻启动器: tools\install_autostart.ps1 -OnDemand
 *   2. 编辑器里加载本文件（TurboWarp: 添加扩展 -> 自定义扩展 -> URL/文件）
 *   3. 用"连接 IP 地址 [ ]"积木填板子的 IP
 */
(function (Scratch) {
    'use strict';

    /*
     * 摄像头积木里「拍完直接变成造型」需要 scratch-vm 的内部接口
     * (runtime.storage / vm.addCostume), 沙箱里的扩展拿不到 —— 这一行请求
     * 编辑器把扩展跑在非沙箱环境 (TurboWarp 支持; 不支持的编辑器会忽略它,
     * 那时拍照仍然能拿到照片数据, 只是不会自动生成造型)。
     */
    Scratch.extensions.unsandboxed = true;

    var WS_URL = 'ws://127.0.0.1:9007';
    var BANYAN_ID = 'to_esp32_gateway';       // 与 s3-extend 的 esp32gw 一致
    var REPORT_TOPIC = 'from_esp32_gateway';
    var QUEUE_MAX = 200;                      // 排队上限, 防止死循环把内存撑爆

    // ---- 按需启动 (点积木自动拉起服务) ----
    // 扩展跑在编辑器沙箱里, 起不了本机进程, 所以由本机常驻的"启动器"
    // (tools\start_launcher.ps1 -> onegpio_launcher.py) 代劳:
    // 扩展发一个 HTTP /start, 启动器把 s3-extend 拉起来, 我们再连 9007。
    var AUTOSTART_ENABLED = true;             // 关掉就退回"手动启动服务"
    var LAUNCHER_HOST = 'http://127.0.0.1';
    var LAUNCHER_PORTS = [8000, 8001];        // 启动器端口, 依次尝试
    var AUTOSTART_DELAY_MS = 700;             // 点积木后先给 WebSocket 一点时间,
                                              // 真连不上才去请启动器 (服务已经在跑时不打扰)
    var AUTOSTART_COOLDOWN_MS = 8000;         // 两次请求拉起的最小间隔(防连点)
    var AUTOSTART_POLL_MS = 1500;             // 拉起期间的 WebSocket 重连间隔
    var AUTOSTART_GIVEUP_MS = 60000;          // 一次拉起最多等多久

    // 10 / 11 是这块板子引出舵机排针的 GPIO (ESP32-S3 上它们不是保留脚)
    var DIGITAL_PINS = ['2', '4', '5', '10', '11', '12', '13', '14', '16', '17', '18', '19', '21'];
    var ANALOG_PINS = ['32', '33', '34', '35', '36', '39'];

    // 摄像头: 分辨率索引与固件一致 (0=QVGA ~ 5=UXGA)
    var CAMERA_SIZES = ['QVGA', 'VGA', 'SVGA', 'XGA', 'SXGA', 'UXGA'];
    var CAMERA_PHOTO_TIMEOUT_MS = 20000;   // 「拍照」积木最多等这么久
    var CAMERA_INFO_REFRESH_MS = 2000;     // 「摄像头状态」的查询/显示节流
    // 流式播放: 板子两帧之间的最小间隔 (实际帧率还受 WiFi / 解码速度限制)。
    // 想更流畅就把「摄像头尺寸」设成 QVGA —— 一帧只有 VGA 的 1/4。
    //
    // 别把这里当"限制", 它只防板子空转: 实测板子到 PC 的链路上限约 50~90 KB/s,
    // QVGA 一帧 3~5KB 时链路能喂到 15~20 帧/秒 —— 之前写 80ms 等于自己先把
    // 帧率锁死在 12.5 帧/秒, 白白丢了一截。
    var CAMERA_STREAM_INTERVAL_MS = 30;
    // 视频默认用比照片更"小"的质量 (数字大 = 帧小): 预览不需要那么细,
    // 但帧小了帧率能翻倍。点「打开摄像头」时临时用这个值, 关闭时恢复拍照质量。
    var CAMERA_STREAM_DEFAULT_QUALITY = 35;
    // 视频渲染的容错: 单帧画不出来只丢这一帧, **连续**这么多帧都画不出来才算真坏了。
    // (以前任意一帧出错就 videoOn = false: 整条流永久停掉, 但板子那边还在出图发热)
    var CAMERA_VIDEO_MAX_ERRORS = 5;
    // 看门狗: 帧一直在来、却这么久没画成功 -> 判定卡在"上一帧还没画完"上, 自动补画
    var CAMERA_VIDEO_STALL_MS = 3000;
    var VIDEO_COSTUME_NAME = '摄像头画面';

    // 引脚模式编号 (与固件/telemetrix 协议一致)
    var AT_OUTPUT = 1;
    var AT_ANALOG = 3;
    var AT_INPUT = 0;
    var AT_INPUT_PULLUP = 2;

    function Esp32S3() {
        this.socket = null;
        this.connected = false;
        this.ipAddress = '';
        this.queued = [];
        // 是否收到过守护进程上报的 board_status。
        // 没守护进程(手工起 s3-extend)时收不到, 那就退回老行为: 填了 IP 就照发。
        this.sawBoardStatus = false;
        this.digitalInputs = {};
        this.analogInputs = {};
        this.sonarDistances = {};
        this.micLevelValue = 0;
        this.pinModes = {};
        this.lastSonarTrigger = -1;
        // 连接状态 (给「连接状态」/「已连接板子?」积木用)
        this.statusState = 'offline';       // offline/local/starting/connecting/connected/disconnected/error/waiting
        this.statusAddress = '';
        this.statusFirmware = '';
        this.statusMessage = '';
        this.statusText = '本地服务未启动（点任意积木会自动拉起）';
        // 按需启动相关
        this.launcherPort = 0;              // 已经应答过的启动器端口
        this.launcherState = 'unknown';     // unknown/starting/ready/missing/disabled
        this.launcherError = '';            // 启动器拒绝时的原因 (HTTP 403 之类)
        this.launcherHttpStatus = 0;        // 启动器回了非 2xx 时的状态码
        this.launching = false;             // 正在等启动器拉起服务
        this.lastLaunchAt = 0;              // 上次请求拉起的时间戳
        this.waitDeadline = 0;              // 这次拉起最多等到什么时候
        this.pollTimer = null;              // 拉起期间的轮询定时器
        this.launchTimer = null;            // "点积木后 0.7 秒还没连上就拉起"的定时器
        this.pendingConnectIp = '';         // 服务起来后要接着连的板子 IP
        // 摄像头 (OV2640 拍照)
        this.photoDataUrl = '';             // 最近一张照片的 data URL
        this.photoCount = 0;                // 已经变成造型的照片数
        this.photoDeferred = null;          // 「拍照」积木在等的那个 promise
        this.cameraInfoValue = null;        // 最近一次 0x12 摄像头状态
        this.cameraInfoAt = 0;              // 上面那次的时刻 (节流用)
        // 「摄像头状态」积木显示的就是最近发生的这一件事 (拍照结果 / 出错 / 分辨率)
        this.cameraNote = '还没拍过（点「拍照」试试）';
        this.cameraNoteAt = 0;
        // 流式播放 (打开摄像头 -> 每帧原地更新同一个造型)
        this.videoOn = false;
        this.videoCostume = null;           // 正在被刷新的那个造型对象
        this.videoLastCanvas = null;        // 最后一帧画布, 关闭时把资源刷成它
        this.videoLastFrame = null;         // 最后一帧的原始消息 (视频中"截图"用)
        this.videoBusy = false;             // 上一帧还没画完
        this.videoFrames = 0;               // 画了多少帧
        this.videoDropped = 0;              // 因为上一帧没画完而丢掉的帧
        this.videoFps = 0;
        this.videoFpsAt = 0;
        this.videoFpsCount = 0;
        this.videoErrors = 0;               // 连续画失败的帧数 (画成功一帧就清零)
        this.videoLastError = '';           // 最近一次画失败的原因 (给人看的)
        this.videoLastArrivalAt = 0;        // 最近收到一帧的时刻 (看门狗用)
        this.videoLastDrawAt = 0;           // 最近画成功一帧的时刻 (看门狗用)
        this.videoAliveAt = 0;              // "这条流还活着"的时刻 (板子重启后靠它发现)
        this.lastReviveAt = 0;              // 上次自动重开流的时刻 (节流用)
        this.videoRecoveries = 0;           // 看门狗自动重画了几次
        this.reportsReceived = 0;           // 一共收到多少条板上/网关上报 (调试用)
        this.lastReport = '';               // 最近一条上报的类型
        this.lastReportAt = 0;              // 最近一条上报的时刻
        this.streamQuality = CAMERA_STREAM_DEFAULT_QUALITY;   // 「视频质量」积木设的
        this.qualityBeforeStream = -1;      // 开流前的拍照质量, 关流时还回去
    }

    // 把状态拼成一句给人看的话。
    // 状态有两个来源: 本地 WebSocket 的连接情况, 以及守护进程上报的 board_status。
    function formatStatus(ext) {
        var state = ext.statusState;
        var address = ext.statusAddress;
        var firmware = ext.statusFirmware;
        var message = ext.statusMessage;
        if (state === 'connected') {
            var text = '已连接板子';
            if (address) { text += ' ' + address; }
            if (firmware) { text += '（固件 ' + firmware + '）'; }
            return text;
        }
        if (state === 'starting') {
            return '正在启动本地服务…（点积木触发的自动拉起，约 10~20 秒）';
        }
        if (state === 'connecting') {
            return '正在连接 ' + (address || '板子') + ' …';
        }
        if (state === 'disconnected') {
            return '板子连接已断开' + (message ? '（' + message + '）' : '') +
                '，请再点一次「连接板子 IP」';
        }
        if (state === 'error') {
            return '连接失败' + (message ? '：' + message : '');
        }
        if (state === 'waiting') {
            return '网关已运行，但还没连上板子' + (message ? '（' + message + '）' : '');
        }
        if (state === 'local') {
            return '已连上本地服务，等点「连接板子 IP」';
        }
        // offline: 本地服务没在跑, 说清楚"下一步该干什么"
        if (!AUTOSTART_ENABLED || ext.launcherState === 'disabled') {
            return '本地服务未连接（先运行 tools\\start_s3extend.ps1）';
        }
        if (ext.launcherState === 'missing') {
            if (ext.launcherError) {
                return '启动器拒绝了自动拉起（' + ext.launcherError + '）：' +
                       '可以打开 http://127.0.0.1:8000/ 手动点「启动服务」';
            }
            return '本地服务未启动，启动器也没在运行（先跑一次 tools\\start_launcher.ps1）';
        }
        if (ext.launcherState === 'starting') {
            return '正在启动本地服务…（点积木触发的自动拉起，约 10~20 秒）';
        }
        return '本地服务未启动（点任意积木会自动拉起）';
    }

    /* ------------------------- 摄像头辅助 ------------------------- */

    function makeDeferred() {
        var deferred = {};
        deferred.promise = new Promise(function (resolve) { deferred.resolve = resolve; });
        return deferred;
    }

    /*
     * 板子给的是 JPEG。先解成 canvas: 拍照片那条路要转 PNG 存资源,
     * 流式播放那条路直接把 canvas 交给渲染器换贴图 (不新建造型)。
     *
     * reuseCanvas = true 时复用同一块画布 (流式播放专用): 每帧都 new 一个 XGA
     * 画布, 等于每帧要一块 3MB 的画布后备存储 —— 实测 TurboWarp 的 GPU 进程内存
     * 因此以 30~40MB/s 往上涨 (一直涨到 2.8GB 才回收), 这就是"软件用着用着整个
     * 崩掉"的根源。渲染器收到 canvas 会在 updateBitmapSkin() 里**同步**做
     * texImage2D (scratch-render 的 BitmapSkin._setTexture), 上传完再覆盖它是安全的。
     * 拍照/截图那条路仍然每次新建 (要拿去编码 PNG, 不能和视频抢同一块画布)。
     */
    var v_streamCanvas = null;

    function decodeJpegToCanvas(base64, reuseCanvas) {
        return new Promise(function (resolve, reject) {
            var image = new Image();
            image.onload = function () {
                var canvas;
                if (reuseCanvas) {
                    if (!v_streamCanvas) {
                        v_streamCanvas = document.createElement('canvas');
                    }
                    canvas = v_streamCanvas;
                    if (canvas.width !== image.width || canvas.height !== image.height) {
                        canvas.width = image.width;
                        canvas.height = image.height;
                    }
                } else {
                    canvas = document.createElement('canvas');
                    canvas.width = image.width;
                    canvas.height = image.height;
                }
                canvas.getContext('2d').drawImage(image, 0, 0);
                /*
                 * 这一行是必须的: scratch-render 收到 canvas 时, 默认会先做一次
                 *   canvas.getContext('2d').getImageData(0, 0, w, h)
                 * 全画面拷贝 (见 BitmapSkin.setBitmap), 只有 canvas.reusable === false
                 * 才直接把画布上传给纹理。
                 *
                 * 不标的话: 视频每一帧都白拷一份像素 —— 1024x768 是 3MB, 1280x1024
                 * 是 5MB, 6 帧/秒就是每秒十几 MB 的垃圾; 渲染进程内存一路涨
                 * (实测 30 秒 +96MB)。
                 * TurboWarp 自己(Sprite2/位图适配)交 canvas 给渲染器时也是这么标的。
                 * 它的意思是"上传完就随便你了": 渲染器是同步上传的, 所以流式播放
                 * 复用同一块画布也没问题 (只有轮廓/碰撞检测会读到最新内容, 用不到)。
                 */
                canvas.reusable = false;
                resolve(canvas);
            };
            image.onerror = function () { reject(new Error('画面解码失败')); };
            image.src = 'data:image/jpeg;base64,' + base64;
        });
    }

    function canvasToPngBytes(canvas) {
        return new Promise(function (resolve, reject) {
            canvas.toBlob(function (blob) {
                if (!blob) {
                    reject(new Error('画面转 PNG 失败'));
                    return;
                }
                var reader = new FileReader();
                reader.onload = function () { resolve(new Uint8Array(reader.result)); };
                reader.onerror = function () { reject(new Error('画面转 PNG 读回失败')); };
                reader.readAsArrayBuffer(blob);
            }, 'image/png');
        });
    }

    /*
     * 板子给的是 JPEG, 而 Scratch 的位图造型是 PNG —— 先过一遍 canvas 转成 PNG。
     * (和 Scratch 自己"上传一张 jpg"时做的事一样: 直接塞 JPEG 资源虽然浏览器
     *  多半也能画出来, 但 DataFormat 和渲染器的假设对不上, 不保险。)
     */
    function jpegToPngBytes(base64) {
        return decodeJpegToCanvas(base64).then(canvasToPngBytes);
    }

    // 在当前角色上找一个叫这个名的造型 (流式播放要复用它, 不能每帧都新建)
    function findCostume(target, name) {
        if (!target || !target.sprite || !target.sprite.costumes) {
            return null;
        }
        var costumes = target.sprite.costumes;
        for (var i = 0; i < costumes.length; i++) {
            if (costumes[i] && costumes[i].name === name) {
                return costumes[i];
            }
        }
        return null;
    }

    /*
     * 让舞台上显示的确实是「摄像头画面」这块造型。
     *
     * 踩过的坑: 造型被复制过一份 (TurboWarp 自动命名成「摄像头画面2」) 并成了
     * 当前造型, 之后扩展每帧都在刷原来的「摄像头画面」—— 扩展画得再勤, 舞台上
     * 显示的也是那张静止的复制品, 看起来就是"画面不动"。
     * 返回 true 表示这次真的把造型切回来了。
     */
    function showVideoCostume(vm, costume) {
        var target = vm && vm.editingTarget;
        if (!costume || !target || !target.sprite || !target.sprite.costumes) {
            return false;
        }
        var index = target.sprite.costumes.indexOf(costume);
        if (index < 0 || target.currentCostume === index) {
            return false;
        }
        try {
            target.currentCostume = index;      // scratch-vm 的 setter 会同步给渲染器
            return true;
        } catch (ignored) {
            return false;
        }
    }

    /*
     * 把一帧照片加成当前角色的一个新造型。
     *
     * 扩展本身没有"加造型"的 API, 得借编辑器内部的 scratch-vm:
     *   1. runtime.storage.createAsset() 把 PNG 字节存成一个资源;
     *   2. vm.addCostume(md5ext, costume, targetId) 加载并挂到角色上,
     *      顺手把它设成当前造型。
     * 这要求扩展跑在非沙箱模式 (见文件开头的 Scratch.extensions.unsandboxed)。
     * 拿不到 VM 时抛错, 外面会把它显示成一句人话。
     */
    function addPhotoCostume(msg, base64, name) {
        return new Promise(function (resolve, reject) {
            var vm = Scratch.vm;
            if (!vm || !vm.runtime || !vm.runtime.storage || !vm.runtime.renderer ||
                    !vm.editingTarget || !vm.editingTarget.sprite) {
                reject(new Error('拿不到编辑器内部接口，没法自动变成造型' +
                                 '（换 TurboWarp 这类支持非沙箱扩展的编辑器打开）'));
                return;
            }
            jpegToPngBytes(base64).then(function (bytes) {
                var storage = vm.runtime.storage;
                var asset = storage.createAsset(storage.AssetType.ImageBitmap,
                                                storage.DataFormat.PNG,
                                                bytes, null, true);
                var width = parseInt(msg['width'], 10) || 0;
                var height = parseInt(msg['height'], 10) || 0;
                var costume = {
                    name: name,
                    asset: asset,
                    assetId: asset.assetId,
                    dataFormat: storage.DataFormat.PNG,
                    md5: asset.assetId + '.' + storage.DataFormat.PNG,
                    // 按"双倍分辨率"挂: 640x480 的照片在 480x360 的舞台上占 320x240,
                    // 正好放得下; 用 1 的话会变成 640x480 单位, 四边都被裁掉。
                    bitmapResolution: 2,
                    rotationCenterX: width / 2,
                    rotationCenterY: height / 2
                };
                return vm.addCostume(costume.md5, costume, vm.editingTarget.id);
            }).then(function () {
                resolve('已把「' + name + '」加为造型');
            }).catch(reject);
        });
    }

    /*
     * 流式播放: 把一帧画到舞台上。
     *
     * 关键点是**不能每帧新建造型** —— 15fps 跑一分钟就是 900 个造型, 编辑器会卡死。
     * 所以第一帧用 vm.addCostume 建一个固定名字的造型, 之后每一帧都只是
     * renderer.updateBitmapSkin() 原地换贴图, 造型数量不变、内存不涨。
     * 代价是资源的 assetId 还停在第一帧, 所以关闭摄像头时会再刷一次资源
     * (见 refreshVideoAsset), 这样工程存档里存的是最后一帧而不是第一帧。
     */
    function renderVideoFrame(msg) {
        return new Promise(function (resolve, reject) {
            var vm = Scratch.vm;
            if (!vm || !vm.runtime || !vm.runtime.renderer || !vm.runtime.storage ||
                    !vm.editingTarget || !vm.editingTarget.sprite) {
                reject(new Error('拿不到编辑器内部接口（要用 TurboWarp 这类支持非沙箱扩展的编辑器）'));
                return;
            }
            var width = parseInt(msg['width'], 10) || 0;
            var height = parseInt(msg['height'], 10) || 0;
            var resolution = 2;     // 与"照片造型"一致: 640x480 在舞台上占 320x240
            var center = [width / 2 / resolution, height / 2 / resolution];

            decodeJpegToCanvas(msg['data'], true).then(function (canvas) {
                var renderer = vm.runtime.renderer;
                var existing = findCostume(vm.editingTarget, VIDEO_COSTUME_NAME);
                if (existing) {
                    renderer.updateBitmapSkin(existing.skinId, canvas, resolution, center);
                    existing.size = [width, height];
                    existing.rotationCenterX = width / 2;
                    existing.rotationCenterY = height / 2;
                    existing.bitmapResolution = resolution;
                    // 别画在一块"没被显示"的造型上 (见 showVideoCostume)
                    resolve({ costume: existing, canvas: canvas,
                              switched: showVideoCostume(vm, existing) });
                    return;
                }
                // 第一帧: 存成 PNG 资源, 挂一个固定名字的造型
                canvasToPngBytes(canvas).then(function (bytes) {
                    var storage = vm.runtime.storage;
                    var asset = storage.createAsset(storage.AssetType.ImageBitmap,
                                                    storage.DataFormat.PNG,
                                                    bytes, null, true);
                    var costume = {
                        name: VIDEO_COSTUME_NAME,
                        asset: asset,
                        assetId: asset.assetId,
                        dataFormat: storage.DataFormat.PNG,
                        md5: asset.assetId + '.' + storage.DataFormat.PNG,
                        bitmapResolution: resolution,
                        rotationCenterX: width / 2,
                        rotationCenterY: height / 2
                    };
                    return vm.addCostume(costume.md5, costume, vm.editingTarget.id)
                        .then(function () {
                            resolve({ costume: costume, canvas: canvas,
                                      switched: showVideoCostume(vm, costume) });
                        });
                }).catch(reject);
            }).catch(reject);
        });
    }

    Esp32S3.prototype.getInfo = function () {
        return {
            id: 'esp32s3OneGPIO',
            name: 'ESP32-S3 (s3-extend)',
            color1: '#0C5986',
            color2: '#34B0F7',
            blocks: [
                {
                    opcode: 'connect',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '连接板子 IP [IP]',
                    arguments: {
                        IP: { type: Scratch.ArgumentType.STRING, defaultValue: '192.168.1.123' }
                    }
                },
                '---',
                {
                    opcode: 'digitalWrite',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '数字引脚 [PIN] 设为 [VALUE]',
                    arguments: {
                        PIN: { type: Scratch.ArgumentType.NUMBER, defaultValue: '2', menu: 'digitalPins' },
                        VALUE: { type: Scratch.ArgumentType.NUMBER, defaultValue: '1', menu: 'onOff' }
                    }
                },
                {
                    opcode: 'pwmWrite',
                    blockType: Scratch.BlockType.COMMAND,
                    text: 'PWM 引脚 [PIN] 输出 [VALUE] %',
                    arguments: {
                        PIN: { type: Scratch.ArgumentType.NUMBER, defaultValue: '5', menu: 'digitalPins' },
                        VALUE: { type: Scratch.ArgumentType.NUMBER, defaultValue: '50' }
                    }
                },
                {
                    opcode: 'servoWrite',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '舵机引脚 [PIN] 转到 [ANGLE] 度',
                    arguments: {
                        PIN: { type: Scratch.ArgumentType.NUMBER, defaultValue: '4', menu: 'digitalPins' },
                        ANGLE: { type: Scratch.ArgumentType.NUMBER, defaultValue: '90' }
                    }
                },
                '---',
                {
                    opcode: 'digitalRead',
                    blockType: Scratch.BlockType.REPORTER,
                    text: '数字引脚 [PIN] 的值',
                    arguments: {
                        PIN: { type: Scratch.ArgumentType.NUMBER, defaultValue: '4', menu: 'digitalPins' }
                    }
                },
                {
                    opcode: 'analogRead',
                    blockType: Scratch.BlockType.REPORTER,
                    text: '模拟引脚 [PIN] 的值',
                    arguments: {
                        PIN: { type: Scratch.ArgumentType.NUMBER, defaultValue: '32', menu: 'analogPins' }
                    }
                },
                {
                    opcode: 'sonarRead',
                    blockType: Scratch.BlockType.REPORTER,
                    text: '超声波(厘米) 触发 [TRIG] 回波 [ECHO]',
                    arguments: {
                        TRIG: { type: Scratch.ArgumentType.NUMBER, defaultValue: '4', menu: 'digitalPins' },
                        ECHO: { type: Scratch.ArgumentType.NUMBER, defaultValue: '5', menu: 'digitalPins' }
                    }
                },
                '---',
                {
                    opcode: 'screenBacklight',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '屏幕背光 [STATE]',
                    arguments: {
                        STATE: { type: Scratch.ArgumentType.STRING, defaultValue: '开', menu: 'backlightState' }
                    }
                },
                {
                    opcode: 'screenColor',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '屏幕颜色设为 [COLOR]',
                    arguments: {
                        COLOR: { type: Scratch.ArgumentType.COLOR, defaultValue: '#2fb0f7' }
                    }
                },
                '---',
                {
                    opcode: 'audioTone',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '播放音调 频率 [FREQ] Hz 时长 [MS] 毫秒 音量 [VOL] %',
                    arguments: {
                        FREQ: { type: Scratch.ArgumentType.NUMBER, defaultValue: '440' },
                        MS: { type: Scratch.ArgumentType.NUMBER, defaultValue: '500' },
                        VOL: { type: Scratch.ArgumentType.NUMBER, defaultValue: '60' }
                    }
                },
                {
                    opcode: 'audioStop',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '停止播放音频',
                    arguments: {}
                },
                {
                    opcode: 'micLevel',
                    blockType: Scratch.BlockType.REPORTER,
                    text: '麦克风响度',
                    arguments: {}
                },
                {
                    opcode: 'micDetect',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '麦克风检测 [STATE]',
                    arguments: {
                        STATE: { type: Scratch.ArgumentType.STRING, defaultValue: '开', menu: 'backlightState' }
                    }
                },
                {
                    opcode: 'speakText',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '朗读文字 [TEXT]',
                    arguments: {
                        TEXT: { type: Scratch.ArgumentType.STRING, defaultValue: '你好，我是小乐' }
                    }
                },
                {
                    opcode: 'stopSpeaking',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '停止朗读',
                    arguments: {}
                },
                '---',
                {
                    opcode: 'cameraSize',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '摄像头尺寸 [SIZE]',
                    arguments: {
                        SIZE: {
                            type: Scratch.ArgumentType.STRING,
                            menu: 'cameraSizes',
                            defaultValue: 'VGA'
                        }
                    }
                },
                {
                    opcode: 'cameraQuality',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '拍照质量 [QUALITY]',
                    arguments: {
                        QUALITY: { type: Scratch.ArgumentType.NUMBER, defaultValue: 20 }
                    }
                },
                {
                    opcode: 'streamQualityBlock',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '视频质量 [QUALITY]',
                    arguments: {
                        QUALITY: { type: Scratch.ArgumentType.NUMBER, defaultValue: 35 }
                    }
                },
                {
                    opcode: 'openVideo',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '打开摄像头（画面显示在当前角色上）',
                    arguments: {}
                },
                {
                    opcode: 'closeVideo',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '关闭摄像头',
                    arguments: {}
                },
                {
                    opcode: 'takePhoto',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '拍一张照片（变成新造型）',
                    arguments: {}
                },
                {
                    opcode: 'cameraState',
                    blockType: Scratch.BlockType.REPORTER,
                    text: '摄像头状态',
                    arguments: {}
                },
                {
                    opcode: 'photoData',
                    blockType: Scratch.BlockType.REPORTER,
                    text: '照片（数据 URL）',
                    arguments: {}
                },
                '---',
                {
                    opcode: 'boardStatus',
                    blockType: Scratch.BlockType.REPORTER,
                    text: '连接状态',
                    arguments: {}
                },
                {
                    opcode: 'boardConnected',
                    blockType: Scratch.BlockType.BOOLEAN,
                    text: '已连接板子?',
                    arguments: {}
                },
                '---',
                {
                    opcode: 'startService',
                    blockType: Scratch.BlockType.COMMAND,
                    text: '启动本地服务（自动拉起）',
                    arguments: {}
                },
                {
                    opcode: 'launcherStatus',
                    blockType: Scratch.BlockType.REPORTER,
                    text: '本地服务状态',
                    arguments: {}
                }
            ],
            menus: {
                digitalPins: { acceptReporters: true, items: DIGITAL_PINS },
                analogPins: { acceptReporters: true, items: ANALOG_PINS },
                onOff: { acceptReporters: true, items: ['0', '1'] },
                backlightState: { acceptReporters: true, items: ['开', '关'] },
                cameraSizes: { acceptReporters: true, items: CAMERA_SIZES }
            }
        };
    };

    /* ------------------------- 通信 ------------------------- */

    Esp32S3.prototype.ensureSocket = function () {
        var self = this;
        if (this.socket && (this.socket.readyState === 0 || this.socket.readyState === 1)) {
            return;
        }
        this.connected = false;
        this.socket = new WebSocket(WS_URL);

        this.socket.onopen = function () {
            self.connected = true;
            self.launching = false;
            self.stopWatching();
            if (self.launchTimer) {         // 连上了就不用去拉服务了
                clearTimeout(self.launchTimer);
                self.launchTimer = null;
            }
            // 板子/网关重启后，固件里的引脚模式已经丢了，不能继续复用旧缓存
            self.pinModes = {};
            if (self.statusState === 'starting' && self.pendingConnectIp) {
                // 服务是刚被"点积木"拉起来的, 队列里那条 ip_address 马上会发出去,
                // 所以状态要回到"正在连接 <ip>", 而不是"已连上本地服务"
                self.setStatus('connecting', self.pendingConnectIp);
            } else if (self.statusState !== 'connecting') {
                // 正在连板子时不要把状态改回"已连本地服务"
                self.setStatus('local');
            }
            self.pendingConnectIp = '';
            self.send({ id: BANYAN_ID });      // 告诉 ws 网关: 这个客户端要跟 esp32 网关通信
            self.flushQueue();
        };

        this.socket.onclose = function () {
            self.connected = false;
            // 本地服务断了: 板子那边会自己停流 (发不出去就收工), 这边把状态对上
            self.videoOn = false;
            self.videoBusy = false;
            self.videoErrors = 0;
            self.videoLastError = '';
            self.videoLastArrivalAt = 0;
            self.videoLastDrawAt = 0;
            self.videoAliveAt = 0;
            // 正在等启动器拉起服务时, 别把"正在启动"盖成"未连接"
            if (self.launching) {
                self.setStatus('starting');
                return;
            }
            self.setStatus('offline');
        };

        this.socket.onerror = function () { /* 交给 onclose 统一处理 */ };

        this.socket.onmessage = function (event) {
            var msg;
            try {
                msg = JSON.parse(event.data);
            } catch (e) {
                return;
            }
            var report = msg['report'];
            // 调试用: 记一下"到底收到过板子/网关的上报没有" (见 video_diag)
            self.reportsReceived = (self.reportsReceived || 0) + 1;
            self.lastReport = report || '';
            self.lastReportAt = Date.now();
            if (report === 'board_status') {
                // 守护进程上报的网关/板子连接状态
                self.applyBoardStatus(msg);
                return;
            }
            if (report === 'digital_input') {
                self.digitalInputs[parseInt(msg['pin'], 10)] = parseInt(msg['value'], 10);
            } else if (report === 'analog_input') {
                self.analogInputs[parseInt(msg['pin'], 10)] = parseInt(msg['value'], 10);
            } else if (report === 'sonar_data') {
                self.sonarDistances[parseInt(msg['pin'], 10)] = parseInt(msg['value'], 10);
            } else if (report === 'audio_input') {
                // 麦克风响度 (0~100), 来自固件上报 0x0D
                self.micLevelValue = parseInt(msg['value'], 10) || 0;
            } else if (report === 'camera_info') {
                // 摄像头状态 (固件上报 0x12)
                var info = {
                    width: parseInt(msg['width'], 10) || 0,
                    height: parseInt(msg['height'], 10) || 0,
                    quality: msg['quality'],
                    sizeName: msg['size_name'],
                    sizeIndex: msg['size_index'],
                    xclkMhz: msg['xclk_mhz'],
                    state: msg['state']
                };
                self.cameraInfoValue = info;
                self.cameraInfoAt = Date.now();
                self.noteCamera(info.width + 'x' + info.height + '（' + info.sizeName +
                                '） 质量 ' + info.quality + ' XCLK ' + info.xclkMhz + 'MHz');
            } else if (report === 'camera_status') {
                // 拍照进度 (固件上报 0x11): 0=空闲 1=开始拍 2=出错
                if (msg['state_name'] === 'error') {
                    self.noteCamera('摄像头出错（连续失败 ' + msg['value'] + ' 次），看串口日志');
                }
            } else if (report === 'camera_frame') {
                // 一整帧 JPEG (网关把 0x10 帧头 + 0x0F 分片拼好并 base64 了)
                if (self.videoOn) {
                    self.handleVideoFrame(msg);
                } else if (self.photoDeferred) {
                    // 只有"我们主动在等一张照片"时才当成照片
                    self.handlePhoto(msg);
                } else if (Date.now() - (self.strayFrameAt || 0) > 5000) {
                    /*
                     * 既没在放视频、也没在等照片 —— 说明板子那头还挂着一条没停干净的
                     * 连续流 (别人开的 / camera_stop 没送到 / 上次按停止时没发出去)。
                     *
                     * 这里必须丢掉这些帧: 以前会走 handlePhoto, 每帧编码成 PNG 再挂一个
                     * 「照片 N」造型 —— 实测 10 分钟就攒出 934 个造型, 编辑器直接卡到
                     * 画面不动, 一按停止反而恢复 (2026-09-25 晚真实故障)。
                     * 顺手让板子把那边的流停掉, 5 秒最多发一次。
                     */
                    self.strayFrameAt = Date.now();
                    self.noteCamera('收到没人要的视频帧（板子上还有流没停），已让它停流');
                    self.send({ command: 'camera_stop' }, true);
                }
            }
            // 有板子数据回来 = 整条链路 (Scratch→网关→板子→回传) 是通的
            if (self.statusState === 'connecting') {
                self.setStatus('connected', self.ipAddress, self.statusFirmware);
            }
        };
    };

    /* ------------------------- 按需启动 (点积木 -> 拉起本机服务) ------------------------- */

    Esp32S3.prototype.socketOpen = function () {
        return !!(this.socket && this.socket.readyState === 1);
    };

    // 点积木时调用: 本地服务 (wsgw:9007) 没在跑就请本机启动器把它拉起来。
    // 先等 AUTOSTART_DELAY_MS: 服务本来就在跑的话, WebSocket 这段时间已经连上了,
    // 就不用去打扰启动器 (也避免状态闪一下"正在启动")。
    // force = true 只用来跳过冷却时间 (「启动本地服务」/「连接板子 IP」积木)。
    // 返回 true 表示本地服务已经连着。
    Esp32S3.prototype.ensureService = function (force) {
        if (!AUTOSTART_ENABLED) {
            this.launcherState = 'disabled';
            return false;
        }
        if (this.socketOpen()) {
            return true;
        }
        if (this.launching) {          // 已经有一次拉起在路上
            return false;
        }
        if (typeof fetch !== 'function') {
            return false;
        }
        var now = Date.now();
        if (!force && (now - this.lastLaunchAt) < AUTOSTART_COOLDOWN_MS) {
            return false;              // 刚请求过, 别再连点
        }
        if (force) {
            this.lastLaunchAt = 0;     // 手动点 = 不受冷却限制
        }
        var self = this;
        if (this.launchTimer) {
            return false;              // 已经排好队了
        }
        this.launchTimer = setTimeout(function () {
            self.launchTimer = null;
            if (!self.socketOpen()) {
                self.requestService();
            }
        }, AUTOSTART_DELAY_MS);
        return false;
    };

    // 真的去请启动器拉起服务, 并在这段时间里反复重连 WebSocket
    Esp32S3.prototype.requestService = function () {
        if (this.socketOpen() || this.launching || typeof fetch !== 'function') {
            return;
        }
        this.lastLaunchAt = Date.now();
        this.launching = true;
        this.launcherState = 'starting';
        this.waitDeadline = this.lastLaunchAt + AUTOSTART_GIVEUP_MS;
        this.setStatus('starting');
        this.watchForService();

        var ports = this.launcherPort ? [this.launcherPort] : LAUNCHER_PORTS;
        this.tryLauncher(ports, 0);
    };

    // 依次试 LAUNCHER_PORTS, 谁先应答就用谁 (比如用户的 8000 被别的服务占了)
    Esp32S3.prototype.tryLauncher = function (ports, index) {
        var self = this;
        if (index >= ports.length) {
            self.launching = false;
            self.launcherState = 'missing';
            self.setStatus(self.socketOpen() ? 'local' : 'offline');
            return;
        }
        var port = ports[index];
        fetch(LAUNCHER_HOST + ':' + port + '/start', {
            method: 'POST',
            mode: 'cors',
            cache: 'no-store'
        }).then(function (response) {
            if (!response.ok) {
                // 启动器回了错误 (最常见: 403 来源没放行), 把它的说明一起带出来,
                // 免得状态积木只会说"启动器没在运行", 让人查错方向。
                var status = response.status;
                var readBody = (typeof response.text === 'function')
                    ? response.text().catch(function () { return ''; })
                    : Promise.resolve('');
                return readBody.then(function (body) {
                    var detail = '';
                    try {
                        var parsed = JSON.parse(body);
                        if (parsed && parsed.error) { detail = String(parsed.error); }
                    } catch (ignored) { /* 不是 JSON 就算了 */ }
                    var err = new Error('HTTP ' + status + (detail ? '：' + detail : ''));
                    err.onegpioHttpStatus = status;
                    throw err;
                });
            }
            self.launcherError = '';
            self.launcherHttpStatus = 0;
            return response.json();
        }).then(function (info) {
            self.launcherPort = port;
            self.launcherState = 'ready';
            self.launching = false;
            self.launchMessage = (info && info.message) ? String(info.message) : '';
            if (info && info.ok === false) {
                // 启动器应答了, 但服务没起来: 别让状态一直挂在"正在启动"
                self.stopWatching();
                self.setStatus('error', self.ipAddress, '', self.launchMessage);
                return;
            }
            self.ensureSocket();       // 服务起来了, 立刻连 9007
        }).catch(function (err) {
            if (err && err.onegpioHttpStatus) {
                self.launcherHttpStatus = err.onegpioHttpStatus;
                self.launcherError = err.message || ('HTTP ' + err.onegpioHttpStatus);
            }
            self.tryLauncher(ports, index + 1);
        });
    };

    // 拉起服务的十几秒里, 每 1.5 秒重连一次 WebSocket, 连上 / 超时就停
    Esp32S3.prototype.watchForService = function () {
        var self = this;
        if (this.pollTimer) {
            return;
        }
        this.pollTimer = setInterval(function () {
            if (self.socketOpen()) {
                self.stopWatching();
                return;
            }
            if (Date.now() > self.waitDeadline) {
                self.stopWatching();
                if (!self.socketOpen()) {
                    self.setStatus('offline');
                }
                return;
            }
            self.ensureSocket();
        }, AUTOSTART_POLL_MS);
    };

    Esp32S3.prototype.stopWatching = function () {
        if (this.pollTimer) {
            clearInterval(this.pollTimer);
            this.pollTimer = null;
        }
    };

    /* ------------------------- 连接状态 ------------------------- */

    Esp32S3.prototype.setStatus = function (state, address, firmware, message) {
        var previous = this.statusState;
        this.statusState = state;
        this.statusAddress = address || '';
        this.statusFirmware = firmware || '';
        this.statusMessage = message || '';
        this.statusText = formatStatus(this);

        if (previous === 'connected' && state !== 'connected') {
            // 板子掉线/重启后引脚模式要重新设置一遍，不能继续复用旧缓存
            this.pinModes = {};
        }
        if (state === 'connected' && previous !== 'connected') {
            // 板子连上了，把排队等着的积木指令补发出去
            this.flushQueue();
        }
        return this.statusText;
    };

    // 处理守护进程发来的 board_status
    Esp32S3.prototype.applyBoardStatus = function (msg) {
        this.sawBoardStatus = true;
        var state = msg['state'];
        if (state === 'connected') {
            // IP 自动发现: 没点过「连接板子 IP」积木时, 用网关上来的地址当目标,
            // 这样不手填 IP 也能直接发指令 (后台守护进程会自动发现板子并发地址)。
            if (!this.ipAddress && msg['address']) {
                this.ipAddress = String(msg['address']).trim();
            }
            this.setStatus('connected', msg['address'], msg['firmware']);
        } else if (state === 'disconnected') {
            this.setStatus('disconnected', msg['address'], '', msg['message']);
        } else if (state === 'error') {
            this.setStatus('error', msg['address'], '', msg['message']);
        } else if (this.statusState === 'connecting' && this.ipAddress) {
            // 刚点完 IP 积木还没确认: 保留"正在连接", 别急着说没连上
            this.setStatus('connecting', this.ipAddress);
        } else {
            this.setStatus('waiting', '', '', msg['message']);
        }
    };

    Esp32S3.prototype.boardStatus = function () {
        return this.statusText;
    };

    Esp32S3.prototype.boardConnected = function () {
        return this.statusState === 'connected';
    };

    /* ------------------------- 发送队列 ------------------------- */

    // 排队, 超过上限就丢掉最旧的
    Esp32S3.prototype.queue = function (obj) {
        if (this.queued.length >= QUEUE_MAX) {
            this.queued.shift();
        }
        this.queued.push(obj);
    };

    // 把队列里的报文按顺序发出去 (只在 socket 可用时调用)
    Esp32S3.prototype.flushQueue = function () {
        if (!this.socket || this.socket.readyState !== 1 || this.queued.length === 0) {
            return;
        }
        var pending = this.queued.slice();
        this.queued = [];
        for (var i = 0; i < pending.length; i++) {
            this.socket.send(JSON.stringify(pending[i]));
        }
    };

    Esp32S3.prototype.send = function (obj, requireIp) {
        this.ensureSocket();
        if (!this.connected) {
            // 本地服务没在跑: 顺手请启动器把它拉起来 (连上后队列会自动补发)
            this.ensureService();
            // 未连上 / 未给 IP 时先排队，保证 ip_address 是第一条发出去的报文
            this.queue(obj);
            return;
        }
        if (requireIp && !this.ipAddress) {
            this.queue(obj);
            return;
        }
        // 板子还没连上时也先排队: 网关刚重启时收到积木指令会自己退出,
        // 而且板子没连上时这些指令本来也到不了。等状态变成"已连接"再补发。
        if (requireIp && this.sawBoardStatus && this.statusState !== 'connected') {
            this.queue(obj);
            return;
        }
        this.socket.send(JSON.stringify(obj));
    };

    /* ------------------------- 积木 ------------------------- */

    Esp32S3.prototype.connect = function (args) {
        var ip = String(args.IP).trim();
        this.ipAddress = ip;
        this.pendingConnectIp = ip;      // 服务没在跑时, 起来后要接着显示"正在连接 <ip>"
        // 新连接一律让第一块功能积木重新发送 set_mode_xxx
        this.pinModes = {};
        // 板子重启后麦克风采集是关的, 上一轮的响度读数不能留着
        this.micLevelValue = 0;
        // 这块积木本身没有任何回馈, 所以立刻把状态改成"正在连接"
        this.setStatus('connecting', ip);

        if (this.socket && this.socket.readyState === 1) {
            this.socket.send(JSON.stringify({ command: 'ip_address', address: ip }));
            this.queued = [];
            this.pendingConnectIp = '';
            return;
        }
        // 连接还没建立: 让 queued 的第一条就是这个 IP 命令
        this.queued = [{ command: 'ip_address', address: ip }];
        this.ensureSocket();
        // 本地服务没在跑时, 这块积木也负责把它拉起来
        this.ensureService(true);
    };

    // 「启动本地服务」积木: 手动触发一次按需启动
    Esp32S3.prototype.startService = function () {
        if (this.socketOpen()) {
            this.setStatus(this.statusState === 'connecting' ? 'connecting' : 'local', this.ipAddress);
            return;
        }
        this.lastLaunchAt = 0;      // 手动点 = 立刻拉, 不受冷却限制
        this.ensureService(true);
    };

    // 「本地服务状态」积木: 把启动器/服务的情况说清楚
    Esp32S3.prototype.launcherStatus = function () {
        if (this.socketOpen()) {
            return '本地服务已连接（ws://127.0.0.1:9007）';
        }
        if (this.launching || this.statusState === 'starting') {
            var left = Math.max(0, Math.ceil((this.waitDeadline - Date.now()) / 1000));
            return '正在启动本地服务…（最多再等 ' + left + ' 秒）';
        }
        if (!AUTOSTART_ENABLED) {
            return '自动拉起已关闭，请手工运行 tools\\start_s3extend.ps1';
        }
        if (this.launcherState === 'missing') {
            if (this.launcherError) {
                return '启动器拒绝了自动拉起（' + this.launcherError + '）：' +
                       '浏览器打开 http://127.0.0.1:8000/ 可以手动启动服务';
            }
            return '启动器未运行（先跑一次 tools\\start_launcher.ps1）';
        }
        if (this.launcherState === 'ready') {
            return '启动器在线，服务未运行（点积木会自动拉起）';
        }
        return '未检查（点积木时会自动拉起）';
    };

    Esp32S3.prototype.pin = function (value) {
        return parseInt(value, 10);
    };

    Esp32S3.prototype.digitalWrite = function (args) {
        var pin = this.pin(args.PIN);
        var value = parseInt(args.VALUE, 10) ? 1 : 0;
        if (this.pinModes[pin] !== AT_OUTPUT) {
            this.pinModes[pin] = AT_OUTPUT;
            this.send({ command: 'set_mode_digital_output', pin: pin }, true);
        }
        this.send({ command: 'digital_write', pin: pin, value: value }, true);
    };

    Esp32S3.prototype.pwmWrite = function (args) {
        var pin = this.pin(args.PIN);
        var percent = parseInt(args.VALUE, 10);
        if (percent < 0) { percent = 0; }
        if (percent > 100) { percent = 100; }
        var value = Math.round(percent * 255 / 100);   // 与上游一致: 8bit PWM
        if (this.pinModes[pin] !== 'pwm') {
            this.pinModes[pin] = 'pwm';
            this.send({ command: 'set_mode_pwm', pin: pin }, true);
        }
        this.send({ command: 'pwm_write', pin: pin, value: value }, true);
    };

    Esp32S3.prototype.servoWrite = function (args) {
        var pin = this.pin(args.PIN);
        var angle = parseInt(args.ANGLE, 10);
        if (this.pinModes[pin] !== 'servo') {
            this.pinModes[pin] = 'servo';
            this.send({ command: 'set_mode_servo', pin: pin }, true);
        }
        this.send({ command: 'servo_position', pin: pin, position: angle }, true);
    };

    /* 屏幕积木: 背光开关 (0x70) + 整屏颜色 (0x71), 与固件 tmx_protocol.h 对应 */
    Esp32S3.prototype.screenBacklight = function (args) {
        // 菜单给的是 开/关; 如果被塞了报告积木(0/1/false), 也按"关"处理
        var v = args.STATE;
        var off = (v === '关') || (v === 0) || (v === '0') || (v === false) || (v === 'false');
        this.send({ command: 'lcd_backlight', value: off ? 0 : 100 }, true);
    };

    Esp32S3.prototype.screenColor = function (args) {
        // Scratch 的颜色选择器给的是 "#rrggbb" (也可能是简写 "#rgb")
        var hex = String(args.COLOR === undefined ? '' : args.COLOR).trim().replace(/^#/, '');
        if (hex.length === 3) {
            hex = hex[0] + hex[0] + hex[1] + hex[1] + hex[2] + hex[2];
        }
        if (!/^[0-9a-fA-F]{6}$/.test(hex)) {
            hex = '000000';
        }
        this.send({
            command: 'lcd_color',
            red: parseInt(hex.substr(0, 2), 16),
            green: parseInt(hex.substr(2, 2), 16),
            blue: parseInt(hex.substr(4, 2), 16)
        }, true);
    };

    /* 音频积木: 音调 (0x72) / 停止 (0x73) / 麦克风 (0x74), 上报 0x0D */
    Esp32S3.prototype.audioTone = function (args) {
        var freq = parseInt(args.FREQ, 10);
        if (!isFinite(freq)) { freq = 440; }
        if (freq < 20) { freq = 20; }
        if (freq > 20000) { freq = 20000; }

        var ms = parseInt(args.MS, 10);
        if (!isFinite(ms)) { ms = 500; }
        if (ms < 1) { ms = 1; }
        if (ms > 60000) { ms = 60000; }

        var vol = parseInt(args.VOL, 10);
        if (!isFinite(vol)) { vol = 60; }
        if (vol < 0) { vol = 0; }
        if (vol > 100) { vol = 100; }

        this.send({
            command: 'audio_tone',
            frequency: freq,
            duration: ms,
            volume: vol
        }, true);
    };

    Esp32S3.prototype.audioStop = function () {
        this.send({ command: 'audio_stop' }, true);
    };

    Esp32S3.prototype.micLevel = function () {
        return this.micLevelValue;
    };

    Esp32S3.prototype.micDetect = function (args) {
        // 菜单给的是 开/关; 被塞了报告积木(0/1/false)时也认得
        var v = args.STATE;
        var off = (v === '关') || (v === 0) || (v === '0') || (v === false) || (v === 'false');
        if (off) {
            this.micLevelValue = 0;
        }
        this.send({ command: 'audio_mic', value: off ? 0 : 1 }, true);
    };

    /* 朗读文字: 板子上的 esp-tts 把中文合成语音放出来 (0x75 / 0x76) */
    Esp32S3.prototype.speakText = function (args) {
        var text = String(args.TEXT === undefined ? '' : args.TEXT).trim();
        if (!text) {
            return;
        }
        // UTF-8 拆包和字节序由网关负责 (Python 那边处理字符串更稳)
        this.send({ command: 'audio_tts', text: text }, true);
    };

    Esp32S3.prototype.stopSpeaking = function () {
        this.send({ command: 'audio_tts_stop' }, true);
    };

    /* 摄像头积木: OV2640 拍照 (命令 0x78~0x7B, 上报 0x0F~0x12) */

    Esp32S3.prototype.cameraSize = function (args) {
        var index = CAMERA_SIZES.indexOf(String(args.SIZE === undefined ? '' : args.SIZE).toUpperCase());
        if (index < 0) {
            index = 1;                    // 认不出来就退回 VGA
        }
        this.send({ command: 'camera_config', size: index }, true);
        /*
         * 2026-09-25 起固件已经能在线换分辨率了: tmx_camera_set_format() 发现尺寸
         * 真的变了, 会自己把采集通路重建一遍 (deinit -> 重新 init + 预热),
         * 新尺寸从下一帧就生效 —— 实测在线 SXGA/QVGA/VGA 都立刻跟着变。
         * 所以这里只需要发命令; 之前那版"停流再重开"的绕法已经不需要了
         * (旧固件才有这个问题: 只调 sensor->set_framesize() 出图尺寸不会变)。
         */
    };

    Esp32S3.prototype.cameraQuality = function (args) {
        var quality = parseInt(args.QUALITY, 10);
        if (!isFinite(quality)) {
            quality = 20;
        }
        if (quality < 0) { quality = 0; }
        if (quality > 63) { quality = 63; }
        this.send({ command: 'camera_config', quality: quality }, true);
    };

    // 「视频质量」: 只在流式播放期间生效; 帧越小帧率越高 (数字大 = 帧小)
    Esp32S3.prototype.streamQualityBlock = function (args) {
        var quality = parseInt(args.QUALITY, 10);
        if (!isFinite(quality)) {
            quality = CAMERA_STREAM_DEFAULT_QUALITY;
        }
        if (quality < 0) { quality = 0; }
        if (quality > 63) { quality = 63; }
        this.streamQuality = quality;
        if (this.videoOn) {
            // 正在放视频就立刻换过去, 不用重开
            this.send({ command: 'camera_config', quality: quality }, true);
        }
    };

    /*
     * 「打开摄像头」: 让板子连续出图 (0x79 帧数=0), 每一帧原地刷到当前角色的
     * 「摄像头画面」造型上, 就是舞台上看到的实时画面。
     *
     * 打开要一次上电 + 预热 (约 3 秒), 之后的帧才是连续的 —— 所以这块积木
     * 不等, 开完立刻往下走; 画面过几秒就会出现。
     * 用「关闭摄像头」停, 或者点编辑器的停止按钮 (会自动停)。
     */
    Esp32S3.prototype.openVideo = function () {
        /*
         * 已经开着的时候再点一次 = **重开**: 卡住时就是靠这个动作救回来的。
         * 所以这里绝对不能 return (以前那版一 return, 卡住后点它完全没反应,
         * 只能先去点「关闭摄像头」)。
         *
         * 但"重开"必须只在**真的卡住**时才做: 有人把这块积木放在 forever 循环里
         * (实测每秒上千次), 如果每次都重发 camera_config + camera_snapshot, 命令
         * 洪水会把整条链路堵死 —— 表现就是"一按运行画面就不动, 按停止反而恢复"。
         * 判断标准: 最近 2 秒内还有帧进来, 就认为画面是活的, 直接返回不发命令。
         */
        if (this.videoOn && this.videoLastArrivalAt &&
                (Date.now() - this.videoLastArrivalAt) < 2000) {
            return;
        }
        var restarting = this.videoOn;
        this.wantVideo = true;              // 关流前一直想放: 服务断了会自动重连重开
        this.videoOn = true;
        this.videoBusy = false;
        this.videoErrors = 0;
        this.videoLastError = '';
        this.videoFrames = 0;
        this.videoDropped = 0;
        this.videoFps = 0;
        this.videoFpsCount = 0;
        this.videoFpsAt = Date.now();
        this.videoLastArrivalAt = 0;
        this.videoLastDrawAt = 0;
        this.videoAliveAt = Date.now();     // 从现在开始算"还没有画面"
        /* 视频用更小的帧 (帧率能翻倍), 关的时候把拍照质量还回去。
         * 重开时别覆盖 qualityBeforeStream —— 那时它记的已经是「视频质量」了,
         * 覆盖掉的话关流时就会把视频质量当成原来的拍照质量还回去。 */
        if (!restarting) {
            this.qualityBeforeStream = this.cameraInfoValue &&
                                       this.cameraInfoValue.quality !== undefined
                ? this.cameraInfoValue.quality : -1;
        }
        this.send({ command: 'camera_config', quality: this.streamQuality }, true);
        this.noteCamera(restarting
            ? '正在重开摄像头…（板子上电 + 预热，约 3 秒）'
            : '正在打开摄像头…（板子上电 + 预热，约 3 秒）');
        this.send({ command: 'camera_snapshot', frames: 0, interval: CAMERA_STREAM_INTERVAL_MS },
                  true);
    };

    Esp32S3.prototype.closeVideo = function () {
        if (!this.videoOn) {
            return;
        }
        this.videoOn = false;
        this.wantVideo = false;             // 用户明确关了, 别再自动重开
        this.videoBusy = false;
        this.videoErrors = 0;
        this.videoLastError = '';
        this.videoLastArrivalAt = 0;
        this.videoLastDrawAt = 0;
        this.videoAliveAt = 0;
        this.send({ command: 'camera_stop' }, true);
        if (this.qualityBeforeStream >= 0) {
            // 流期间借用了质量设置, 还回去 (下次"拍一张照片"还是原来的清晰度)
            this.send({ command: 'camera_config', quality: this.qualityBeforeStream }, true);
            this.qualityBeforeStream = -1;
        }
        this.noteCamera('摄像头已关闭');
        this.refreshVideoAsset();
    };

    // 把「摄像头画面」造型的资源刷成最后一帧 (不然存档里存的是打开时的第一帧)
    Esp32S3.prototype.refreshVideoAsset = function () {
        var vm = Scratch.vm;
        var canvas = this.videoLastCanvas;
        if (!vm || !vm.runtime || !vm.runtime.storage || !canvas) {
            return;
        }
        var costume = findCostume(vm.editingTarget, VIDEO_COSTUME_NAME);
        if (!costume) {
            return;
        }
        canvasToPngBytes(canvas).then(function (bytes) {
            var storage = vm.runtime.storage;
            costume.asset = storage.createAsset(storage.AssetType.ImageBitmap,
                                                storage.DataFormat.PNG, bytes, null, true);
            costume.dataFormat = storage.DataFormat.PNG;
            costume.assetId = costume.asset.assetId;
            costume.md5 = costume.assetId + '.' + costume.dataFormat;
        }).catch(function () { /* 存档时大不了还是第一帧, 不值得打扰用户 */ });
    };

    /*
     * 「拍一张照片」: 变成一个新造型, 而且会等造型挂好了才往下走。
     *
     * 摄像头正开着的时候直接拿当前这一帧 (瞬时, 不用再拍一次);
     * 关着的时候让板子单独拍一张 —— 那要重新上电 + 预热, 约 3 秒。
     */
    Esp32S3.prototype.takePhoto = function () {
        var self = this;
        if (this.videoOn && this.videoLastFrame) {
            var frame = this.videoLastFrame;
            this.photoDataUrl = 'data:image/jpeg;base64,' + frame['data'];
            this.photoCount++;
            var shotName = '照片 ' + this.photoCount;
            return addPhotoCostume(frame, frame['data'], shotName).then(function (note) {
                self.noteCamera(note);
            }).catch(function (err) {
                self.noteCamera('截图失败：' + (err && err.message ? err.message : err));
            });
        }
        var deferred = makeDeferred();
        this.photoDeferred = deferred;
        this.noteCamera('正在拍照…（要等板子上电 + 预热，约 3 秒）');
        this.send({ command: 'camera_snapshot', frames: 1, interval: 0 }, true);
        return Promise.race([
            deferred.promise,
            new Promise(function (resolve) {
                setTimeout(function () {
                    if (self.photoDeferred === deferred) {
                        self.photoDeferred = null;
                        self.noteCamera('拍照超时：' + (CAMERA_PHOTO_TIMEOUT_MS / 1000) +
                                        ' 秒没收到照片（看「连接状态」和串口日志）');
                    }
                    resolve();
                }, CAMERA_PHOTO_TIMEOUT_MS);
            })
        ]);
    };

    // 往「摄像头状态」上写一句最近发生的事
    Esp32S3.prototype.noteCamera = function (text) {
        this.cameraNote = text;
        this.cameraNoteAt = Date.now();
    };

    /*
     * 「摄像头状态」: 显示最近发生的一件事 —— 拍照结果 / 摄像头出错 / 分辨率。
     * 报告积木被读取的频率很高 (每帧都在问), 所以顺手刷新的查询做了 2 秒节流:
     * 拍照刚有结果时不会被"640x480..."立刻顶掉, 过两秒没新消息才会刷新成状态。
     */
    Esp32S3.prototype.cameraState = function () {
        var stale = (Date.now() - this.cameraNoteAt) > CAMERA_INFO_REFRESH_MS;
        // 流式播放时状态文字由 fps 每秒刷新, 不用再去问板子
        if (this.socketOpen() && !this.photoDeferred && !this.videoOn && stale) {
            this.cameraNoteAt = Date.now();      // 先记上, 免得连发一串查询
            this.send({ command: 'camera_info' }, true);
        }
        return this.cameraNote;
    };

    Esp32S3.prototype.photoData = function () {
        return this.photoDataUrl;
    };

    // 收到一整帧: 存下 data URL, 变成当前角色的新造型, 再放行「拍照」积木
    Esp32S3.prototype.handlePhoto = function (msg) {
        var self = this;
        var base64 = msg['data'];
        if (typeof base64 !== 'string' || !base64) {
            return;
        }
        this.photoDataUrl = 'data:image/jpeg;base64,' + base64;
        this.photoCount++;
        var name = '照片 ' + this.photoCount;
        var waiting = this.photoDeferred;
        this.photoDeferred = null;

        addPhotoCostume(msg, base64, name).then(function (note) {
            self.noteCamera(note);
        }).catch(function (err) {
            self.noteCamera('照片拿到了，但变成造型失败：' +
                            (err && err.message ? err.message : err));
        }).then(function () {
            if (waiting) {
                waiting.resolve();       // 造型挂好了, 让积木继续往下走
            }
        });
    };

    // 流式播放: 一帧到了就原地换贴图。上一帧还没画完就直接丢掉新帧 ——
    // 宁可掉帧也不要排队积压 (积压会让画面越来越滞后, 像卡带一样)。
    Esp32S3.prototype.handleVideoFrame = function (msg) {
        var self = this;
        if (!this.videoOn) {
            return;
        }
        this.videoLastFrame = msg;
        this.videoLastArrivalAt = Date.now();
        this.videoAliveAt = Date.now();
        if (this.videoBusy) {
            this.videoDropped++;
            return;
        }
        this.videoBusy = true;
        renderVideoFrame(msg).then(function (result) {
            if (!self.videoOn) {
                return;                  // 画的途中被关掉了
            }
            self.videoCostume = result.costume;
            self.videoLastCanvas = result.canvas;
            self.videoErrors = 0;              // 画成功一帧就把"连续失败"清零
            self.videoLastDrawAt = Date.now();
            if (result.switched) {
                // 之前角色显示的是别的造型 (最常见的是复制出来的「摄像头画面2」),
                // 那种情况下画面看着永远是静止的
                self.noteCamera('已切回「' + VIDEO_COSTUME_NAME +
                                '」造型（原来显示的是别的造型）');
            }
            self.videoFrames++;
            self.videoFpsCount++;
            var now = Date.now();
            var elapsed = now - self.videoFpsAt;
            if (elapsed >= 1000) {
                self.videoFps = Math.round(self.videoFpsCount * 1000 / elapsed);
                self.videoFpsCount = 0;
                self.videoFpsAt = now;
                var note = '视频中：' + self.videoFps + ' 帧/秒' +
                           (self.videoDropped ? '（丢掉 ' + self.videoDropped + ' 帧）' : '');
                if (self.videoFps < 6) {
                    // 帧率就是"板子到电脑的带宽 ÷ 每帧字节数", 想快只能让帧变小
                    note += '（想更流畅：尺寸换 QVGA，或把视频质量调大）';
                }
                self.noteCamera(note);
                self.videoDropped = 0;
            }
        }).catch(function (err) {
            /*
             * 单帧画不出来只丢这一帧 —— 网络抖一下、偶发一帧解不开, 都不该
             * 把整条流停掉 (板子那边可不会知道, 会一直出图发热)。
             * 连续 CAMERA_VIDEO_MAX_ERRORS 帧都失败, 才当成真的显示不了。
             */
            var message = err && err.message ? err.message : String(err);
            self.videoErrors = (self.videoErrors || 0) + 1;
            self.videoLastError = message;
            if (self.videoErrors >= CAMERA_VIDEO_MAX_ERRORS) {
                self.videoOn = false;
                self.noteCamera('视频显示失败（连续 ' + self.videoErrors + ' 帧）：' + message +
                                '，再点一次「打开摄像头」可以重开');
            } else {
                self.noteCamera('这一帧没画出来（' + message + '），继续（已连续 ' +
                                self.videoErrors + ' 次）');
            }
        }).then(function () {
            self.videoBusy = false;
        });
    };

    /*
     * 流式播放看门狗。
     *
     * 画帧是"上一帧没画完就把新帧丢掉"(见 handleVideoFrame), 靠的是每帧的 promise
     * 最后把 videoBusy 清掉。可 decode/render 里有个别路径 (比如某个回调一直不回来)
     * 会让那个 promise 永远不 settle —— 于是 videoBusy 永远是 true, 之后每一帧都被
     * 丢掉: 画面定格, 板子却还在出图。这个看门狗就是兜这个底的。
     *
     * 只在"帧还在来、但很久没画成功"时才动手; 帧本身不来(链路断了)不管 —— 那是
     * 另一回事, 重画也画不出新画面。
     */
    Esp32S3.prototype.videoWatchdog = function () {
        /*
         * 先处理"本地服务断了": 只要这块积木没被关掉 (wantVideo), 就自己重连并重新
         * 发一遍 camera_config / camera_snapshot —— 服务重启 (守护进程重启 wsgw /
         * esp32gw) 之后画面能自己回来, 不用再去点积木。
         * 3 秒一次, 免得重连不上时把待发队列撑爆。
         */
        if (this.wantVideo && !this.socketOpen()) {
            if (Date.now() - (this.lastReconnectAt || 0) > 3000) {
                this.lastReconnectAt = Date.now();
                this.noteCamera('本地服务断了，正在重连并重开摄像头…');
                this.openVideo();
            }
            return;
        }
        /*
         * "服务连着、但板子那头已经没有流了": 板子换电源 / 被复位 / 重启之后会自己
         * 回到空闲状态, 而扩展这边 wantVideo 还是 true —— 以前这种情况画面就一直
         * 定格, 必须人工再点一次「打开摄像头」(2026-09-26 换电池供电时真踩到:
         * 板子重启后 IP 也从 .100 变成 .103)。这里补上自动重开: 8 秒没有画面就
         * 重发一遍命令; 10 秒最多一次, 因为板子上电 + 预热本身要 3 秒, 重发太密
         * 反而会把它打断。
         */
        if (this.wantVideo && this.socketOpen() && this.videoAliveAt &&
                (Date.now() - this.videoAliveAt) > 8000 &&
                (Date.now() - (this.lastReviveAt || 0)) > 10000) {
            this.lastReviveAt = Date.now();
            this.noteCamera('好一会儿没有画面了（板子重启过？），正在重新开流…');
            this.openVideo();
            return;
        }
        if (!this.videoOn || !this.videoLastFrame) {
            return;
        }
        var now = Date.now();
        if (!this.videoLastArrivalAt) {
            return;                 // 一帧都还没到 (板子上电 + 预热那几秒)
        }
        if (now - this.videoLastArrivalAt > CAMERA_VIDEO_STALL_MS) {
            return;                 // 帧也不来了: 链路/板子的问题, 不在这里装活
        }
        if (this.videoLastDrawAt && now - this.videoLastDrawAt < CAMERA_VIDEO_STALL_MS) {
            return;                 // 画得好好的
        }
        // 卡住了: 不再等那一帧, 把手里最新的一帧补画上去
        this.videoBusy = false;
        this.videoRecoveries++;
        this.videoLastDrawAt = now;
        this.noteCamera('画面卡住了，正在自动重画（第 ' + this.videoRecoveries + ' 次）');
        this.handleVideoFrame(this.videoLastFrame);
    };

    /*
     * 调试上报 (排障用, 用完请把 VIDEO_DIAG_ENABLED 改回 false)。
     *
     * 每 5 秒往 Banyan 总线 (topic `to_esp32_gateway`) 发一条
     * {"command": "video_diag", ...} —— 网关对不认识的命令是直接忽略的, 不会去
     * 打扰板子; 而 PC 侧用它就能看到扩展内部状态, 不用盯着 Scratch 界面:
     *
     *     python tools\sniff_camera_stream.py --diag --seconds 20
     *
     * 里面的 current_costume / costume 两项专门用来回答"扩展画出来的造型, 是不是
     * 舞台上正在显示的那个"。
     */
    var VIDEO_DIAG_ENABLED = false;     // 排障用, 平时关掉 (每 5 秒一条上报)
    var VIDEO_DIAG_INTERVAL_MS = 5000;

    Esp32S3.prototype.videoDiag = function () {
        if (!this.connected) {
            return;                 // 没连上就别发, 免得把待发队列撑大
        }
        var vm = Scratch.vm;
        var target = vm && vm.editingTarget;
        var sprite = target && target.sprite;
        var names = [];
        var videoCostume = null;
        var currentName = null;
        if (sprite && sprite.costumes) {
            for (var i = 0; i < sprite.costumes.length; i++) {
                var costume = sprite.costumes[i];
                if (!costume) {
                    continue;
                }
                names.push(costume.name);
                if (costume.name === VIDEO_COSTUME_NAME) {
                    videoCostume = costume;
                }
                if (target.currentCostume === i) {
                    currentName = costume.name;
                }
            }
        }
        var now = Date.now();
        this.send({
            command: 'video_diag',
            on: this.videoOn,
            busy: this.videoBusy,
            frames: this.videoFrames,
            dropped: this.videoDropped,
            errors: this.videoErrors,
            last_error: this.videoLastError,
            recoveries: this.videoRecoveries,
            fps: this.videoFps,
            since_arrival_ms: this.videoLastArrivalAt ? now - this.videoLastArrivalAt : -1,
            since_draw_ms: this.videoLastDrawAt ? now - this.videoLastDrawAt : -1,
            reports: this.reportsReceived || 0,
            last_report: this.lastReport || '',
            last_report_ms: this.lastReportAt ? now - this.lastReportAt : -1,
            photo_count: this.photoCount,
            costume: videoCostume ? videoCostume.name : null,
            costume_skin: videoCostume ? videoCostume.skinId : null,
            current_costume: currentName,
            costumes: names,
            sprite: sprite ? sprite.name : null,
            visible: sprite ? sprite.visible : null,
            size: sprite ? sprite.size : null,
            connected: this.connected,
            status: this.statusState
        }, false);
    };

    Esp32S3.prototype.digitalRead = function (args) {
        var pin = this.pin(args.PIN);
        if (this.pinModes[pin] !== AT_INPUT_PULLUP) {
            this.pinModes[pin] = AT_INPUT_PULLUP;
            this.send({ command: 'set_mode_digital_input_pullup', pin: pin }, true);
        }
        return this.digitalInputs[pin] === undefined ? 0 : this.digitalInputs[pin];
    };

    Esp32S3.prototype.analogRead = function (args) {
        var pin = this.pin(args.PIN);
        if (this.pinModes[pin] !== AT_ANALOG) {
            this.pinModes[pin] = AT_ANALOG;
            this.send({ command: 'set_mode_analog_input', pin: pin }, true);
        }
        return this.analogInputs[pin] === undefined ? 0 : this.analogInputs[pin];
    };

    Esp32S3.prototype.sonarRead = function (args) {
        var trig = this.pin(args.TRIG);
        var echo = this.pin(args.ECHO);
        if (this.pinModes[trig] !== 'sonar' || this.lastSonarTrigger !== trig) {
            this.pinModes[trig] = 'sonar';
            this.lastSonarTrigger = trig;
            this.send({ command: 'set_mode_sonar', trigger_pin: trig, echo_pin: echo }, true);
        }
        return this.sonarDistances[trig] === undefined ? 0 : this.sonarDistances[trig];
    };

    var extensionInstance = new Esp32S3();

    /*
     * 每秒让看门狗看一眼流式播放有没有卡住。
     * node 里跑测试时 unref() 一下, 免得这个定时器把测试进程拖住不退出。
     */
    function startVideoWatchdog(ext) {
        var timer = setInterval(function () {
            try {
                ext.videoWatchdog();
            } catch (ignored) {
                /* 看门狗自己出问题不能把扩展带崩 */
            }
        }, 1000);
        if (timer && typeof timer.unref === 'function') {
            timer.unref();
        }
        return timer;
    }

    startVideoWatchdog(extensionInstance);

    // 调试上报定时器 (VIDEO_DIAG_ENABLED = false 时不会启动)
    function startVideoDiag(ext) {
        if (!VIDEO_DIAG_ENABLED) {
            return null;
        }
        var timer = setInterval(function () {
            try {
                ext.videoDiag();
            } catch (ignored) {
                /* 调试上报自己不能把扩展带崩 */
            }
        }, VIDEO_DIAG_INTERVAL_MS);
        if (timer && typeof timer.unref === 'function') {
            timer.unref();
        }
        return timer;
    }

    startVideoDiag(extensionInstance);

    /*
     * 点编辑器的停止按钮时, 顺手把板子的流也停掉 —— 指令停了画面还在放、
     * 板子还在发热, 是个很容易忘的坑。拿不到 runtime 就算了 (沙箱环境)。
     */
    try {
        var runtime = Scratch.vm && Scratch.vm.runtime;
        if (runtime && typeof runtime.on === 'function') {
            runtime.on('PROJECT_STOP_ALL', function () {
                extensionInstance.closeVideo();
            });
        }
    } catch (ignored) {
        /* 没有 vm 的编辑器: 靠「关闭摄像头」积木自己停 */
    }

    Scratch.extensions.register(extensionInstance);
})(Scratch);
