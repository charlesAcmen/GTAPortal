# 里程碑候选：葛洛夫街重叠与 Lock In 校准（2026-10-12）

本阶段把“两个葛洛夫街尽量重叠”设为相机和门面几何的共同目标。Release 构建、ReShade shader 编译与五项 smoke test 已通过；实际街景重叠、主相机同步稳定性和 ImGui 操作仍需双游戏内验收。

## 相机契约

- GTA V 不再强制第一人称；玩家可自由切换视角。跨游戏映射始终读取实际 final rendered camera，因此第三人称时也使用相机位置而不是人物位置。
- 跨游戏映射继续以 GTA V 的 final rendered camera 位置、旋转和 FOV 为输入，而不是玩家坐标。玩家坐标只用于 TP。
- GTA V native FOV 按垂直 FOV 传递；PortalSA 按固定 16:9 门画面转换成 SA 横向 FOV，并同步写入主相机 `CCam` 和 `CDraw`。
- 启用映射时，PortalSA 通过 `SetCamPositionForFixedMode` 与 `TakeControlNoEntity` 直接接管 SA 主相机；禁用或命令超时 500 ms 后通过 `RestoreWithJumpCut` 恢复正常控制。

当前不注册也不构建实验性的第二相机。SA 的正常窗口就是门户相机输出，PortalDev 始终通过 WGC 捕获该窗口并发布到既有 `SAFrames` GPU 通道。这条链路牺牲了同步启用期间的 SA 本地第三人称游玩画面，但删除了离屏相机、RenderWare current-camera 状态以及 WGC/离屏源切换带来的不稳定因素。

## F9 校准与持久化

- GTA V 前台按 F9：在当前人物脚下按人物朝向放置并立即保存直径 8 m 的正圆门；圆底低于地面 0.6 m，随后打开 ReShade overlay 中的 `GTAPortal Calibration`。
- Center、Yaw、宽高、形状、边缘柔化以及 source crop 都实时生效，便于看着传输画面微调；默认保持宽高相等。
- F9 的基础摆放立即写入 `PortalVPlane.txt`；后续 overlay 微调仍需 `Lock In` 保存，`Cancel` 恢复 F9 基础摆放或上一次锁定值。
- SA 前台 F9 同样在 CJ 脚下按 CJ 朝向放置直径 8 m 的正圆门，圆底低于地面 0.6 m，写入 `PortalSAPlane.txt`；同时通知 PortalDev 更新并保存 SA 相机映射锚点，因此同步画面立即移动到新门位。
- 两侧都把 16:9 来源等比放大后居中裁掉左右区域，得到正方形取景后再做正圆裁剪，不改变画面比例。
- PortalDev 的 SA X/Y/Z/Yaw 输入在相机映射启用时实时生效；“保存 SA 出口参数”才写入 `PortalCalibration.txt`。
- PortalDev 默认不再绘制 SA/V 实时预览，也不创建预览 swap chain；两路后台画面传输继续运行。窗口仅保留日常部署、日志、TP 和校准按钮。
- PortalDev 可分别控制“显示 GTA V 中的 SA 门”和“显示 SA 中的 GTA V 门”。默认只显示 GTA V 一侧，避免镜中镜递归。

默认 SA 地面锚使用最新保存值 `(2468.084229, -1660.620117, 13.311936)`，yaw 为 `-99.351112°`；默认 GTA V 地面锚为 `(88.110458, -1922.158081, 20.698630)`，yaw 为 `-141.490860°`。门中心统一为地面加 `3.4 m`。已有本地保存文件会自动迁移为新版格式并继续优先于默认值。

## 游戏内验收

1. 构建并部署两个 ASI 与最新 `PortalSA.fx`，启动 PortalDev 和两个游戏。
2. 在 GTA V 自由切换第一/第三人称；PortalDev 中 final rendered camera 应随实际视角变化，且不会强制切回第一人称。
3. 控制 GTA V 人物站到目标点并朝向目标方向，前台按 F9；确认正圆门立刻落在脚下、底部略微入地、画面比例正常且 ReShade overlay 打开。需要时微调并 Lock In。
4. 启用 Portal 相机，确认 SA 日志出现 `Portal main camera sync active`，PortalDev 状态显示 `SA 主相机同步中`。首次启用时 SA 相机应从 SA 门中心开始，CJ 不出现在同步画面中。关闭相机映射后，SA 应恢复正常跟随 CJ 并恢复 CJ 显示。
5. 从 V 门正中、左右、远近观察同一道路边线和房屋角点。用 PortalDev 的 SA 出口输入做实时微调，保存后重启三端，确认两个锁定文件均恢复。
6. 分别切换两侧 portal 显示复选框，确认可以独立隐藏；控制 CJ 站到目标点并朝向目标方向，SA 前台按 F9，确认正圆门落在脚下、PortalDev 的 SA 出口数值同步变化，并检查圆边缘和场景深度遮挡。

本轮静态验证命令：

```powershell
& '.\src\Build-Dev.ps1' -RunTests
```

结果：PortalSA、PortalV、PortalDev Release 构建通过；`SAFramePublisherSmoke`、`D3D11D3D12BridgeSmoke`、`VToSACpuBridgeSmoke` 与新增 `SAOffscreenCpuBridgeSmoke` 通过。以上结果不替代双游戏内视觉验收。
