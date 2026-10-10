# 里程碑候选：可校准的 V → SA Portal Camera（2026-10-10）

本阶段在已有 SA → GTA V GPU 画面链上增加相机几何与开发传送能力。代码和合成测试已经通过，但尚未完成双游戏内验收。

## 已实现

- PortalV 通过 `VState.v2` 发布最终渲染相机位置、旋转和 FOV，而不只发布 gameplay camera。
- PortalV 的 TP 落脚点与 `PortalVPlane.txt` 门面拆分。默认使用用户确认的 Grove Street `(85.336830, -1922.733000, 21.681070)`、heading `-134.562100°`；F10 可保存当前人物/车辆位置。Story Mode 玩家稳定两秒后自动 TP；移动期间冻结玩家或当前车辆，请求碰撞、加载场景并探测地面。Online session 会被拒绝。
- PortalDev 提供手动 V TP、SA 出口 X/Y/Z/Yaw、抓取当前 SA 人物位置、保存校准和启用相机映射。
- PortalDev 将 V 相机相对于 V 门的局部位置和方向映射到 SA 门坐标系，并以最新值方式发送给 PortalSA。
- PortalSA 用 fixed camera 接管主相机；命令关闭或超过 500 ms 未刷新时用 jump cut 恢复。
- SA 前台按 F8 写入 `SA_CALIBRATION_MARKER`，保留之后确定 SA TP 点所需的精确人物位置、朝向和相机数据。

## 当前边界

- SA 使用主相机，不是离屏第二相机；启用时 SA 被当作 portal renderer。
- fixed camera 每帧强制 FOV 70，协议中的目标 FOV 已预留但本阶段不强行覆盖投影。
- `PortalCalibration.txt` 是 dev 游戏旁的本地校准数据，不写入 `game/clean`。
- 当前门映射采用两个水平 portal basis 和对称透视；off-axis projection 属于后续里程碑。
- 构建通过不能证明 TP 落点、SA streaming、左右视差方向或恢复行为正确，必须在 Story Mode 和 SA gameplay 中实测。

## 游戏内验收顺序

1. 部署最新 PortalV/PortalSA，启动 PortalDev。
2. 进入 GTA V Story Mode，确认日志出现自动 TP 排队和成功结果；需要时用手动 TP 重试。
3. 手动控制 CJ 到 SA 葛洛夫街指定位置，确保已经离开安全屋并完成室外加载，然后按 F8。
4. 在 PortalDev 点“取当前 SA 人物位置”，调整 SA 出口 Z/Yaw 并保存。
5. 启用 V → SA Portal 相机，分别从 V 门正中、左侧、右侧和远近位置观察 SA 画面。
6. 关闭开关、退出 PortalDev以及制造超过 500 ms 的状态中断，分别确认 SA 相机能恢复。

本轮静态验证命令：

```powershell
& '.\src\Build-Dev.ps1' -RunTests
```

结果：PortalSA、PortalV、PortalDev Release 构建通过；`SAFramePublisherSmoke` 和 `D3D11D3D12BridgeSmoke` 通过。
