# ReShade Pre-UI add-on

[![Build](https://github.com/2529714076/reshade-preui-addon/actions/workflows/build.yml/badge.svg)](https://github.com/2529714076/reshade-preui-addon/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/2529714076/reshade-preui-addon)](https://github.com/2529714076/reshade-preui-addon/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

把 ReShade 的 `.fx` 效果绘制到 **UI 合成之前的 3D 画面** 上——UI 完全不受滤镜影响，
画面仍然由游戏自己 Present。也就是所谓的“游戏内画面实时替换”。

目标环境：**UE4 / DX12 / DLSS**（在《王者荣耀世界》4K + DLSS + HDR 下实测通过）。

> **A ReShade add-on that renders effects into the pre-UI frame.** It hooks
> `NVSDK_NGX_D3D12_EvaluateFeature`, takes the DLSS output (display resolution, before the
> UI is composed), runs ReShade's effect chain on it via `effect_runtime::render_effects()`,
> and copies the result back — so the UI stays crisp while the 3D scene gets filtered.

---

## ⚠️ 先读这一段

- 本程序会**注入 DLL 并 inline hook 显卡驱动的 DLSS 函数**。在任何带反作弊的在线游戏里使用都存在
  **封号风险**，请自行评估。强烈建议先在单机 / 离线场景验证。
- 只在**你拥有或获准修改**的游戏上使用，遵守游戏的服务条款与当地法律。
- 目前只支持 **DX12 + DLSS**；不开 DLSS 时本 add-on 不工作（ReShade 会回到默认行为，UI 会被滤镜影响）。

---

## 特性

- ✅ 滤镜只作用于 3D 画面，**UI（血条、小地图、聊天框）保持原样**
- ✅ 与 DLSS 共存：效果在 DLSS 输出之后、UI 合成之前执行
- ✅ 复用 ReShade 官方 add-on API，无私有 hack；不修改 ReShade 本体
- ✅ 失败时静默降级：不满足条件就什么都不做，画面与原生一致
- ✅ 自带文件日志 `PreUI.log`，每个失败原因都有明确文案
- ✅ 单个 `ReShade_PreUI.addon64`，静态 CRT，无需额外运行库

## 兼容性

| 项 | 状态 |
|---|---|
| 图形 API | DX12（DX11 / Vulkan 不支持） |
| 升采样 | 必须开启 DLSS（DLSS 2 / 3 / 4 的超分路径均走同一个 `EvaluateFeature`） |
| DLSS 帧生成（DLSS-G） | **建议关闭**，插值帧不走本路径 |
| ReShade | 6.8.0（`RESHADE_API_VERSION = 20`） |
| 系统 | Windows 10 / 11 x64 |
| 显卡 | NVIDIA RTX（需要 NGX 运行时，即驱动目录下的 `_nvngx.dll`） |

---

## 工作原理

```
3D 场景 ──► 后处理 ──► [DLSS] ──► UI 合成 ──► Present
                          │
                          └──► 我们的注入点（UI 之前）
```

1. 在第 120 帧之后，扫描已加载模块，找到 `_nvngx.dll` / `nvngx_dlss.dll` 的
   `NVSDK_NGX_D3D12_EvaluateFeature` 并 inline hook。
2. DLSS 输出完成时，从 NGX 参数里取出 `Output` 资源（显示分辨率、UI 之前的画面）。
3. 拷贝到一张同尺寸临时纹理，调用 ReShade 官方 API：

   ```cpp
   effect_runtime::render_effects(cmd_list, temp_rtv, temp_rtv_srgb);
   ```

4. 把结果拷回 DLSS 输出资源，引擎随后照常叠加 UI。
5. 因为本帧已经手动渲染过，ReShade 会跳过 Present 前对整帧（含 UI）的第二次渲染。

ReShade 文档对 `render_effects` 的原话就是为此设计的：

> Applies post-processing effects to the specified render targets and **prevents the usual
> rendering of effects before swap chain presentation** of the current frame. This can be used
> to force ReShade to render effects at a certain point during the frame to e.g. **avoid effects
> being applied to user interface elements** of the application.

### 一个关键实现细节：命令列表代理偏移

ReShade 的 D3D12 命令列表是**代理对象**：

```
ID3D12GraphicsCommandList10 (COM，前 8 字节是 vtable)
└─ command_list_impl         (reshade::api::command_list 基类)
```

所以 `reshade::api::command_list*` 通常等于 `ID3D12GraphicsCommandList* + 8`。
本 add-on 在第一次成功解析后缓存这个偏移（实测 `+8`），后续直接命中，不做扫描。

> 反面教材：不要用 `device->get_native()` 创建命令列表来“标定”这个偏移——它返回的是真实设备，
> 绕过了 ReShade 的代理，永远不会触发 `reset_command_list` 事件，标定必然失败。

---

## 安装

1. 先装好 **ReShade 6.8.0**（游戏目录里应有 `dxgi.dll` 或 `d3d12.dll`）。
2. 到 [Releases](../../releases/latest) 下载 `ReShade_PreUI-vX.Y.Z.zip`。
3. 把压缩包里的 **`ReShade_PreUI.addon64`** 和 **`PreUI.ini`** 解压到**游戏 exe 同目录**
   （就是 ReShade 那个目录）。例：

   ```
   E:\KingLauncher\HOKWorld\WindowsNoEditor\NGR\Binaries\Win64\
       dxgi.dll
       ReShade_PreUI.addon64
       PreUI.ini
   ```

4. 启动游戏 → 视频设置里**开启 DLSS** → 进实际场景（不要停在菜单）。
5. 打开 ReShade 面板，勾一个滤镜。

ReShade 启动时会自动加载同目录下所有 `*.addon64`，不需要改任何配置去“注册”它。

## 配置

`PreUI.ini` 与 `.addon64` 同目录，**整个文件可以不存在**，缺失时全部用默认值。

| 键 | 默认 | 说明 |
|---|---|---|
| `Enabled` | `1` | `0` = 只加载 DLL、不装任何 hook。排查“游戏启动不了”时先用这个 |
| `DelayFrames` | `120` | 第 N 帧之后才开始扫描并挂钩。启动困难就调到 `600` 或 `1200` |
| `Trace` | `1` | `1` = 在 `PreUI.log` 里记录失败原因（最多 60 条） |

改完不需要重装，**重启游戏**生效。

## 验证是否生效

启动后游戏目录下会生成 `PreUI.log`（写不进去时退回 `%TEMP%\PreUI.log`）。
生效时会看到类似内容：

```
[PreUI] ===== AddonInit v0.8.0 =====
[PreUI] delaying hook for 120 frames
[PreUI] target _nvngx.dll!NVSDK_NGX_D3D12_EvaluateFeature -> 00007FFED8276F90
[PreUI] proxy offset resolved: 8 (native=00000000E44E6750)
[PreUI] temp target created 3840x2160 fmt=10
[PreUI] injected 1 frames (3840x2160 fmt=10)
[PreUI] injected 600 frames (3840x2160 fmt=10)
```

判据：

| 日志 | 含义 |
|---|---|
| `target ... EvaluateFeature -> 0x...` + `MH_EnableHook -> 0` | 钩子装上了 |
| `proxy offset resolved: 8` | 命令列表代理偏移已解析 |
| `temp target created WxH fmt=N` | 临时纹理已按 DLSS 输出尺寸创建 |
| `injected N frames` 持续增长 | **逐帧注入成功，正在工作** |

画面判据：勾上滤镜后**只有 3D 场景变色、UI 不变色** → 成功。

> `fmt=10` 是 `R16G16B16A16_FLOAT`（HDR 线性空间）。所以画面正常，但为“后 tonemap”写的
> `.fx` 观感会偏，需要换 HDR 适配的预设。

---

## 常见问题

### 没有 `injected`，只有 `no cmd list` / `Get(Output)` 失败

说明 hook 到了但拿不到可用的命令列表或输出资源。把 `PreUI.log` 全文（`Trace=1`）
和 `ReShade.log` 最后 30 行贴到 Issue。

### 游戏启动不了 / 启动器点开始就退

1. 把 `ReShade_PreUI.addon64` 移出游戏目录，确认游戏能正常启动 → 确认是本 add-on 引起。
2. 放回去，`PreUI.ini` 里设 `Enabled=0`：
   - 能启动 → 是 hook 逻辑的问题，把 `DelayFrames` 调到 `600` 再试。
   - 仍然启动不了 → 是“加载这个 DLL”本身触发的问题（反作弊 / 注入器拦截），本方案不适用。

### 进游戏后 UI 也被滤镜影响了

说明注入没生效，ReShade 回退到了 Present 阶段的整帧渲染。依次检查：

1. DLSS 是否真的开着（`nvngx` 模块有没有被加载）；
2. 日志里有没有 `injected N frames`；
3. 有没有同时装其它 hook NGX 的 add-on（例如 RenoDX DLSS 相关），它们会抢先挂同一个函数。

### 画面全黑 / 闪烁 / 崩溃

- 先移除其它 DX12 注入工具（MSI Afterburner、其它 ReShade add-on）排除冲突。
- 自己编译时如果动过 barrier 序列，恢复成上传版本——DLSS 输出资源的状态在 UE4 里不可预知，
  本实现用 `general`(COMMON) 当前后态是唯一稳的写法。

### 性能

每帧多两次全屏拷贝 + 效果本身开销。4K HDR 一次约 66 MB，两次约 4 GB/s @60fps。

**这两次拷贝结构上省不掉**：`render_effects` 的 `rtv` 参数就是“效果链眼中的 backbuffer”，
所以输入必须先落到我们自己的纹理上、效果再写进这张纹理、最后拷回 DLSS 输出。
想让效果直接写游戏资源，就会让同一资源同时被当作 SRV 和 RTV 绑定（D3D12 不允许）。

---

## 从源码构建

### 方式 A：GitHub Actions（推荐，零本地环境）

Fork 或新建仓库，把本目录内容推到仓库根目录（根目录应直接是 `CMakeLists.txt`、`src/`、
`third_party/`），然后：

- **push 到 `main` / 建 PR** → `Build` workflow 跑一次编译，产物在 Actions 页面的
  **Artifacts → `ReShade_PreUI`**。
- **打 tag** → `Release` workflow 自动编译、打包、创建 Release（见下）。

workflow 会先从 `crosire/reshade` 的 **v6.8.0** 标签同步 8 个 add-on 头文件，
所以头文件版本永远和你本机的 ReShade 6.8.0 对齐。

### 方式 B：本地编译

需要 **Visual Studio 2022（含“使用 C++ 的桌面开发”）** 和 **CMake ≥ 3.20**。

```bat
build.bat
```

或手动：

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

产物：`build\Release\ReShade_PreUI.addon64`

### 构建失败排查

| 报错 | 原因 | 处理 |
|---|---|---|
| `C1083: Cannot open include file: 'reshade_overlay.hpp'` | `reshade.hpp` 会包含 `reshade_overlay.hpp`，工程里漏了这个文件 | 用本仓库的 workflow（会自动同步 8 个头文件），或手动补上 |
| `C1083: Cannot open include file: 'MinHook.h'` | `third_party/minhook` 不完整 | 重新拉取整个 `third_party/` |
| `LNK2019` / `LNK1120` | 少编译了 MinHook 的 `.c` 文件 | 确认 `CMakeLists.txt` 里 4 个 MinHook 源文件都在 |
| 编译通过但游戏里没日志 | ReShade 没加载 add-on | 确认放在 exe 同目录，扩展名是 `.addon64`（不是 `.dll`） |

---

## 发布新版本

版本号唯一来源是 `CMakeLists.txt` 的 `project(reshade_preui_addon VERSION x.y.z)`，
编译期通过 `PREUI_VERSION` 宏注入到日志里。发布流程：

```bat
:: 1. 改 CMakeLists.txt 里的 VERSION，并在 CHANGELOG.md 记一笔
:: 2. 提交
git add -A
git commit -m "release: v0.8.0"
git push

:: 3. 打 tag 并推送（tag 必须与 CMake VERSION 一致，否则 workflow 会直接失败）
git tag v0.8.0
git push origin v0.8.0
```

`Release` workflow 会：

1. 校验 tag 与 `CMakeLists.txt` 的 VERSION 是否一致（不一致直接报错，避免发错版本）；
2. 编译；
3. 打包 `ReShade_PreUI-v0.8.0.zip`（内含 `.addon64`、`PreUI.ini`、README、CHANGELOG、LICENSE）；
4. 创建 GitHub Release 并附上 zip。

---

## 目录结构

```
.
├─ CMakeLists.txt                 # 版本号唯一来源
├─ build.bat                      # 本地一键编译
├─ PreUI.ini                      # 随包发布的默认配置
├─ src/
│  └─ main.cpp                    # 全部逻辑（约 730 行，含详细注释）
├─ third_party/
│  ├─ reshade/include/            # ReShade v6.8.0 add-on API 头文件（8 个）
│  ├─ ngx/                        # NVIDIA NGX SDK 头文件（3 个）
│  └─ minhook/                    # MinHook（x64 inline hook）
├─ .github/workflows/
│  ├─ build.yml                   # push / PR 编译 + artifact
│  └─ release.yml                 # tag -> GitHub Release
├─ CHANGELOG.md
├─ THIRD_PARTY_NOTICES.md
└─ LICENSE
```

---

## 已知限制

- **必须开启 DLSS**：注入点就在 DLSS 上。关闭 DLSS 时 add-on 静默不工作，ReShade 回到默认行为。
- **某帧没有调用 DLSS 时**：那一帧 `g_frame_injected` 仍为 0，ReShade 会按默认路径在 Present 前
  叠一次效果，于是这一帧 UI 会被滤镜影响（观感上是偶发闪一下）。这是公开 API 的边界，
  无法从 add-on 侧屏蔽。实测每帧都调 DLSS 的游戏不会出现。
- **色彩空间**：DLSS 输出是 HDR 线性（`R16G16B16A16_FLOAT`），与最终 backbuffer 格式不同，
  需要适配 HDR 的预设。
- **不同游戏差异**：DLSS 调用位置、`Output` 资源状态、是否有多条命令队列都可能不同。
  本实现针对 UE4 的单队列路径验证过，其它引擎可能需要按日志微调。
- 这是**实验性**实现，不提供任何担保。

---

## 致谢
- [Deepseek Harness](https://www.deepseek.com/harness)（深度求索）—— 本代码完全由Deepseek Harness 设计 （作者只提供设计方向）
- [ReShade](https://reshade.me)（crosire）—— add-on API 与 `render_effects` 的设计
- [MinHook](https://github.com/TsudaKageyu/minhook)（Tsuda Kageyu）—— inline hook
- [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) —— NGX 参数接口

第三方组件的许可详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，
本仓库原创代码使用 [MIT License](LICENSE)。

## 免责声明

本项目仅供学习与技术研究。使用者需自行承担使用本软件的全部风险，包括但不限于账号封禁、
数据损坏、系统不稳定。作者不对任何直接或间接损失负责。请勿用于规避游戏反作弊机制或
任何违反服务条款的用途。
