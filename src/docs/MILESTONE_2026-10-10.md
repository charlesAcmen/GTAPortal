# 里程碑：双向 GPU 画面桥接候选完成（2026-10-10）

本阶段把 GTA San Andreas 与 GTA V Enhanced 之间的两条画面路径整理成独立、可构建的 GPU 管线，并完成了 SA → GTA V 的首次游戏内显示。这里严格区分“代码和合成测试通过”“游戏内看到了画面”和“最终视觉验收完成”；三者不是同一件事。

## 本阶段已经完成

| 项目 | 当前结果 | 验证状态 |
| --- | --- | --- |
| SA → GTA V 生产与消费 | PortalDev 捕获 SA 窗口的最新 GPU 帧，发布到三个带 keyed mutex 的命名 D3D11 共享纹理；PortalV 通过 D3D11 → D3D12 GPU 桥复制到 GTA V 可采样纹理，ReShade 在世界空间平面上显示 | 用户已在 GTA V 中启用 `PortalSA` 技术并看到实时 SA 画面；当次画面仍带 Windows 窗口边框 |
| SA 客户区裁剪 | PortalDev 在 GPU 内用 `CopySubresourceRegion` 只复制客户区，不做整帧 CPU 回读；坐标查询改为物理像素，并按 WGC 纹理尺寸映射，状态栏会显示裁剪尺寸和偏移 | Release 构建通过；最新 DPI/坐标修复尚待游戏内复验，不能把边框问题标记为已关闭 |
| GTA V → SA 候选管线 | PortalDev 用三个 staging 纹理异步读回，写入动态尺寸的三槽 CPU 最新帧邮箱；PortalSA 将最新帧上传到自己的三槽 D3D9 纹理并绘制深度测试世界矩形 | Release 构建和像素完整性 smoke test 通过；尚未完成 GTA SA 游戏内画面、遮挡、延迟和稳定性验收 |
| 三槽最新帧协议 | 生产者循环使用三个槽；消费者读取最新 `Ready` 帧；允许覆盖未读取旧帧，但不覆盖 `Reading` 槽。资源重建使用进程级唯一 generation | `SAFramePublisherSmoke` 验证发布、覆盖和重建；generation 重用问题已修复 |
| GTA V 深度遮挡 | `PortalSA.fx` 采样 GTA V 本帧场景深度，并将门户平面深度与场景深度比较 | 使用原生场景深度，不硬编码人物、车辆或墙；真实场景中的遮挡边界仍需专项验收 |
| 开发工具整理 | 删除无人消费的通用 `VFrames` 通道；旧 SA 内部生产者和 D3D9Ex 整机替换代码迁入 `legacy/diagnostics`；构建脚本可选择运行三个 bridge smoke tests | Release 构建和三个 smoke tests 通过 |

## 当前正式架构

```text
GTA SA 客户区
  -> Windows Graphics Capture（GPU 纹理）
  -> GPU 客户区裁剪
  -> 3 槽命名 D3D11 共享纹理 + keyed mutex
  -> PortalV 的 D3D11/D3D12 GPU 桥
  -> ReShade 世界平面 + GTA V 原生场景深度

GTA V 客户区
  -> Windows Graphics Capture（GPU 纹理）
  -> GPU 客户区裁剪
  -> 3 槽 D3D11 staging 异步 DMA 读回
  -> 动态分辨率 3 槽 CPU 最新帧邮箱
  -> SA 3 槽本地上传纹理
  -> PortalSA 世界矩形（候选，待游戏内验收）
```

SA → V 方向的整帧像素不经过 CPU。V → SA 因普通 D3D9 不支持 D3D11 跨进程共享表面，整帧像素经过异步 GPU 读回、共享内存和 SA 本地上传；CPU 控制区还负责槽状态、帧号、时间戳和同步。三个槽是低延迟最新帧环形池：它避免生产者等待慢消费者，但不承诺每一帧都被消费。

## 放弃作为正式路径的方案

尝试把 SA 的原生 D3D9 设备整体替换为 D3D9Ex，以直接创建可跨进程共享的游戏内部纹理。设备创建成功后，RenderWare 后续在 `0x00800203` 访问空对象，表现为黑屏后崩溃。该方法改变了 2004 年游戏对设备对象的内部假设，因此只保留为显式诊断选项，不作为默认或推荐路径。

正式路径保留 SA 原有 D3D9/RenderWare 设备，只从系统窗口合成结果取得 GPU 帧。代价是存在 Windows Graphics Capture/桌面合成带来的约一到数帧延迟；GPU 客户区裁剪本身只是一次区域复制，不是主要延迟来源。

## 本次构建和自动验证

提交前执行：

```powershell
& '.\src\Build-Dev.ps1' -RunTests
```

要求通过：

- `PortalSA.asi`、`PortalV.asi`、`PortalDev.exe` Release 构建；
- `SAFramePublisherSmoke`；
- `D3D11D3D12BridgeSmoke`。

这些结果证明编译和隔离 GPU 传输，不替代双游戏运行验收。

## 下一轮验收

1. 重启最新 `PortalDev.exe`，确认 SA 状态显示 `GPU 客户区裁剪正常 <宽>x<高>（偏移 x,y）`，并确认 GTA V 中不再出现标题栏和窗口边框。
2. 固定 SA 分辨率和窗口缩放，验证最小化、恢复、移动到另一显示器、调整尺寸以及停止/重启捕获后 generation 切换。
3. 在 GTA V 中验证门户平面被墙、人物和车辆的原生场景深度正确遮挡，并记录异常边缘。
4. 完成 GTA V → SA 的首次游戏内显示、深度遮挡、停止生产者约 500 ms 后隐藏和设备重置测试。
5. 双游戏固定场景运行，记录游戏 FPS、捕获帧龄 P50/P95、覆盖/丢弃计数和 GPU 帧时间，再决定是否需要更低延迟的原生 D3D9 专项实验。
