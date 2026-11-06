# 更新日志

版本号唯一来源是 `CMakeLists.txt` 的 `project(reshade_preui VERSION x.y.z)`，
编译期以 `PREUI_VERSION` 宏注入。tag 写 `v0.7` 或 `v0.7.0` 都可以，CI 会归一化后比较。

---

## v0.7.0 — 2026-11-06

**首个公开发布版本。** 滤镜正确覆盖 3D 画面、UI 完全不受影响、DLSS 可正常开启渲染。
已在《王者荣耀世界》4K + DLSS + HDR 下连续 37800 帧验证。

### 核心机制

- Hook `NVSDK_NGX_D3D12_EvaluateFeature`，在 DLSS 输出完成后、UI 合成之前插入效果链。
- 命令列表查找：扫描 `native + {0, 8, 16, …, 256}`，命中后缓存偏移（实测为 `+8`）。
- 通过 `effect_runtime::render_effects()` 渲染，副作用是 ReShade 会跳过 Present 阶段
  对整帧（含 UI）的第二次渲染——这正是「UI 不被滤镜影响」的来源。

### 本次发布包含的优化

- **扫描降频**：hook 成功后，模块枚举从「每 60 帧」降为「每 3600 帧（约 1 分钟）」。
  原先每秒一次 `CreateToolhelp32Snapshot`（全进程模块快照）且永久执行，会在渲染线程上
  产生周期性尖峰。保留低频扫描，以便模块被重载后仍能重新挂钩。
- **效果关闭时跳过注入**：`render_effects` 前先查 `effect_runtime::get_effects_state()`，
  关闭时直接返回，省掉每帧 2 次全屏拷贝（4K HDR 约 66 MB/次，60 fps ≈ 4 GB/s）。
- **消除两处数据竞争**：`g_device` 写入挪进 `g_temp_mutex`；代理偏移改为在锁内通过出参读出
  （原先解锁后才读取 `g_proxy_offset` 拼日志）。
- **日志自动截断**：`PreUI.log` 超过 4 MiB 时在启动阶段清空，避免早期版本的刷屏日志无限膨胀。
- **`AddonUninit` 补全清理**：新增 `MH_RemoveHook` 与 7 个 `unregister_event`，
  避免 ReShade 卸载 add-on 后回调悬空。
- **`ensure_temp` 增加 0 尺寸防护**：切换分辨率 / 首帧偶尔读到 0 尺寸，直接跳过，
  不再刷 `create_resource failed`。
- 版本号改由 CMake 注入的 `PREUI_VERSION` 宏提供，启动日志打印真实版本。

### 刻意未改（这些是能跑通的原因）

- 每帧的 barrier 序列与两次全屏拷贝：DLSS 输出资源在 UE4 里状态不可预知，
  用 `general`(COMMON) 作为前后状态是唯一稳的写法。
- 每帧重新 `get_resource_desc`：D3D12 资源指针存在地址复用，缓存尺寸会读到过期 desc。
- hook 模块名单不扩大到 `*nvngx*`：否则会把 `nvngx_dlssg.dll`（帧生成）也勾上，
  滤镜会被叠加两次。

### 技术背景：命令列表代理偏移

ReShade 的 D3D12 命令列表是代理对象：`D3D12GraphicsCommandList`（COM，前 8 字节是 vtable）
后接 `command_list_impl`（`reshade::api::command_list` 基类），所以 `api::command_list*`
通常等于 `ID3D12GraphicsCommandList* + 8`。v0.6 试图通过「自己创建命令列表并观察事件」
标定这个偏移，但 `device->get_native()` 返回真实设备，绕过了代理，永远不会触发
`reset_command_list`，因此标定必然失败（日志里满屏 `api=0000000000000000`）。

---

## 开发记录（0.1 ~ 0.6，未公开发布）

### v0.6.0 — 2026-11

- 尝试用自建命令列表标定代理偏移（**失败**，原因见上）。
- 引入 `PreUI.ini`（`Enabled` / `DelayFrames` / `Trace`）。

---

### v0.5.0 — 2026-11

- 修正模块名：游戏实际使用的是驱动目录下的 `_nvngx.dll`，而不是游戏自带的 `nvngx_dlss.dll`。
- 记录 `init/reset/close/execute_command_list` 事件，按 `api::command_list*` 建表
  （此时仍查不到，因为拿到的 key 是 native 指针）。

---

### v0.4.0 — 2026-11

- 改为扫描已加载模块寻找 NGX 导出，而不是只检查固定文件名。
- 引入 `PreUI.log` 文件日志（与 DLL 同目录，失败时退回 `%TEMP%`）。

---

### v0.3.0 — 2026-11

- **修复游戏无法启动**：v0.2 在 `AddonInit` 里立刻开线程 hook，而 MinHook 启用 hook 会挂起
  进程内所有线程，在启动阶段（loader lock / 反作弊扫描）极易卡死。改为延迟到第 120 帧
  `reshade_present` 之后再在渲染线程上 hook。
- 新增 `PreUI.ini` 的 `Enabled=0` 逃生开关。

---

### v0.2.0 — 2026-11

- 直接 hook `nvngx_dlss.dll!NVSDK_NGX_D3D12_EvaluateFeature`，成功后复制 NGX 的
  `Output` 资源 → 临时纹理 → `render_effects` → 拷回。
- 因启动即 hook，会导致游戏启动失败（见 v0.3）。

---

### v0.1.0 — 2026-11

- 验证 `effect_runtime::render_effects()` 可以在 UI 合成前渲染，且会阻止 ReShade 在
  Present 阶段重复渲染（这正是「UI 不被滤镜影响」的关键）。
