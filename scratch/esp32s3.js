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
                backlightState: { acceptReporters: true, items: ['开', '关'] }
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

    Scratch.extensions.register(new Esp32S3());
})(Scratch);
