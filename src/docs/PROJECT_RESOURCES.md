# GTAPortal：现有资源与跨游戏传送门分析

调查日期：2026-10-08（Asia/Shanghai）。本文件最初汇总本地只读检查、公开项目资料和设计判断；初始调查阶段没有编译、启动游戏、安装依赖或验证游戏内行为，后续构建状态见下方更新段落。

后续里程碑 1 已写入 `src/PortalSA/`：x86 ASI 项目、状态日志和定期最终画面 BMP；另有外部 `PortalDev.exe` 负责构建入口、dev 部署、启动、日志和抓帧状态。2026-10-08 两者已在本机成功编译，用户已在真实游戏中验收注入、相机/玩家日志、周期 BMP 和双屏窗口化工作流，M1 完成。plugin-sdk、gta-reversed 与开发运行依赖 III.VC.SA.WindowedMode 均登记为固定 commit 的 Git submodule。下文保留部分调查写作时的资源快照，涉及“尚无 PortalSA 源码”等叙述以本段更新为准。

## 1. 本次范围和结论

资源调查阶段只新增根目录 `.gitignore` 和本文档。原始 `src/docs/commands.txt` 保留不变，全文收录在附录 A；后续里程碑 1 又新增 `src/PortalSA/`、`src/PortalDev/` 和开发脚本。

当前有效要求优先于附录中的旧任务：里程碑 1 已由 Codex 编译部署，并由用户按 `src/PortalSA/README.md` 完成实机验收。

长期目标是 GTA V 与经典 PC 版 GTA SA 两个独立进程，通过双向实时透视传送门连接；后续人物跨门时，切换控制权，呈现两个时代之间的视觉过渡。

当前工程路线保持：先做 SA 端 ASI 读取探针，再做 Win32 共享内存、外部读取程序和相机覆盖实验。之后研究离屏渲染与跨进程画面。人物穿越和控制权切换不属于 V1。

主要判断：

- **现有 HOODLUM 1.0 US 适合作为当前固定开发基准，继续保留。** 它并不是所有工具唯一支持的版本，也没有证据证明它天然比 Compact 更适合渲染传送门。
- **plugin-sdk 足够支撑当前 SA 探针和共享内存阶段。** gta-reversed 继续作为源码参考；最终渲染桥接还需要项目自行实现。
- **Minecraft ↔ GTA V 已有公开的双进程传帧案例可研究。** 但网上的游戏混搭采用多种架构，不能一概归为同一种跨进程渲染方案。
- **难点集中在正确生成另一侧视角，以及稳定、低延迟地合成画面。** IPC 能传数据，并不意味着游戏已经能为传送门再渲染一次场景。

## 2. 本地已有资源

项目根目录：`D:\coding\repo\GTAPortal`。

| 资源 | 实际路径 | 本次确认与用途 |
| --- | --- | --- |
| 原始项目说明 | `src/docs/commands.txt` | 本次修改前唯一已有的 src 文件；保存环境、约束和阶段目标 |
| plugin-sdk | `third_party/plugin-sdk/plugin-sdk` | 注意有两层 plugin-sdk；SA 类、事件、RenderWare 定义与 hook 工具的主要依赖 |
| III.VC.SA.WindowedMode | `third_party/III.VC.SA.WindowedMode` | 可选开发运行依赖；解决双屏调试时独占全屏失焦最小化，不链接到 PortalSA |
| gta-reversed | `reference/gta-reversed` | 研究相机、渲染、流式加载等实现；不构建、不注入 |
| 游戏基准 | `game/clean/GTA San Andreas` | 保持基准环境，不部署开发产物 |
| 开发游戏 | `game/dev/GTA San Andreas` | 已存在；后续只往这里部署插件 |
| tools / vendor | `tools`、`vendor` | 本次未发现可用项目文件 |

资源调查时根目录尚不是 Git 仓库；现在根目录已有 Git 仓库，三个第三方 checkout 已登记为 submodule。资源调查时 `src` 中尚无 PortalSA；此后已添加源码、项目文件与用户可执行的编译命令，见 `src/PortalSA/README.md`。

### 2.1 固定第三方源码版本

这三个目录现在通过 `.gitmodules` 和 Git gitlink 固定版本；跨设备克隆时需使用 `git clone --recurse-submodules`，已有 clone 则执行 `git submodule update --init --recursive`。根目录 `README.md` 有完整说明。

| 依赖 | 上游 | 当前本地 HEAD |
| --- | --- | --- |
| plugin-sdk | [DK22Pac/plugin-sdk](https://github.com/DK22Pac/plugin-sdk) | `15f15b60bbf74c106e1b496ff92c98764abf4605`，2026-09-08 |
| III.VC.SA.WindowedMode | [ThirteenAG/III.VC.SA.WindowedMode](https://github.com/ThirteenAG/III.VC.SA.WindowedMode) | `f2f4e499a69019a47df35c77d354f8a1195db016`，tag `v2.2` |
| gta-reversed | [gta-reversed/gta-reversed](https://github.com/gta-reversed/gta-reversed) | `01709e8eefdf0b4871f33a8eab19dfa0716d509f`，2026-10-04 |

以下是资源调查阶段记录的**手动恢复方法**；现在优先使用上述 submodule 命令，且两种方法都不会下载游戏文件：

```powershell
Set-Location 'D:\coding\repo\GTAPortal'
git clone https://github.com/DK22Pac/plugin-sdk.git third_party/plugin-sdk/plugin-sdk
git -C third_party/plugin-sdk/plugin-sdk checkout 15f15b60bbf74c106e1b496ff92c98764abf4605
git clone https://github.com/gta-reversed/gta-reversed.git reference/gta-reversed
git -C reference/gta-reversed checkout 01709e8eefdf0b4871f33a8eab19dfa0716d509f
```

### 2.2 游戏 EXE 与插件

本次分别读取 clean、dev 的 `gta-sa.EXE` 并计算 MD5，两者均为：

```text
Size: 14,383,616 bytes
MD5: 170B3A9108687B26DA2D8901C6948A18
Target: GTA SA classic PC / 1.0 US HOODLUM / x86
```

这确认两份 **EXE** 一致，不代表本次比较了两个游戏目录中的所有文件。版本名称来自原始项目记录，并与 [sa-sdk 上游声明的目标指纹](https://github.com/TsyVM/sa-sdk#readme) 一致。

本次读取 dev 目录文件信息，确认：

- `vorbisFile.dll` 的描述是 Ultimate ASI Loader，产品名为 Ultimate-ASI-Loader-x86；已有加载器，不重复安装。
- `DINPUT8.DLL` 的描述是 Microsoft DirectInput；不要以另一个加载器覆盖它。
- 存在 `vorbisHooked.dll`、`III.VC.SA.LimitAdjuster.asi`、`SilentPatchSA.asi`、`wm_sachs.asi`。
- 三个 ASI 大小分别为 396800、282624、138752 bytes，与原始记录一致。

加载成功、正常游玩、DPI 问题已解决、中文显示与初始存档情况，属于用户在 commands.txt 中提供的已知环境。本次没有启动游戏重新验证；不修改这些资源。

## 3. HOODLUM 与 Compact：当前该选哪个

### 3.1 plugin-sdk 明确支持识别两种 1.0 US

本地源码证据：

- `shared/GameVersion.h` 定义 `GAME_10US_COMPACT = 1001`、`GAME_10US_HOODLUM = 1002`。
- `shared/GameVersion.cpp` 的 `detect_game_id()` 在 `GTASA` 分支分别识别两种 EXE。
- `IsGameVersion10us()` 接受两者。
- `by_v_dyn()` 同时提供区分两种版本的地址选择，以及两种版本共用 1.0 US 地址的形式。

可在固定版本的 [GameVersion.h](https://github.com/DK22Pac/plugin-sdk/blob/15f15b60bbf74c106e1b496ff92c98764abf4605/shared/GameVersion.h) 和 [GameVersion.cpp](https://github.com/DK22Pac/plugin-sdk/blob/15f15b60bbf74c106e1b496ff92c98764abf4605/shared/GameVersion.cpp) 查看对应代码。

**因此，“plugin-sdk 基于 HOODLUM 而不是 Compact”不准确。** 版本检测支持两者，也不等于每一个 hook 和地址都可以跨版本无条件复用。具体接口仍需逐项核对，尤其是跳转、代码字节和渲染阶段 hook。

### 3.2 gta-reversed 的可加载产物要求 Compact

本地 README 的“Set up the game and ASI”明确要求 Compact，大小为 **5,189,632 bytes**，并警告不符合要求可能随机崩溃。可核对 [当前本地 commit 的 README](https://github.com/gta-reversed/gta-reversed/blob/01709e8eefdf0b4871f33a8eab19dfa0716d509f/README.md)。

这与现有 HOODLUM EXE 的 14,383,616 bytes 不同。我们可以参考它的函数逻辑，但不能直接把它的产物、所有地址或安装步骤套用到当前游戏。

### 3.3 选型判断

| 使用情境 | 判断 |
| --- | --- |
| 现在：plugin-sdk 编写 SA ASI 探针 | 保持 HOODLUM；目标已经固定，现有环境可用，没有迁移必要 |
| 阅读 gta-reversed 理解算法和生命周期 | 保持 HOODLUM；阅读源码不要求更换 EXE |
| 将来决定实际加载 gta-reversed 的实现 | 按其 Compact 要求建立另一个实验环境；需先另行决定，不改变现有 clean/dev |
| 低延迟图像传输、GPU 共享纹理 | 主要受图形设备、渲染接口和同步机制制约；EXE 名称本身不能解决这些问题 |

**当前最合理的是保留 HOODLUM，而不是宣布它是普遍最优版本。** 不 downgrade、不替换 Compact、不修改 EXE；也不根据源码参考需求迁移整套环境。

## 4. 最近的游戏混搭实际采用哪些路线

以下结论来自作者仓库、技术记录和可读源码；“作者报告可用”与“本项目已验证”分开。没有给出具体视频链接的演示，不能仅凭画面确认其架构。

### 4.1 Minecraft ↔ GTA V：与我们最接近的公开例子

[Universal Modder 的 Minecraft × GTA V 示例](https://github.com/rehan-remade/universal-modder/tree/main/examples/minecraft-gta5-passthrough) 描述两个真实游戏同时运行：Minecraft 的 Fabric mod 与 GTA V 的 ScriptHookV ASI 交换相机和状态，ReShade add-on 接收画面并合成。作者报告的验证配置是 GTA V **Legacy** build 3889、ScriptHookV 3889.0、ReShade 6.8.0、Minecraft Java 26.3。[示例 README](https://github.com/rehan-remade/universal-modder/blob/main/examples/minecraft-gta5-passthrough/README.md)

需要区分两条数据通道：

- 相机、输入、事件等通过本地 WebSocket。
- 世界 color/depth 和 overlay 通过命名共享内存传输。

其 [compositor.cpp](https://github.com/rehan-remade/universal-modder/blob/main/examples/minecraft-gta5-passthrough/gta/src/compositor.cpp) 可读到 `MapViewOfFile`、帧槽描述、`create_resource()` 与 `update_texture_region()`。所以这个案例确实包含 **CPU 共享内存中的像素上传至接收端纹理**，不能把它说成必然使用跨进程 GPU 零拷贝。

作者的 [技术记录](https://github.com/rehan-remade/universal-modder/blob/main/knowledge/games/gta-v/minecraft-passthrough.md) 还讨论了 depth 合成、相机帧时序和重投影，明确没有验证 Enhanced 等其他配置。可借鉴架构与调试方法，但它没有证明 SA 的第二相机已经可用，也不是可直接改名使用的 SA ↔ V 双向传送门。

### 4.2 Minecraft ↔ Skyrim：双进程，但由宿主画世界内容

[SkyCraft](https://github.com/chasmlol/SkyCraft) 的 README 描述 Minecraft 在后台负责自身逻辑，SKSE 插件和 Fabric mod 通过共享内存连接，Skyrim 负责绘制。它说明“两个游戏同时运行”并不必然等于把另一游戏的完整 framebuffer 贴入宿主。

这类几何、逻辑与碰撞融合适合共同世界中的混搭。我们想保留两套完整城市、画风和 gameplay，更适合先研究相机画面桥接，不移植整个 SA 世界的资源和行为到 GTA V。

### 4.3 滑板混搭：不能全部归为跨进程传帧

[2010 Rust Rewrite Mashup](https://github.com/chasmlol/2010-rust-rewrite-mashup) 明确基于 IW4L 的 Rust MW2 重写，并加入 Skate 模式和 Minecraft 世界；这是重实现运行时融合的公开例子。

还有直接面向 SA 的 [GTA San AnSkateas](https://github.com/ryglizzy/GTA-San-AnSkateas)，其仓库注明使用 Skate 3 Rust engine、plugin-sdk 等资源。它是研究 SA 与外部游戏运行时接合的线索，不能据此认定它采用我们规划的双游戏 framebuffer 传送门。

本次没有确认某一条“Skate 3 在 GTA V 中”的具体视频与源码对应关系。上面的案例用于解释不同技术路线，不冒充对用户看到的所有视频逐一验证。

### 4.4 对我们意味着什么

“高度定制渲染管线”这个理解抓住了部分核心，但实际还有相机、时序、输入和游戏生命周期的适配。有的项目传图像，有的传几何和游戏状态，有的重写运行时再在同一进程融合。

对 GTAPortal 的设计建议是：**保留两个原游戏进程，先做 state IPC，再单向传 color frame，再做双向窗口，最后单独处理穿越和控制权。** 这是本项目的工程判断，不是从演示中推断它们一定都这样实现。

## 5. GTAPortal 的渲染问题

下面是候选设计与待验证问题，没有承诺某个游戏接口已可工作。

### 5.1 “能看见画面”与“正确透视窗口”是两个目标

固定拍摄 SA，再把画面贴在 GTA V 的矩形上，可以先证明传输，却会像监视器。传送门要求观察者移动时，另一侧视点和透视对应变化。

采用列向量、两个门的局部坐标定义一致、刚体变换且尺度相同的约定时，可把相机映射写成：

```text
C_B = P_B · R_turn · inverse(P_A) · C_A
```

`P_A/P_B` 是入口/出口局部到各自世界的变换，`C_A/C_B` 是 camera-to-world；`R_turn` 通常是门局部竖直轴上的半圈旋转，用于匹配面对面的门。真正实现前要确认法线、坐标手性、矩阵乘法约定和旋转方向，不能直接把公式中的轴名当成两个引擎的轴名。

此外还要处理 projection 和采样：

- **候选简单路线：** 映射相机，用匹配宿主视野的完整图像，通过宿主屏幕坐标/投影坐标在门区域采样。不能把完整视野任意压缩进矩形的普通 0..1 UV。
- **另一条路线：** 以映射眼点和出口门角点计算 off-axis frustum，只渲染门覆盖的视锥，再匹配门平面采样。

off-axis 方法可参考 [PsychoPy 的 generalizedPerspectiveProjection 官方实现说明](https://psychopy.org/api/tools/viewtools.html#psychopy.tools.viewtools.generalizedPerspectiveProjection)。它用眼点和屏幕角点构建投影；此处把门作为窗口平面是我们的应用设计。

初期不用完整跨门 depth，也仍需门区域裁切及宿主场景对门面的基本遮挡。这可先用受宿主深度测试的矩形 surface；不等于要求 V1 实现复杂 stencil、跨世界深度或递归。

### 5.2 第二相机是最早需要单独证明的渲染能力

覆盖主相机只能证明相机控制。最终两边都要保持可玩的主视图，同时输出对方请求的门视角；后台那一边也不能因失焦停止渲染。

SA 的 RenderWare 相机对象只是入口。额外渲染还可能涉及视锥裁剪、可见实体列表、模型 streaming、透明队列、阴影和后处理。只创建 `RwCamera` 或再次调用一个渲染函数，不保证场景正确，也不保证原画面不受影响。

值得阅读的现有 reference：

- `reference/gta-reversed/source/game_sa/Camera.cpp`
- `reference/gta-reversed/source/game_sa/Renderer.cpp`
- `reference/gta-reversed/source/game_sa/VisibilityPlugins.cpp`
- `reference/gta-reversed/source/game_sa/Streaming.cpp`
- `reference/gta-reversed/source/game_sa/Mirrors.cpp`

本地 `Mirrors.cpp` 包含 camera texture / z-buffer 创建、相机矩阵和额外渲染流程，可研究现有引擎怎样处理另一视角；它不是可直接复制的 portal renderer。第一轮可先选两个已加载的室外固定地点，减少 streaming 变量。

### 5.3 CPU 传帧先证明链路，GPU 共享留给性能阶段

最朴素的图像链路是：

```text
SA 请求的离屏视角 → GPU readback → CPU shared memory
                                      ↓
V 的纹理上传 → 门区域绘制

另一方向需要另一组相机请求和输出帧，不能把接收画面反复作为发送源。
```

共享内存省去网络和编码，却没有省掉 GPU→CPU readback、内存复制与 CPU→GPU upload。以单方向 RGBA8 为例，以下只是原始像素负载计算，不是实测性能：

| 分辨率 / 帧率 | 每帧字节 | 每秒原始像素负载 |
| --- | --- | --- |
| 512 × 512 / 30 fps | 1,048,576 | 约 31.5 MB/s |
| 1280 × 720 / 60 fps | 3,686,400 | 约 221.2 MB/s |
| 1920 × 1080 / 60 fps | 8,294,400 | 约 497.7 MB/s |

双向负载再乘二，多次复制会增加实际流量；同步 readback 导致的 GPU 等待往往比负载本身更需要测量。先以低分辨率 color-only 验证即可，不先增加 depth、压缩或重投影。

传统 D3D9 与 9Ex 必须分清。[Microsoft 的图形 API 共享说明](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/surface-sharing-between-windows-graphics-apis) 区分了传统 D3D9c 与 9Ex 的共享能力；不能因为系统有新版 DirectX，就认定 SA 使用了可共享的 9Ex device。当前运行时 device 类型仍未验证。

将来考虑共享 GPU surface 时，要先确定两端实际 API、adapter、格式和资源创建方式。[D3D11 OpenSharedResource 文档](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-opensharedresource) 对 D3D9↔11 纹理给出 mip、MSAA、格式和用途限制。共享 handle 也不自动完成生产/消费同步。**引入一个 SDK 不能自动把普通 D3D9 游戏改成 9Ex，也不能自动获得离屏场景。**

### 5.4 帧必须带上“它用哪个相机渲染”

以后 state/frame 协议应明确：magic/version、固定宽度字段、布局与大小、frame ID、相机请求 ID、渲染时的 pose/projection、时间戳、尺寸、像素格式、row pitch，以及完整帧的发布规则。

不共享进程内指针或引擎类对象；x86 与 x64 使用各自本地的 pointer/HANDLE，通过 Win32 规定的方式传输必要标识，不能裸拷贝本机结构。不要假设 x86 下普通 64-bit 读写天然原子。同步方案单独设计，先保证不会读到半帧。

双向渲染允许消费最近一张完整帧；避免双方在 render thread 上互相等待，造成停顿或死锁。记录端到端 frame age，相机和图像错帧会在快速转身、平移时显现。以上是设计约束，尚不冻结最终协议。

### 5.5 后续控制权交接不是 Alt-Tab

穿越至少涉及门平面的有方向跨越检测、出口放置、朝向与速度映射、相机连续性、输入归属、窗口/鼠标焦点、HUD/声音、后台世界运行，以及一次按键不会触发两边行为。

建议后续先实现“在出口操控另一个游戏自己的玩家”，例如 GTA V 主角切到 SA 的 CJ，保留各自角色和玩法。把同一个模型、装备和所有物理状态真实搬进另一个引擎，是额外项目范围。两种效果不同，开发交接阶段时再明确选择。

双向透视也不要求双方玩家都被同一个输入操控。当前被控制的一边提供主相机，另一边提供映射后的门视角；反向观察同理。不先做 NPC、子弹或车辆跨门。

## 6. third_party 是否够用，以及推荐顺序

| 资源 | 建议 | 解决什么 / 不能解决什么 |
| --- | --- | --- |
| 已有 plugin-sdk | 现在继续使用 | SA 类型、读取、事件、hook；不提供完整 SA↔V portal pipeline |
| 已有 gta-reversed | 现在只阅读 | 帮助理解生命周期；产物要求 Compact，不直接注入 HOODLUM |
| [III.VC.SA.WindowedMode](https://github.com/ThirteenAG/III.VC.SA.WindowedMode) v2.2 | 已固定为可选开发运行依赖 | 解决独占全屏失焦最小化，方便第二显示器操作 PortalDev；不链接进 PortalSA，不安装到 clean 游戏 |
| Win32 file mapping | IPC 阶段直接使用 | 不需要 Boost、gRPC、protobuf；画面阶段仍要做同步与复制 |
| [Universal Modder 的公开示例](https://github.com/rehan-remade/universal-modder/tree/main/examples/minecraft-gta5-passthrough) | 优先参考，暂不安装整个工具包 | 参考帧传输、接收端纹理和合成；不照搬 Minecraft 逻辑、WebSocket、碰撞代理和安装脚本 |
| [ScriptHookV SDK](https://www.dev-c.com/gtav/scripthookv/) | GTA V 阶段再获取 | V 端 native/脚本接入；不等于完整离屏渲染 SDK，也不是 SA 依赖 |
| [ReShade add-on API](https://github.com/crosire/reshade) | 未来 V 合成实验的候选 | 现成案例采用它接收/合成；生成游戏第二视角仍需游戏侧方案 |
| [Spout2](https://github.com/leadedge/Spout2) | CPU 链路证实且性能需要时优先评估 | Windows 图像/纹理共享 SDK，README 列出 DX9/11/12/OpenGL；需核对游戏设备能否接入，不是现成 GTA 传送门 |
| [TsyVM/sa-sdk](https://github.com/TsyVM/sa-sdk) | 暂不加入 | 上游声明针对相同 HOODLUM MD5；本次未验证其结构和接口，不能仅凭自称 production-grade 替换已有 SDK |

除已固定的 WindowedMode 开发辅助外，**现在不用再增加依赖。** 如果需要新资源，优先研究公开 Minecraft↔V 案例；到 GPU 传帧优化阶段，再评估 Spout2。已有 `injector`、`hooking`、`safetyhook` 内容，不为同一用途额外引入一套 hook 库。

未来选择 V 端时，先考虑已有案例验证过的 Legacy 路线，减少变量。[ScriptHookV 作者已宣布 Enhanced 支持](https://www.dev-c.com/post/scripthookv-enhanced)，但这不能证明案例中的渲染插件和我们的离屏方案同时兼容 Enhanced。仍需确认用户的实际 V 版本与 build；本次没有检查 V 安装，也没有下载其 SDK。

## 7. 后续开发与用户验收顺序

| 阶段 | 最小产物 | 用户应看到的结果 |
| --- | --- | --- |
| M1：SA ASI probe（已完成） | x86 PortalSA + 节流日志 + 周期 BMP | 已实机确认 dev 游戏加载插件、状态变化、最终 backbuffer 抓取与窗口化双屏调试 |
| M2：state IPC | file mapping + 外部 Inspector | 独立进程持续读取完整 state；确认 x86 写入和以后需要的 x64 读取布局一致 |
| M3：相机覆盖 | 固定视角实验与恢复 | 游戏画面受指定 pose/FOV 控制；退出覆盖后恢复原相机 |
| M4：SA 离屏相机 | 门视角 color target | 主画面保留，第二张图随请求视角变化；恢复渲染状态，不影响普通游戏画面 |
| M5：单向画面 | CPU mapping + 外部纹理 viewer | 正确尺寸/颜色/方向，无半帧；可观察 frame ID 和延迟 |
| M6：引入 GTA V | 先单向，后双向固定门 | 门内画面实时变化，平移时有正确视差；后台游戏持续更新，前景物体能挡住门面 |
| 后续：穿越 | 一次控制权交接 | 跨门后只控制目标玩家，镜头连续，不重复交接，无按键残留 |

M1 可优先查本地 `FindPlayerPed()`、`CPlaceable::GetPosition()/GetHeading()`、`TheCamera`、`m_mCameraMatrix`、`m_pRwCamera`、`m_aCams[m_nActiveCam]` 与 `CCam::m_fFOV`，配合 `Events::gameProcessEvent`。这些字段确实存在，但最终渲染相机与 active CCam 在过渡时可能不同，不能直接宣称取哪个都一样。

特别注意：本地 `CMatrix::GetForward()` 返回名为 `up` 的基向量、`GetUp()` 返回 `at`；相机的 RenderWare basis 与实体命名不能盲目混用。先用实际相机旋转数据确认 forward/up 约定，再建立 IPC 坐标协议。

plugin-sdk 的 [官方项目创建说明](https://github.com/DK22Pac/plugin-sdk/wiki/Creating-a-new-plugin-in-plugin-sdk) 推荐 installer-launcher 生成插件项目。当前 checkout 也有 `tools/premake/premake5.lua`、`premake5.exe`，脚本支持 `newplugin --type=ASI --name=... --dir=... --gtasa`，且其 `newplugin` 分支默认选择 `vs2026`；经典 SA 路径采用 x32 架构。不要假设别处的旧教程或 VS 默认值正好匹配本 checkout。

里程碑 1 已用本地 Premake 模板生成 PortalSA 工程，移除了模板中停止游戏和自动部署的 post-build 操作，并在 `src/PortalSA/README.md` 提供 build/run/test 命令。2026-10-08 已使用完整的 VS 2022 Build Tools v143 成功编译 plugin-sdk 与 x86 `PortalSA.asi`，并生成 x64 `PortalDev.exe`；随后用户完成上述实机验收，因此 M1 已通过。

### 7.1 M2 相机 metadata 的低延迟方案

当前 `OnGameProcess` 每个游戏 tick 都先更新内存中的 `g_state`，100 ms 只限制日志频率；`OnRenderScene` 还会在实际世界渲染阶段取得 view/projection 矩阵。因此已有数据源足以构造完整相机快照：position、right/forward/up、FOV、aspect/view-window、near/far、view/projection、active camera、phase，以及 frame/sequence/timestamp。建立协议前必须固定坐标系、矩阵行列约定和 FOV 轴向，不能只传一个含糊的 matrix。

M2 使用一个 versioned、pointer-free、固定宽度类型的共享结构，保证 SA x86 写端与 PortalDev/未来 V x64 读端布局一致。建议用双槽或 sequence lock 发布：生产者先构造本地快照，再一次发布完成的 slot；消费者校验发布序号前后一致，只取最新完整状态，不排队、不等待旧状态。时间戳使用同机跨进程可比较的 `QueryPerformanceCounter`，PortalDev 显示 `source_frame_id`、publish rate、read rate、age_us 和 torn-read count。

相机 metadata 只有数百字节，60 Hz 吞吐量只是几十 KB/s，复制成本可忽略。实际端到端年龄主要来自两个游戏 render tick 的相位与 Windows 调度：60 FPS 一帧为 16.67 ms，合理目标是目标游戏渲染前总能读取最新快照，通常落在 0 到 1 帧，调度抖动时可能接近 2 帧。外部 supervisor 只做矩阵变换并发布最新值，不应形成消息队列；portal 映射可表达为“目标门变换 × 穿门翻转 × 源门逆变换 × 当前相机”。是否真正达到目标必须由 QPC 指标实测，不能仅凭共享内存理论宣称零延迟。

### 7.2 连续画面不能沿用 BMP 路径

BMP 是无压缩位图；当前实现写 32-bit top-down BGRA，并每约两秒原子替换同一文件。它适合作为最终 backbuffer 访问证明，但 `GetRenderTargetData` 是同步 GPU→CPU readback，随后还要写磁盘，不是实时传输设计。

CPU shared-memory 原型应使用 2 到 3 个固定 frame slot、每帧的 sequence/QPC/width/height/pitch/format 元数据，以及“只消费最新完成帧、允许丢过期帧”的策略。1600x900 BGRA 每帧约 5.76 MB，60 FPS 单向约 346 MB/s；1920x1080 每帧约 8.29 MB，单向约 498 MB/s。内存带宽本身并非必然瓶颈，但 D3D9 readback stall、额外 CPU copy、目标端 texture upload 和双向传输都可能主导延迟，所以 60 FPS 目前是待测目标，不是已证事实。Supervisor 应变换 metadata，避免再复制整张图像；M5 先做单向、较低分辨率、带 frame-age 统计的原型，再决定是否需要 GPU 共享路径。

### 7.3 M3 固定相机与恢复路线

plugin-sdk 已为 HOODLUM 1.0 US 提供 `CCamera::SetCamPositionForFixedMode`、`TakeControlNoEntity`、`Restore` 和 `RestoreWithJumpCut`。最小实验可以设置固定 camera source、look-at target/up offset，以 jump cut 进入 `MODE_FIXED`，退出时恢复原相机，因此不必为了观察固定地点而每次手动移动 CJ。

但 gta-reversed 的 `CCam::Process_Fixed` 显示 fixed mode 每次处理都会重新计算 front/up，并把 FOV 设为 70 度；任意 FOV/姿态同步不能只在进入模式时写一次，必须在相机更新完成后、世界渲染前的稳定阶段应用，并在退出时完整恢复。另一个风险是世界 streaming：相机可以远离 CJ，但远处模型/LOD/碰撞未必已经加载。M3 应先在玩家附近验证固定 pose/FOV 与恢复，再测试远距离硬编码点并记录 streaming 缺失；这仍只是覆盖主相机的实验，最终 portal 离屏相机属于 M4。

## 8. 本次 Git 忽略规则与用户检查方法

根目录 `.gitignore` 采用白名单：

```gitignore
/*
!/.gitignore
!/.gitmodules
!/README.md
!/src/
!/third_party/
/third_party/*
!/third_party/plugin-sdk/
/third_party/plugin-sdk/*
!/third_party/plugin-sdk/plugin-sdk/
!/third_party/III.VC.SA.WindowedMode/
!/reference/
/reference/*
!/reference/gta-reversed/
```

结果是收录 `src/`、根目录 README/配置以及三条 submodule gitlink；`game/`、其他 `third_party/` 与 `reference/` 内容、`tools/`、`vendor/`、`.vscode/` 默认被忽略。里程碑 1 还增加了对 `src/PortalSA/bin/`、`obj/`、`.vs/` 和用户项目配置的忽略，避免构建产物进入版本控制。

用户可在 PowerShell 中检查当前登记：

```powershell
Set-Location 'D:\coding\repo\GTAPortal'
git status --short --untracked-files=all
git submodule status
git ls-files --stage .gitmodules third_party/plugin-sdk/plugin-sdk third_party/III.VC.SA.WindowedMode reference/gta-reversed
git check-ignore -- game/clean/ game/dev/ tools/ vendor/ .vscode/
```

预期 `git submodule status` 列出三个固定 commit；`git ls-files --stage` 中三条依赖为模式 `160000`；`git check-ignore` 输出游戏和其他排除目录。规则不会自动移除已跟踪文件，今后不要用 `git add -f` 强行加入游戏目录。这个仓库配置变更无需编译或运行游戏验收。

## 附录 A：commands.txt 原文

以下原文保存项目来历与已有资源。关于“当前要做什么”、直接编译验证、HOODLUM/Compact 支持等内容，应结合本文第 1、3、7 节阅读；最新用户要求优先。未来变更以正文中的明确决定为准，不暗改这份历史文本。

````text
你现在接手一个跨游戏实时传送门项目 **GTAPortal**。请先完整理解下面的现状和约束，再查看仓库，不要擅自扩大 V1 范围。

# 1. 项目最终目标

目标是实现：

**GTA V ↔ GTA San Andreas (经典 PC 版)**

之间的跨进程、跨游戏实时传送门。

长期设想是：
- GTA V 世界中存在一个固定传送门，可以透视看到 GTA SA 另一侧。
- GTA SA 世界中也存在一个固定传送门，可以透视看到 GTA V 另一侧。
- 两个游戏是两个独立进程。
- 需要同步 portal camera。
- 后续可能真正实现人物穿越，并进行 gameplay authority / control handoff。

但这些长期功能现在不要做。

---

# 2. V1 的严格范围

V1 要极度简化，只做：

1. 两个**硬编码固定位置**的 Portal。
2. GTA V 和 GTA SA 两个独立进程。
3. Portal 另一侧的**实时透视画面**。
4. Camera transform 同步。
5. 进程间通信可以使用共享内存。
6. 后续真正做到画面传递时，可以通过共享内存传输 color frame / texture data。

V1 明确**不做**：

- 实体真正穿越
- 玩家控制权切换
- NPC 穿越
- 物理交互
- 两边世界实体同步
- portal depth
- stencil/depth-aware clipping 的完整实现
- portal recursion
- 复杂 portal placement editor
- 任意位置动态 Portal
- 联机
- gameplay authority handoff

当前首先只开发 **GTA SA 一侧**。

不要现在碰 GTA V。

---

# 3. GTA SA 当前版本：已经精确确认
在game下，有两份目前一模一样的gtasa。以后只往dev里的那一份加减东西。
游戏 EXE：

```text
gta-sa.EXE
```

文件大小：

```text
14,383,616 bytes
```

MD5：

```text
170B3A9108687B26DA2D8901C6948A18
```

这是：

```text
GTA San Andreas PC
1.0 US
HOODLUM
x86 / 32-bit
```

这就是我们目前选定的目标版本。

**不要 downgrade。**
**不要换 Compact EXE。**
**不要修改 gta-sa.EXE。**
因为目前很多工具都是基于
1.0 US
HOODLUM
而不是compact，除了reference/下的gta-reversed,据我所知。他们是针对compact的对吧？
你应该调查一下。
目前这个版本游戏已经可以正常启动并游玩。

游戏目录目前已知位置：

```text
D:\coding\repo\GTAPortal\game\clean\GTA San Andreas
```

---

# 4. 已经解决的启动问题

之前双击 GTA SA 时出现：

```text
Cannot find 1707x960x32 video mode
```

机器使用 2560×1440 显示器，Windows 150% DPI scaling：

```text
2560 / 1.5 ≈ 1707
1440 / 1.5 = 960
```

问题已经通过 Windows 的 High DPI compatibility 设置解决。

游戏目前能正常启动。

不要为了修复这个问题再次修改游戏代码、分辨率逻辑或者 EXE。

两份游戏我都改好了
---

# 5. 当前 ASI 加载系统已经确认

游戏目录中已经存在：

```text
vorbisFile.dll
```

它的 VersionInfo：

```text
FileDescription  : Ultimate ASI Loader
ProductName      : Ultimate-ASI-Loader-x86
FileVersion      : 1.0.0.0
CompanyName      : ThirteenAG
OriginalFilename : Ultimate-ASI-Loader-x86.dll
```

所以当前 GTA SA 已经安装了：

```text
Ultimate ASI Loader x86
```

并且是通过 `vorbisFile.dll` proxy 方式加载。

目录中还有：

```text
vorbisHooked.dll
```

因此大致加载链是：

```text
gta-sa.EXE
    ↓
vorbisFile.dll
    ↓
Ultimate ASI Loader x86
    ↓
*.asi
```

**不要再安装新的 Ultimate ASI Loader。**

尤其不要下载或者覆盖：

```text
dinput8.dll
```

---

# 6. DINPUT8.DLL 已确认是正常微软 DLL

当前：

```text
DINPUT8.DLL
```

VersionInfo：

```text
FileDescription  : Microsoft DirectInput
ProductName      : Microsoft® DirectX for Windows® 95 and 98
FileVersion      : 4.08.00.0400
CompanyName      : Microsoft Corporation
OriginalFilename : DInput8.dll
```

SHA256：

```text
D7865FBE8319D993B8EEFF094593CB70BEA3E49F02095A308605E14F11A34353
```

不要用 ASI Loader 的 dinput8.dll 覆盖它。

---

# 7. 当前已有 ASI 插件

游戏目录中目前有：

```text
III.VC.SA.LimitAdjuster.asi   396800 bytes
SilentPatchSA.asi             282624 bytes
wm_sachs.asi                  138752 bytes
```

其中：

```text
wm_sachs.asi
```

是 GTA III / VC / SA 中文汉化插件的一部分。

目录还存在：

```text
wm_sachs\
```

所以当前中文大概率通过 ASI 插件 + 自己的字体/文本资源实现。

---

# 8. 中文环境处理原则

现在游戏有中文字幕，而且已经正常游玩。

主 EXE 的 MD5 与标准 1.0 US HOODLUM 精确匹配，所以汉化并没有修改主 EXE。

目前不要删除中文。

中文插件对第一阶段这些操作预计没有影响：

- Player position
- heading
- CCamera
- camera matrix
- FOV
- IPC
- shared memory

如果后续开始 Hook RenderWare / D3D9 渲染，出现不可解释的问题，再说

但当前不要主动移除。

---

# 9. 游戏目录其他已知信息

还有一个：

```text
gtasa_cn.exe
```

大小只有：

```text
4915 bytes
```

平时实际启动的是：

```text
gta-sa.EXE
```

不是 `gtasa_cn.exe`。

`text` 目录当前至少有：

```text
american.gxt   738256
french.gxt     798974
german.gxt     812934
italian.gxt    751379
spanish.gxt    763917
```

当前已经创建了一个 GTA SA 初始存档。

不要破坏这个基准环境。

---

# 10. 当前代码仓库

在 GTAPortal 项目中已经 clone：

```text
plugin-sdk
gta-reversed
```

**没有 clone：**

```text
sa-sdk
```

目前不要因为以前的建议而自动把它加回来，除非后续确实证明有必要。

也还没有下载：

```text
ScriptHookV SDK
```

这是有意的。

当前首先只完成 GTA SA 一侧。

---

# 11. plugin-sdk 的定位

`plugin-sdk` 是当前 GTA SA 端的**主要开发依赖**。

优先使用它已有的：

- GTA SA classes
- CPlayerPed
- CCamera
- CEntity
- CMatrix
- RenderWare definitions
- events
- injector / hooking utilities

不要重新手写一套已经存在于 plugin-sdk 中的游戏结构。

同时要特别注意当前游戏版本是：

```text
1.0 US HOODLUM
```

使用地址、结构或者 hook 前，必须确认 plugin-sdk 对应版本支持情况。

---

# 12. gta-reversed 的定位

`gta-reversed` 当前只作为：

**源码级 reference / reverse-engineering encyclopedia**

使用。

可以用来研究：

- CCamera
- camera update
- CRenderer
- RenderWare pipeline
- CWorld
- CEntity
- CPhysical
- Player
- matrix
- visibility
- render stages
- frame lifecycle

目前：

**不要 build gta-reversed。**
**不要把 gta-reversed 生成的 DLL 注入当前 Hoodlum 游戏。**

因为 gta-reversed 对 Compact EXE 有自己的严格要求，而当前正式开发目标是 Hoodlum 1.0 US。

我们只阅读它的实现和结构信息。

---

# 13. 当前计划的基础架构

未来总体结构大约是：

```text
┌────────────────────────┐
│ GTA San Andreas x86    │
│                        │
│ PortalSA.asi           │
│ - Player state         │
│ - Camera state         │
│ - Render hook later    │
└───────────┬────────────┘
            │
            │ Shared Memory / IPC
            │
            ▼
┌────────────────────────┐
│ GTAPortal shared state │
│                        │
│ transforms             │
│ camera                 │
│ timestamps/frame ids   │
│ later: color frames    │
└───────────┬────────────┘
            │
            │ Shared Memory / IPC
            ▼
┌────────────────────────┐
│ GTA V x64              │
│                        │
│ future plugin          │
└────────────────────────┘
```

但 GTA V 侧现在不要实现。

---

# 14. IPC 原则

后面需要跨进程 IPC 时，优先直接使用 Win32：

```cpp
CreateFileMapping
OpenFileMapping
MapViewOfFile
```

不要现在引入：

- Boost.Interprocess
- gRPC
- sockets
- protobuf
- Redis
- named pipe framework

除非以后证明 shared memory 不够。

GTA SA 是 x86，未来 GTA V 是 x64，所以共享结构体设计必须明确布局。

例如以后可以设计：

```cpp
struct PortalState {
    uint32_t magic;
    uint32_t version;

    float position[3];
    float forward[3];
    float up[3];

    float fov;

    uint64_t frameId;
};
```

但现在不要急着写最终协议。

---

# 15. 当前第一个真正里程碑

不要一开始做 Portal rendering。

首先建立一个最小的 GTA SA ASI probe/plugin。

第一阶段成功标准：

游戏启动后 Ultimate ASI Loader 成功加载 咱们的.asi。

插件能够创建：

```text
portal.log
```

然后可靠读取并周期性记录：

```text
Player/CJ position: x y z
Player/CJ heading

Camera position: x y z
Camera forward vector
Camera up vector（如果容易获得）
Camera FOV
```

不建议真的每 frame flush 文件导致大量 IO。

可以：
- 每 500 ms / 1 s 输出一次；
- 或只在 debug mode 输出；
- 或内存中更新，每隔一段时间写日志。

首先验证**数值确实随着 CJ 移动和 camera 转动正确变化**。

---

# 16. 第一个 milestone 完成后

顺序应该是：

## Milestone 1 — ASI / Memory Probe

```text
gta-sa.exe
→ Ultimate ASI Loader
→ PortalSA.asi
→ plugin-sdk
→ Player + Camera state
→ portal.log
```

## Milestone 2 — Shared Memory

将上述 Player / Camera state 写入 Win32 shared memory。

然后单独写一个普通 Win32 console test program：

```text
PortalInspector.exe
```

读取 shared memory 并实时打印数据。

此阶段仍然不要碰 GTA V。

这样可以验证：

```text
GTA SA process
    ↓
shared memory
    ↓
independent external process
```

链路可靠。

## Milestone 3 — Camera override experiment

研究 GTA SA camera 能否被我们的插件以稳定方式 override。

例如：
- 设置 camera position
- 设置 target / forward
- 设置 FOV

先证明我们可以从外部 state 驱动 GTA SA portal camera。

## Milestone 4 — Rendering research

然后才开始研究：
- RenderWare
- Direct3D9
- offscreen render target
- alternate camera
- scene render timing
- texture upload/display

最终目标是在 GTA SA 中把“另一侧画面”画到一个 portal surface 上。

---

# 17. 非常重要：不要过早做真实 Portal

当前项目风险最高的部分不是 IPC，而是：

**一个游戏进程怎样获得另一游戏相机看到的实时画面，并把它低延迟地贴到 Portal 上。**

最终可能涉及：

```text
Game A portal camera
→ offscreen rendering / capture
→ shared GPU/CPU frame
→ IPC
→ Game B texture
→ portal surface
```

但是现在不要直接攻击这个问题。

先逐层打通：

```text
ASI
↓
player/camera state
↓
shared memory
↓
external reader
↓
camera override
↓
render hook
↓
cross-process image
```

每一步都必须可以独立验证。

---

# 18. 编译环境注意事项

GTA SA 是 32-bit，所以 `PortalSA.asi` 必须是：

```text
Win32 / x86
```

不能编译成 x64。

请先检查当前机器/仓库中：
- Visual Studio / MSVC 可用版本
- plugin-sdk 的推荐 build 方式
- 当前 plugin-sdk clone 是否包含需要的 submodules
- 项目是否已有 CMake/premake/VS solution

不要在不知道 plugin-sdk 推荐集成方式的情况下随便手写十几个 include/lib 路径。

---

# 19. 开发原则

这个项目是实验性系统/图形项目，希望代码保持可理解。

优先：

- C++
- 原生 Win32
- plugin-sdk
- 明确的数据结构
- 简单 IPC
- 小的 milestone
- 可验证日志
- 最少依赖

避免：

- 一开始引入大型框架
- 魔法脚本
- 过度抽象
- 不必要 dependency
- 一次写完整 portal system
- 没验证就大量 hook
- 直接改游戏 EXE
- 把地址散落在业务代码里

如果必须使用 hardcoded address：
- 集中封装；
- 注明对应版本；
- 明确写 `GTA SA 1.0 US HOODLUM`；
- 尽量先确认 plugin-sdk 是否已经提供对应接口。

---

# 20. Clean 与 Dev 环境原则

`game/clean` 是已验证可正常玩的基准版本。

不要往 clean 中不断塞测试 DLL。

应该保持类似：

```text
game/
├── clean/
│   └── GTA San Andreas/
│
└── dev/
    └── GTA San Andreas/
```

开发时只往 dev 放：

```text
PortalSA.asi
portal.ini
portal.log
```

以及后续必要的开发文件。

如果 `dev` 当前不存在，可以建议创建，但先检查仓库。

---

# 21. 你现在要做的事情

请先：

1. Inspect 当前 `GTAPortal` repository。
2. 找到 `plugin-sdk` 和 `gta-reversed` 的实际路径。
3. 查看 plugin-sdk 当前推荐的 GTA SA ASI 项目创建/编译方式。
4. 不要修改 `game/clean`。
5. 设计一个最小 `PortalSA` Win32/x86 ASI target。
6. 第一版代码只完成：
   - 插件加载验证
   - `portal.log`
   - Player position
   - Player heading
   - Camera position
   - Camera forward
   - FOV
7. 给出实际 build/run/test 方法。
8. 尽量直接编译并验证；如果你具备本地执行能力，就实际执行，不要只给理论步骤。
9. 遇到 plugin-sdk API 不确定时，优先搜索仓库源码；需要理解 GTA SA 内部逻辑时，再搜索 `gta-reversed`。
10. 不要开始 GTA V 侧，也不要开始 cross-process framebuffer。

完成第一个 milestone 后，总结：
- 新增/修改了哪些文件；
- 使用了哪些 plugin-sdk API；
- 编译目标是否为 x86；
- ASI 是否被成功加载；
- log 中读取到哪些真实游戏数据；
- 下一步最小任务是什么。

从 inspect repository 开始。
````
