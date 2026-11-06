# 第三方组件与许可

本仓库的原创代码使用 [MIT License](LICENSE)。`third_party/` 下的组件各自保留原有许可，
下面是它们的来源、版本与许可原文出处。

---

## 1. ReShade add-on API 头文件

| 项 | 内容 |
|---|---|
| 路径 | `third_party/reshade/include/`（8 个头文件） |
| 来源 | https://github.com/crosire/reshade （tag **v6.8.0**） |
| 版权 | Copyright (C) 2021 Patrick Mours；`reshade_overlay.hpp` 另含 Copyright (C) 2014-2026 Omar Cornut（Dear ImGui） |
| 许可 | **BSD-3-Clause OR MIT**（每个文件头部均有 SPDX 标识） |
| 用途 | 仅作为 add-on 编译期头文件使用；**不链接、不修改、不重分发 ReShade 本体** |

CI（`.github/workflows/build.yml`）会在编译前从 v6.8.0 标签重新下载这 8 个文件，
因此仓库里的副本与 CI 行为保持一致，版本固定为 `RESHADE_API_VERSION = 20`（对应 ReShade 6.8.0）。

> ReShade 本体（`ReShade64.dll` / `dxgi.dll`）由使用者自行安装，本仓库不包含、也不分发。

---

## 2. MinHook

| 项 | 内容 |
|---|---|
| 路径 | `third_party/minhook/` |
| 来源 | https://github.com/TsudaKageyu/minhook |
| 作者 | Copyright (C) 2009-2017 Tsuda Kageyu |
| 许可 | **BSD 2-Clause**，全文见 `third_party/minhook/LICENSE.txt` |
| 内嵌第三方 | 包含 Hacker Disassembler Engine（HDE，Vyacheslav Patkov，BSD 风格许可），同样见上述 LICENSE.txt |
| 用途 | 对 `NVSDK_NGX_D3D12_EvaluateFeature` 做 inline hook |

主要条款摘要：保留版权声明与免责声明即可，允许以源码或二进制形式再分发。

---

## 3. NVIDIA NGX / DLSS SDK 头文件

| 项 | 内容 |
|---|---|
| 路径 | `third_party/ngx/nvsdk_ngx.h`、`nvsdk_ngx_defs.h`、`nvsdk_ngx_params.h` |
| 来源 | NVIDIA DLSS SDK（经 https://github.com/NVIDIA/DLSS 公开发布） |
| 版权 | Copyright (c) 2018-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved. |
| 许可 | **LicenseRef-NvidiaProprietary**（NVIDIA SDK 许可，非开源） |
| 用途 | 仅用于编译期声明 `NVSDK_NGX_Result`、`NVSDK_NGX_Parameter` 等类型 |

### 注意

这三个头文件是 NVIDIA 的专有 SDK 头文件，**不是** MIT/BSD 授权。
本仓库包含它们只是为了「clone 下来就能编译」；如果你所在的组织对再分发有更严格的要求，
可以：

1. 从本仓库删除 `third_party/ngx/`；并且
2. 在 `.github/workflows/*.yml` 里加一步，在编译前从 NVIDIA 官方渠道下载这三个头文件
   （与 ReShade 头文件的同步步骤写法相同）。

本仓库**不包含**任何 NVIDIA 的二进制文件（`nvngx_dlss.dll`、`_nvngx.dll` 等），
这些由显卡驱动 / 游戏自带，运行期只做函数地址 hook，不做重分发。
