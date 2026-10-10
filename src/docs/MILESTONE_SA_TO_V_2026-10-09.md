# SA → GTA V Enhanced：GPU 小窗里程碑工作记录

## 已实现的代码路径

```text
SA 游戏窗口
  → Windows Graphics Capture（PortalDev）
  → 三槽 D3D11 命名共享纹理；Ready 旧帧可覆盖，Reading 槽受保护
  → GTA V 的 PortalV.asi / ReShade add-on
  → D3D11 GPU CopyResource 到三个 D3D12 可共享显示纹理之一
  → GPU fence 等待复制完成；ReShade 着色器读取最新显示纹理
  → 世界固定的 16:9 平面；GTA 深度测试；画面过期后停用
```

生产者的三槽池不会排成长队。消费者取当前最大 frameId，复制完成后释放生产者槽；GTA 显示纹理的三个槽通过 fence 在 GPU 读完以后才能复用。Resize 和生产者代际变化会丢弃旧画面。跨 API 资源需要 `ALLOW_RENDER_TARGET | ALLOW_SIMULTANEOUS_ACCESS`，这是本机独立桥测试发现并验证的条件。

脚本读取最终渲染相机参数。F9 把小窗锚定在相机前约 3 米，并保存世界坐标。着色器仅在平面覆盖的像素读取 SA，缩小时使用四个双线性采样近似局部平均；它用 GTA 深度缓冲判断前后关系，默认按 reversed-Z 线性化。它不阻止 GTA 渲染窗后面的场景。

## 当前验证边界

- `PortalV.asi` 和 `PortalDev.exe` 的 Release x64 编译通过。
- SA 与 V 的共享发布测试通过：两条通道分别 `produced=12 overwritten=7`，并验证捕获重启后资源代际变化。
- 独立 D3D11→D3D12 桥测试通过，得到预期颜色 `FF2468AC`。测试末端的 16×16 CPU 读回仅用于断言；实际传输路径无全帧 CPU 读回。
- 固定版本 ReShade FX 编译器使用参考项目的 `ReShade.fxh`，成功为像素和顶点入口生成 DXBC。
- 代码首次编译时，本机 GTA V 目录还没有 ReShade 运行库。随后用户安装了 ReShade 6.8.0 并在故事模式运行，实机结果见下文；世界平面画面、深度遮挡及两游戏并行帧时间仍未完成视觉验收。

## 2026-10-09 晚间实机记录

用户确认以下最后一次 F9 写入的平面中心就在葛洛夫街，用作之后继续测试的**用户确认测试锚点**，不是游戏地图的通用地名坐标：

```text
PortalVPlane.txt: X=96.491821 Y=-1932.880859 Z=21.770508 yaw=-133.509491°
```

`PortalV.log` 多次记录 `PortalV SA plane anchored 3m in front of camera (F9)`，所以 F9 已收到并保存位置。`ReShade.log` 记录 `PortalSA.fx` 在游戏内编译成功（有一条 X4000 警告）、PortalV add-on 注册、D3D11/D3D12 桥初始化，以及截至退出前 `received=8100 skipped=764 frame=21135`。这证明生产帧已到达 GTA V 的 ReShade add-on；`skipped` 是实时最新帧策略下跳过的旧帧计数，不能单凭它推断画面卡顿。

当时 `ReShadePreset.ini` 仍为 0 字节；F9 只固定世界平面，不会启用 ReShade 技术。用户没有看到小窗，最直接的待检项是在 ReShade 界面启用 `PortalSA`。目前不能把“帧已接收”写成“画面及遮挡已验收”。

## 实机验收顺序

1. 安装与 Enhanced/D3D12 兼容、允许 add-on 的 ReShade，确认 `ReShade.fxh` 与运行库已加载。
2. 用 PortalDev 部署 ASI 和 `PortalSA.fx`，在 ReShade 开启 `PortalSA` 技术。
3. SA 与 GTA V 都运行，按 F9 在 Grove Street 锚定，检查站位、视角变化、物体遮挡及深度方向。
4. 让 SA 最小化、停止捕获、重启、改变窗口尺寸，观察画面是否按预期隐藏或恢复；若 WGC 对最小化窗口不再产帧，应显示过期停用而非旧画面。
5. 固定同一镜头测 SA 单独、V 单独和双开各 3 分钟，记录游戏 FPS/P95 帧时间、VRAM、发布覆盖数和消费跳帧数。再分别试 1080p、720p；不能从低分辨率直接推断 GTA V 帧率会明显提高。

下一阶段是否做真正 portal 几何、离屏视角或更复杂的遮挡，要由上述结果决定。
