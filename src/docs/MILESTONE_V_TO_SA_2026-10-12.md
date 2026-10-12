# 里程碑：GTA V 画面在 GTA SA 世界中显示（2026-10-12）

## 游戏内结果

用户在 GTA SA 中按 F9，确认此前只沿电线阴影和桥面露出的 GTA V 画面，在修正绘制状态后能够正常显示。至此，GTA V → SA 不再仅是通过编译与合成测试的候选管线，已经取得首次游戏内世界矩形显示验收。此次确认的是可见画面；墙、人物、车辆的遮挡边界，以及帧延迟、长时间运行和重置恢复仍需分别测量。

## 数据流与定位

```text
GTA V 可见窗口
  -> Windows Graphics Capture 和 GPU 客户区裁剪
  -> 三槽 D3D11 staging 异步读回
  -> 动态尺寸三槽 CPU 最新帧邮箱
  -> GTA SA 三槽 D3D9 本地纹理上传
  -> CJ 面朝方向前方约 3 m 的竖直世界矩形
  -> GTA SA 当前场景深度缓冲决定遮挡
```

邮箱仅传输 BGRA 颜色帧，不传 GTA V 的深度。矩形被 SA 场景遮挡时，应使用 SA 当前的深度缓冲；本里程碑不依赖 D3D9Ex 或跨 API 共享纹理。F9 以 CJ 的位置和朝向放置矩形，中心高于 CJ 位置 1.5 m，宽 2.4 m，高度按收到画面的纵横比计算。放置结果写入游戏旁的 `PortalSAPlane.txt`。早期“相机前方 3 m”会让矩形与第三人称视角中的 CJ 重合，已改为“人物前方 3 m”。

## 阴影形状残片的原因与修正

修复前的游戏内截图显示，画面只在电线阴影、桥面等不规则区域露出，无法看见完整的竖直矩形。`portal.log` 在 SA 矩形绘制入口记录到：

```text
V consumer scene depth: 999x514 format=75 viewport=(0,0,999,514,0.00,1.00) z_mode=0 z_func=8 stencil=1 clip_planes=0 depth_bias=0x00000000
```

也就是说，入口状态中 Z 测试关闭、Z 比较为始终通过、模板测试开启。直接继承这些状态会让矩形受阴影模板约束。PortalSA 现在对矩形显式启用场景 Z/W 深度测试、关闭模板和裁剪平面、清零深度偏移、禁止深度写入，并在绘制后恢复原有 D3D9 状态。没有绑定场景深度表面时跳过绘制并记录错误。首次绘制与每次 F9 重新放置后都会记录实际深度状态，便于继续核查遮挡问题。

## 验证边界

- 用户已确认修复后画面在 SA 中正常显示；这是实际游戏内视觉验收，不等同于遮挡、流畅度或稳定性全项验收。
- 2026-10-12 执行 `& '.\src\Build-Dev.ps1' -RunTests`：PortalSA、PortalV、PortalDev Release 构建均通过。
- `SAFramePublisherSmoke` 通过：`produced=12 overwritten=7`。
- `D3D11D3D12BridgeSmoke` 通过：颜色 `FF2468AC`。
- `VToSACpuBridgeSmoke` 通过：`37x19 produced=9 overwritten=5 dropped=0`，验证 CPU 邮箱的像素完整性和旧帧覆盖。
- 后续应固定双游戏场景，分别检查墙/人物/车辆遮挡、停止采集后约 500 ms 隐藏、分辨率变化、设备重置、帧龄 P50/P95 和两游戏 FPS。当前没有这些量化结果。
