# 第三方组件的本地补丁

`managed_components/` 被 .gitignore 排除（组件管理器会重新下载），所以这里放一份
动过手脚的文件，重新拉组件以后照这里改回去即可。

## esp32-camera / `driver/cam_hal.c`（必须打，否则摄像头一帧都拿不到）

完整改好的文件见同目录的 `cam_hal.c.local`。相对上游有两处改动，都在
`cam_take()` 里 "Check for JPEG SOI in the first buffer" 那段（DRAM 分支）：

1. **不要因为"SOI 不在第 0 字节"就丢帧停止采集。**
   上游代码在 `soi_off != 0` 时执行：

   ```c
   ll_cam_stop(cam_obj);
   cam_obj->state = CAM_STATE_IDLE;
   continue;
   ```

   本板每次开始连拍的头一两帧常常是从半个帧开始的，于是上游逻辑会让每一帧都被丢掉，
   永远拿不到图（串口一直刷 `cam_hal: NO-SOI - JPEG start marker missing`）。
   本地改成只告警、不丢帧（用 `#if 0` 包住上面三行），交给上层
   `tmx_camera.c` 的 `jpeg_align_frame()` 在整帧里找 `FF D8 FF` … `FF D9` 裁出
   干净的一段。

2. **（可选，调试用）** 在同一个分支里加一小段把这一帧开头 16 字节打到串口的代码
   （`cam_hal: DBG-SOI len=... / DBG-HDR ...`，只打前 5 次）。用来判断传感器到底
   在输出什么。不需要可以整段删掉，不影响功能。

改完以后重新编译烧录即可（`tools\idf.ps1 -Port COMx build flash`）。

> 另一个相关的运行期补丁在工程自己的代码里（不受组件更新影响）：
> `firmware/main/tmx_camera.c` 的 `jpeg_align_frame()`、`frame_header_ok()`
> 和开机自检逻辑，见 docs/camera-debug-notes.md。
