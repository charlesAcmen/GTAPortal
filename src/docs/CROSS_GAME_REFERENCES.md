# 跨游戏实例与本项目画面链路（2026-10-09）

`reference/` 是只读参考源码，使用 Git submodule 固定提交；这些项目不会参与 GTAPortal 构建，也不能把作者报告的效果当作本机验证。更新主仓库后用 `git submodule update --init --recursive` 取得它们。阅读时先看指定入口，不要运行参考项目的安装脚本或把其运行库复制进游戏。

| 实例（固定提交） | 建议先读 | 可以借鉴 | 不能直接推断 |
| --- | --- | --- | --- |
| [universal-modder](../../reference/universal-modder) `8370faa8e114baf33acdb23079aff552a7728c4b` | `examples/minecraft-gta5-passthrough/README.md`、`gta/src/compositor.cpp`、`gta/shaders/MCPassthrough.fx` | Legacy 版 GTA V 的 ScriptHookV 状态通道、三槽画面协议、ReShade 颜色与深度合成、相机时序 | 它的颜色/深度帧经过 CPU 共享内存和 GPU 上传；不是我们的 GPU 共享纹理实现，也不证明 Enhanced 兼容 |
| [Gta-V-Enhanced-Minecraft-Steve-Passthrough](../../reference/Gta-V-Enhanced-Minecraft-Steve-Passthrough) `2363ee0169f3983108ff78b8dd3c464ebac96c33` | `README.md`、`source/gta/src/compositor.cpp`、`source/gta/shaders/MCPassthrough.fx` | 作者报告的 Enhanced 1.0.1158.13 双进程画面与 GTA 深度合成、部署方法 | 这是作者的配置和运行报告，不是本机 SA→V 的验收；实现仍需按我们的纹理协议改造 |
| [minecraft-in-gta-enhanced](../../reference/minecraft-in-gta-enhanced) `981f14ab109d7a8f57b533d4c7ea09a9a36d4255` | `docs/ARCHITECTURE.md`、`gta/src/compositor.cpp`、`gta/shaders/MCPassthrough.fx` | 作者记录了 Enhanced 1.0.1158.16 加载集成；可研究渲染门控、深度比较和双游戏启动管理 | 源码注明派生自 universal-modder，不是独立证实 GPU 零 CPU 传帧或原生物体材质接入 |
| [SkyCraft](../../reference/SkyCraft) `bfcaf178524b92c2cdeb88e4ce0f13ef9ded6f32` | `README.md`、`docs/DESIGN.md`、`skse/src/WorldRender.cpp` | Skyrim＋Minecraft 的状态、碰撞、渲染职责划分和另一种 GPU 画面链路 | 宿主是 Skyrim，不是 GTA V；其玩法融合不能直接证明我们的小窗或传送门性能 |

## SA 的画面现在怎样到达工具和 GTA V

```text
GTA SA 自己渲染（D3D9）
        ↓
SA 游戏窗口
        ↓ Windows Graphics Capture，从窗口取得 GPU 画面
PortalDev 的 D3D11 纹理
        ├──→ GPU 内复制并显示 → PortalDev 预览窗
        └──→ 三槽命名共享纹理 → GTA V ReShade 小窗候选实现
```

所以说 **SA 画面生产者已经可用** 是对的：你确实在工具中看到了实时 SA 画面。这里的“生产”指 SA 正常渲染、Windows 捕获并交给 PortalDev。当前没有把整帧复制到 CPU 再传送。

跨进程发布现在也有独立 SA 通道。`CaptureGameFrame()` 使用 `GpuCapture::Publication::Sa`，V 窗口捕获使用 `Publication::V`。两者都发布三槽命名 D3D11 纹理；`PortalV.asi` 的 ReShade add-on 读取 SA 通道并复制到 D3D12 显示纹理。独立 GPU 测试已通过；完整游戏内显示和深度遮挡仍待实测，细节见 [SA → V 里程碑](MILESTONE_SA_TO_V_2026-10-09.md)。

`D3D9` 是 SA 使用的旧版图形接口。早先尝试 `D3D9Ex`，目的是让 **SA 游戏进程自己** 导出共享纹理；这个实验导致启动故障，相关开关保持关闭。当前成功的预览和新增共享发布通过 Windows 的窗口捕获取得画面，**不依赖**那个实验。

参考实例的 ReShade 合成提供了世界平面、深度比较与部署方式的思路。我们已有对应的候选代码；SA→V 的实机性能和遮挡仍需验证。
