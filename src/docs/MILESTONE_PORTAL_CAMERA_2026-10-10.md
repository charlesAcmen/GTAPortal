# 里程碑候选：可校准的 V → SA Portal Camera（2026-10-10）

本阶段在已有 SA → GTA V GPU 画面链上增加相机几何与开发传送能力。代码和合成测试已经通过，但尚未完成双游戏内验收。

## 已实现

- PortalV 通过 `VState.v3` 发布最终渲染相机位置、旋转、FOV 以及 F9 门锚点，而不只发布 gameplay camera。
- PortalV 的 TP 落脚点与 `PortalVPlane.txt` 门面拆分。默认使用用户确认的 Grove Street `(85.336830, -1922.733000, 21.681070)`、heading `-134.562100°`；F10 可保存当前人物/车辆位置。Story Mode 玩家稳定两秒后自动 TP；移动期间冻结玩家或当前车辆，请求碰撞、加载场景并探测地面。Online session 会被拒绝。
- PortalSA 在 gameplay 稳定两秒后自动将 CJ 或当前车辆 TP 一次到 `(2468.331787, -1661.175537, 13.321789)`、heading `-1.892188 rad`，并在移动前加载室外场景和碰撞。
- GTA V 前台按 F9 时，PortalV 从最终渲染相机的 forward 水平投影计算门中心与初始 yaw，不再把旧 Grove Street 门坐标硬编码进 PortalDev。`Shift+F9` / `Ctrl+F9` 可按 `-1°` / `+1°` 微调并保存 yaw。
- `VState.v3` 将 F9 保存的门中心、yaw 和有效位发布给 PortalDev；V → SA 相机映射直接采用该实时门 basis，在 F9 尚未建立门时不会接管 SA 相机。
- PortalDev 提供手动 V TP、SA 出口 X/Y/Z/Yaw、抓取当前 SA 人物位置、保存校准和启用相机映射。
- PortalDev 将 V 相机相对于 V 门的局部位置和方向映射到 SA 门坐标系，并以最新值方式发送给 PortalSA。
- PortalSA 用 fixed camera 接管主相机；命令关闭或超过 500 ms 未刷新时用 jump cut 恢复。
- SA 前台按 F8 写入 `SA_CALIBRATION_MARKER`，保留之后确定 SA TP 点所需的精确人物位置、朝向和相机数据。

## 已确认的葛洛夫街开发锚点

两组位置记录在 `src/config/GroveStreetAnchors.txt`，用途严格分开：

| 游戏 | 用途 | 位置 | 朝向 |
| --- | --- | --- | --- |
| GTA V Enhanced | 当前用户确认的 Grove Street 相机/临时 TP 锚点 | `(85.336830, -1922.733000, 21.681070)` | yaw `-134.562100°` |
| GTA SA | 最后一条 F8 标记；未来 CJ TP 落脚点 | `(2468.331787, -1661.175537, 13.321789)` | heading `-1.892188 rad` / `-108.414386°` |

GTA V 的门面中心继续由 F9 保存的 `PortalVPlane.txt` 决定，初始 yaw 来自相机 forward，之后可用组合键微调；SA 门面中心仍通过 PortalDev 微调。两者都不能替代上表中的人物/相机 TP 锚点。

## 当前边界

- SA 使用主相机，不是离屏第二相机；启用时 SA 被当作 portal renderer。
- fixed camera 每帧强制 FOV 70，协议中的目标 FOV 已预留但本阶段不强行覆盖投影。
- `PortalCalibration.txt` 是 dev 游戏旁的本地校准数据，不写入 `game/clean`。
- 当前门映射采用两个水平 portal basis 和对称透视；off-axis projection 属于后续里程碑。
- 构建通过不能证明 TP 落点、SA streaming、左右视差方向或恢复行为正确，必须在 Story Mode 和 SA gameplay 中实测。

## 游戏内验收顺序

1. 部署最新 PortalV/PortalSA，启动 PortalDev。
2. 进入 GTA V Story Mode，确认日志出现自动 TP 排队和成功结果；需要时用手动 TP 重试。
3. 进入 SA gameplay，确认 `portal.log` 出现 `SA automatic development teleport completed`，且 CJ/当前车辆落在指定位置和朝向；然后按 F8 记录复核标记。
4. 在 PortalDev 点“取当前 SA 人物位置”，调整 SA 出口 Z/Yaw 并保存。
5. 启用 V → SA Portal 相机，分别从 V 门正中、左侧、右侧和远近位置观察 SA 画面。
6. 关闭开关、退出 PortalDev以及制造超过 500 ms 的状态中断，分别确认 SA 相机能恢复。

本轮静态验证命令：

```powershell
& '.\src\Build-Dev.ps1' -RunTests
```

结果：PortalSA、PortalV、PortalDev Release 构建通过；`SAFramePublisherSmoke` 和 `D3D11D3D12BridgeSmoke` 通过。
