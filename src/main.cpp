// =============================================================================
//  ReShade Pre-UI add-on  (UE4 / DX12 / DLSS)
//
//  把 ReShade 的 .fx 效果绘制到「UI 合成之前」的 3D 画面上：
//  UI 不受滤镜影响，效果由游戏自己 Present。
//
//  版本号唯一来源是 CMakeLists.txt 里的 project(... VERSION x.y.z)，
//  由编译期宏 PREUI_VERSION 传入（见下方 #ifndef 兜底）。
//
//  v0.6 日志结论：
//    * 偏移标定每次都是 api=0 —— 因为 device->get_native() 拿到的是真实设备，
//      直接调它的 CreateCommandList 绕过了 ReShade 的代理，不会触发
//      reset_command_list。标定这条路本身不成立。
//    * 但游戏自己的命令列表确实在走 ReShade 代理（reset 事件会到）。
//
//  v0.7（首个公开发布版本）：
//    * 去掉偏移标定（它只会刷屏）。
//    * 查表时直接扫描 native + {0,8,16,...,256}，命中后缓存该偏移。
//      依据：代理类 = ID3D12GraphicsCommandList10(COM, 8字节 vtable)
//                       + command_list_impl(api 基类)，API 指针通常在 +8。
//    * 实测：offset=8，3840x2160 R16G16B16A16_FLOAT，逐帧注入正常。
//
//  v0.7 附带的外围优化（注入热路径一行未改）：
//    * hook 成功后扫描频率从“每 60 帧”降为“每 3600 帧（约 1 分钟）”
//      （原来每秒一次全模块快照，永久执行）。
//    * 关闭 ReShade 效果时直接跳过注入，省掉每帧约 66MB × 2 的全屏拷贝。
//    * g_device 写入挪进 g_temp_mutex；代理偏移改为在锁内读出，消除两处数据竞争。
//    * PreUI.log 超过 4MiB 时在启动阶段自动截断。
//    * AddonUninit 补 MH_RemoveHook + unregister_event。
//    * ensure_temp 增加 0 尺寸防护。
//
//  PreUI.ini（与 .addon64 同目录，可选）：
//      [PreUI]
//      Enabled=1
//      DelayFrames=120
//      Trace=1
// =============================================================================

#ifndef PREUI_VERSION
#define PREUI_VERSION "0.0.0-dev"
#endif

#include <Windows.h>
#include <TlHelp32.h>
#include <d3d12.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <reshade.hpp>

#include "MinHook.h"
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_params.h"

namespace {

using reshade::api::command_list;
using reshade::api::effect_runtime;
using reshade::api::format;
using reshade::api::resource;
using reshade::api::resource_usage;
using reshade::api::resource_view;
using reshade::api::resource_view_desc;

// -----------------------------------------------------------------------------
// 配置与日志
// -----------------------------------------------------------------------------
wchar_t g_base_dir[MAX_PATH] = {};
FILE *g_log_file = nullptr;

bool g_enabled = true;
bool g_trace = true;
int g_delay_frames = 120;

void init_paths(HMODULE self)
{
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0)
        return;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash != nullptr)
    {
        *slash = L'\0';
        wcscpy_s(g_base_dir, path);
    }
}

// 日志只在异常/节流点写，正常不会膨胀；但历史版本刷过几十万行，
// 这里在启动阶段（还没打开日志文件之前）做一次截断，避免无谓地拖大文件。
void rotate_log_if_large()
{
    if (g_base_dir[0] == L'\0')
        return;

    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\PreUI.log", g_base_dir);

    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info))
        return;

    const ULONGLONG size = (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size <= 4ull * 1024ull * 1024ull)
        return;

    FILE *file = nullptr;
    if (_wfopen_s(&file, path, L"w") == 0 && file != nullptr)
    {
        fputs("[PreUI] log rotated (previous log exceeded 4 MiB)\n", file);
        fclose(file);
    }
}

void file_log(const char *fmt, ...)
{
    if (g_log_file == nullptr)
    {
        wchar_t path[MAX_PATH] = {};
        swprintf_s(path, L"%s\\PreUI.log", g_base_dir);
        _wfopen_s(&g_log_file, path, L"a");
        if (g_log_file == nullptr)
        {
            wchar_t temp[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, temp);
            swprintf_s(path, L"%sPreUI.log", temp);
            _wfopen_s(&g_log_file, path, L"a");
        }
        if (g_log_file == nullptr)
            return;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(g_log_file, fmt, args);
    va_end(args);
    fputc('\n', g_log_file);
    fflush(g_log_file);
}

void log_line(reshade::log::level level, const char *fmt, ...)
{
    char buffer[512] = {};
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    reshade::log::message(level, buffer);
}

void load_config()
{
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"%s\\PreUI.ini", g_base_dir);

    const DWORD attr = GetFileAttributesW(path);
    file_log("[PreUI] ini: %ls exists=%d", path, (attr != INVALID_FILE_ATTRIBUTES) ? 1 : 0);

    g_enabled = GetPrivateProfileIntW(L"PreUI", L"Enabled", 1, path) != 0;
    g_trace = GetPrivateProfileIntW(L"PreUI", L"Trace", 1, path) != 0;
    g_delay_frames = static_cast<int>(GetPrivateProfileIntW(L"PreUI", L"DelayFrames", 120, path));
    if (g_delay_frames < 1)
        g_delay_frames = 1;
}

// -----------------------------------------------------------------------------
// 全局状态
// -----------------------------------------------------------------------------
std::mutex g_runtime_mutex;
effect_runtime *g_runtime = nullptr;
reshade::api::device *g_device = nullptr;

std::mutex g_cmd_mutex;
std::unordered_map<uint64_t, command_list *> g_cmd_lists;   // key = api::command_list*

// native(COM) -> api 指针偏移；-1 表示尚未确定
ptrdiff_t g_proxy_offset = -1;

struct temp_target
{
    resource texture{};
    resource_view rtv{};
    resource_view rtv_srgb{};
    uint32_t width = 0;
    uint32_t height = 0;
    format fmt = format::unknown;
    bool ready = false;
};

temp_target g_temp;
std::mutex g_temp_mutex;

using evaluate_feature_fn = NVSDK_NGX_Result (NVSDK_CONV *)(
    ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, void *);

constexpr int kMaxHooks = 8;

evaluate_feature_fn g_originals[kMaxHooks] = {};
int g_slot_count = 0;

std::mutex g_target_mutex;
std::unordered_set<uint64_t> g_hooked_targets;

volatile LONG g_present_count = 0;
volatile LONG g_inject_ok = 0;
volatile LONG g_frame_injected = 0;
volatile LONG g_hook_calls = 0;
volatile LONG g_trace_count = 0;
volatile LONG g_reset_count = 0;
volatile LONG g_hooks_installed = 0;    // 成功 MH_EnableHook 的次数
volatile LONG g_last_hook_frame = 0;    // 最近一次挂上钩子时的 present 帧号
volatile LONG g_disabled_count = 0;     // 效果关闭而跳过注入的帧数（只用于打一次日志）

thread_local int t_call_depth = 0;

void trace(const char *msg)
{
    if (!g_trace)
        return;
    if (InterlockedIncrement(&g_trace_count) <= 60)
        file_log("[PreUI] trace: %s", msg);
}

// -----------------------------------------------------------------------------
// 命令列表记录（按 api 指针）
// -----------------------------------------------------------------------------
void remember_command_list(command_list *cmd_list)
{
    if (cmd_list == nullptr)
        return;
    std::lock_guard<std::mutex> lock(g_cmd_mutex);
    g_cmd_lists[reinterpret_cast<uint64_t>(cmd_list)] = cmd_list;
}

void on_init_command_list(command_list *cmd_list) { remember_command_list(cmd_list); }

void on_reset_command_list(command_list *cmd_list)
{
    const LONG n = InterlockedIncrement(&g_reset_count);
    if (n == 1)
        file_log("[PreUI] first reset_command_list api=%p", cmd_list);
    remember_command_list(cmd_list);
}

void on_close_command_list(command_list *cmd_list) { remember_command_list(cmd_list); }
void on_execute_command_list(reshade::api::command_queue *, command_list *cmd_list) { remember_command_list(cmd_list); }

command_list *resolve_command_list(ID3D12GraphicsCommandList *native, ptrdiff_t *out_offset)
{
    if (native == nullptr)
        return nullptr;

    std::lock_guard<std::mutex> lock(g_cmd_mutex);

    if (g_proxy_offset >= 0)
    {
        auto *candidate = reinterpret_cast<command_list *>(
            reinterpret_cast<char *>(native) + g_proxy_offset);
        const auto it = g_cmd_lists.find(reinterpret_cast<uint64_t>(candidate));
        if (it != g_cmd_lists.end())
        {
            if (out_offset != nullptr)
                *out_offset = g_proxy_offset;
            return it->second;
        }
    }

    for (int offset = 0; offset <= 256; offset += 8)
    {
        auto *candidate = reinterpret_cast<command_list *>(
            reinterpret_cast<char *>(native) + offset);
        const auto it = g_cmd_lists.find(reinterpret_cast<uint64_t>(candidate));
        if (it == g_cmd_lists.end())
            continue;

        if (g_proxy_offset != offset)
        {
            g_proxy_offset = offset;
            file_log("[PreUI] proxy offset resolved: %d (native=%p)", offset, native);
        }
        if (out_offset != nullptr)
            *out_offset = g_proxy_offset;
        return it->second;
    }

    if (out_offset != nullptr)
        *out_offset = g_proxy_offset;
    return nullptr;
}

// -----------------------------------------------------------------------------
// 事件：运行时
// -----------------------------------------------------------------------------
void on_init_effect_runtime(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (g_runtime == nullptr)
        g_runtime = runtime;
    file_log("[PreUI] effect runtime created");
    log_line(reshade::log::level::info, "[PreUI] effect runtime created");
}

void destroy_temp(reshade::api::device *device)
{
    if (device == nullptr || !g_temp.ready)
        return;

    if (g_temp.rtv_srgb.handle != 0 && g_temp.rtv_srgb.handle != g_temp.rtv.handle)
        device->destroy_resource_view(g_temp.rtv_srgb);
    if (g_temp.rtv.handle != 0)
        device->destroy_resource_view(g_temp.rtv);
    if (g_temp.texture.handle != 0)
        device->destroy_resource(g_temp.texture);

    g_temp = {};
}

void on_destroy_effect_runtime(effect_runtime *runtime)
{
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (g_runtime != runtime)
        return;
    g_runtime = nullptr;

    std::lock_guard<std::mutex> temp_lock(g_temp_mutex);
    destroy_temp(g_device);
}

// -----------------------------------------------------------------------------
// 临时纹理
// -----------------------------------------------------------------------------
bool ensure_temp(reshade::api::device *device, uint32_t width, uint32_t height, format fmt)
{
    // 刚进场景/切分辨率时偶尔会读到 0 尺寸，直接跳过，别让 create_resource 刷失败日志。
    if (width == 0 || height == 0)
        return false;

    if (g_temp.ready && g_temp.width == width && g_temp.height == height && g_temp.fmt == fmt)
        return true;

    destroy_temp(device);

    const reshade::api::resource_desc desc(
        width, height, 1, 1, fmt, 1,
        reshade::api::memory_heap::default_,
        resource_usage::render_target | resource_usage::copy_source | resource_usage::copy_dest);

    resource texture{};
    if (!device->create_resource(desc, nullptr, resource_usage::general, &texture))
    {
        file_log("[PreUI] create_resource(%ux%u) failed", width, height);
        return false;
    }

    resource_view rtv{};
    if (!device->create_resource_view(texture, resource_usage::render_target, resource_view_desc(fmt), &rtv))
    {
        device->destroy_resource(texture);
        file_log("[PreUI] create RTV failed");
        return false;
    }

    resource_view rtv_srgb = rtv;
    const format srgb_fmt = reshade::api::format_to_default_typed(fmt, 1);
    if (srgb_fmt != fmt)
        device->create_resource_view(texture, resource_usage::render_target, resource_view_desc(srgb_fmt), &rtv_srgb);

    g_temp.texture = texture;
    g_temp.rtv = rtv;
    g_temp.rtv_srgb = rtv_srgb;
    g_temp.width = width;
    g_temp.height = height;
    g_temp.fmt = fmt;
    g_temp.ready = true;
    file_log("[PreUI] temp target created %ux%u fmt=%d", width, height, static_cast<int>(fmt));
    return true;
}

// -----------------------------------------------------------------------------
// 核心注入
// -----------------------------------------------------------------------------
void inject_pre_ui(ID3D12GraphicsCommandList *native_cmd, const NVSDK_NGX_Parameter *params)
{
    effect_runtime *runtime = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        runtime = g_runtime;
    }
    if (runtime == nullptr)
    {
        trace("no runtime");
        return;
    }
    if (native_cmd == nullptr || params == nullptr)
    {
        trace("null cmd/params");
        return;
    }

    // 效果关闭时注入只是白白做两次全屏拷贝，直接跳过。
    // 注意：这不会改变画面，因为此时 ReShade 在 Present 阶段也不会做任何处理。
    if (!runtime->get_effects_state())
    {
        if (InterlockedIncrement(&g_disabled_count) == 1)
            file_log("[PreUI] effects are disabled, skipping injection until re-enabled");
        return;
    }

    ID3D12Resource *output = nullptr;
    const NVSDK_NGX_Result get_result = params->Get(NVSDK_NGX_Parameter_Output, &output);
    if (NVSDK_NGX_FAILED(get_result) || output == nullptr)
    {
        char msg[128];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "Get(Output) res=0x%08X ptr=%p",
                    static_cast<unsigned int>(get_result), output);
        trace(msg);
        return;
    }

    ptrdiff_t proxy_offset = -1;
    command_list *cmd_list = resolve_command_list(native_cmd, &proxy_offset);
    if (cmd_list == nullptr)
    {
        char msg[200];
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(g_cmd_mutex);
            count = g_cmd_lists.size();
        }
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "no cmd list native=%p offset=%td registered=%zu reset=%ld",
                    native_cmd, proxy_offset, count, g_reset_count);
        trace(msg);
        return;
    }

    reshade::api::device *device = runtime->get_command_queue()->get_device();
    if (device == nullptr)
    {
        trace("no device");
        return;
    }

    const resource target{reinterpret_cast<uint64_t>(output)};
    const reshade::api::resource_desc target_desc = device->get_resource_desc(target);
    if (target_desc.type != reshade::api::resource_type::texture_2d || target_desc.texture.samples > 1)
    {
        char msg[128];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "bad desc type=%d samples=%u",
                    static_cast<int>(target_desc.type), target_desc.texture.samples);
        trace(msg);
        return;
    }

    const format linear_fmt = reshade::api::format_to_default_typed(target_desc.texture.format, 0);
    if (linear_fmt == format::unknown)
    {
        trace("unknown format");
        return;
    }

    std::lock_guard<std::mutex> temp_lock(g_temp_mutex);
    g_device = device;   // 与 on_destroy_effect_runtime 的读取共用一把锁
    if (!ensure_temp(device, target_desc.texture.width, target_desc.texture.height, linear_fmt))
    {
        trace("ensure_temp failed");
        return;
    }

    if (InterlockedCompareExchange(&g_frame_injected, 1, 0) != 0)
    {
        trace("already injected this frame");
        return;
    }

    const resource_usage state_general = resource_usage::general;
    const resource_usage state_copy_src = resource_usage::copy_source;
    const resource_usage state_copy_dst = resource_usage::copy_dest;
    const resource_usage state_rt = resource_usage::render_target;

    cmd_list->barrier(1, &target, &state_general, &state_copy_src);
    cmd_list->barrier(1, &g_temp.texture, &state_general, &state_copy_dst);
    cmd_list->copy_texture_region(target, 0, nullptr, g_temp.texture, 0, nullptr);

    cmd_list->barrier(1, &g_temp.texture, &state_copy_dst, &state_rt);
    runtime->render_effects(cmd_list, g_temp.rtv, g_temp.rtv_srgb);

    cmd_list->barrier(1, &g_temp.texture, &state_rt, &state_copy_src);
    cmd_list->barrier(1, &target, &state_copy_src, &state_copy_dst);
    cmd_list->copy_texture_region(g_temp.texture, 0, nullptr, target, 0, nullptr);

    cmd_list->barrier(1, &g_temp.texture, &state_copy_src, &state_general);
    cmd_list->barrier(1, &target, &state_copy_dst, &state_general);

    const LONG count = InterlockedIncrement(&g_inject_ok);
    if (count == 1 || (count % 600) == 0)
    {
        file_log("[PreUI] injected %ld frames (%ux%u fmt=%d)",
                 count, target_desc.texture.width, target_desc.texture.height,
                 static_cast<int>(linear_fmt));
        log_line(reshade::log::level::info, "[PreUI] injected %ld frames", count);
    }
}

// -----------------------------------------------------------------------------
// 多层 hook：只在最外层（depth==1）注入
// -----------------------------------------------------------------------------
template <int I>
NVSDK_NGX_Result NVSDK_CONV hook_thunk(
    ID3D12GraphicsCommandList *cmd_list,
    const NVSDK_NGX_Handle *handle,
    const NVSDK_NGX_Parameter *params,
    void *callback)
{
    ++t_call_depth;

    const LONG call = InterlockedIncrement(&g_hook_calls);
    if (call <= 10)
        file_log("[PreUI] slot[%d] entered (call=%ld depth=%d)", I, call, t_call_depth);

    const NVSDK_NGX_Result result = g_originals[I](cmd_list, handle, params, callback);

    if (NVSDK_NGX_SUCCEED(result) && t_call_depth == 1)
    {
        __try
        {
            inject_pre_ui(cmd_list, params);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            file_log("[PreUI] exception during injection (slot %d)", I);
        }
    }

    --t_call_depth;
    return result;
}

LPVOID thunk_for(int index)
{
    static const LPVOID table[kMaxHooks] = {
        reinterpret_cast<LPVOID>(&hook_thunk<0>),
        reinterpret_cast<LPVOID>(&hook_thunk<1>),
        reinterpret_cast<LPVOID>(&hook_thunk<2>),
        reinterpret_cast<LPVOID>(&hook_thunk<3>),
        reinterpret_cast<LPVOID>(&hook_thunk<4>),
        reinterpret_cast<LPVOID>(&hook_thunk<5>),
        reinterpret_cast<LPVOID>(&hook_thunk<6>),
        reinterpret_cast<LPVOID>(&hook_thunk<7>),
    };
    return (index >= 0 && index < kMaxHooks) ? table[index] : nullptr;
}

bool is_ngx_module(const wchar_t *name)
{
    return _wcsicmp(name, L"nvngx_dlss.dll") == 0 ||
           _wcsicmp(name, L"nvngx.dll") == 0 ||
           _wcsicmp(name, L"_nvngx.dll") == 0;
}

void hook_one_module(HMODULE module, const wchar_t *module_name, const wchar_t *module_path)
{
    if (module == nullptr)
        return;

    const char *exports[] = {"NVSDK_NGX_D3D12_EvaluateFeature", "NVSDK_NGX_D3D12_EvaluateFeature_C"};

    for (const char *export_name : exports)
    {
        void *target = reinterpret_cast<void *>(GetProcAddress(module, export_name));
        if (target == nullptr)
            continue;

        int slot = 0;
        {
            std::lock_guard<std::mutex> lock(g_target_mutex);
            if (g_hooked_targets.count(reinterpret_cast<uint64_t>(target)) != 0)
                continue;
            if (g_slot_count >= kMaxHooks)
            {
                file_log("[PreUI] no free hook slot");
                return;
            }
            slot = g_slot_count++;
        }

        file_log("[PreUI] target %ls!%s -> %p (%ls)", module_name, export_name, target, module_path);

        MH_STATUS status = MH_CreateHook(target, thunk_for(slot),
                                         reinterpret_cast<LPVOID *>(&g_originals[slot]));
        if (status != MH_OK)
        {
            file_log("[PreUI] MH_CreateHook failed: %d", static_cast<int>(status));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_target_mutex);
            g_hooked_targets.insert(reinterpret_cast<uint64_t>(target));
        }

        status = MH_EnableHook(target);
        file_log("[PreUI] MH_EnableHook -> %d", static_cast<int>(status));
        if (status == MH_OK)
        {
            InterlockedIncrement(&g_hooks_installed);
            InterlockedExchange(&g_last_hook_frame, g_present_count);
        }
    }
}

void scan_and_hook()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
    {
        if (!is_ngx_module(entry.szModule))
            continue;
        hook_one_module(entry.hModule, entry.szModule, entry.szExePath);
    }

    CloseHandle(snapshot);
}

void on_reshade_present(effect_runtime *runtime)
{
    (void)runtime;

    InterlockedExchange(&g_frame_injected, 0);

    if (!g_enabled)
        return;

    const LONG frame = InterlockedIncrement(&g_present_count);
    if (frame < g_delay_frames)
    {
        if (frame == 1)
            file_log("[PreUI] delaying hook for %d frames", g_delay_frames);
        return;
    }

    if (frame != g_delay_frames && (frame % 60) != 0)
        return;

    // 钩子挂上之后，把“每 60 帧枚举一次模块”降频到“每 3600 帧（约 1 分钟）一次”：
    // 既避免每秒一次全进程模块快照的开销，又保留了模块被重载后重新挂钩的能力。
    // 如果一直没挂上（例如 DLSS 稍后才开），则维持每 60 帧扫一次。
    if (InterlockedCompareExchange(&g_hooks_installed, 0, 0) != 0)
    {
        const LONG hooked_at = InterlockedCompareExchange(&g_last_hook_frame, 0, 0);
        if (frame > hooked_at + 3600 && (frame % 3600) != 0)
            return;
    }

    __try
    {
        scan_and_hook();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        file_log("[PreUI] exception while scanning modules");
    }
}

} // namespace

// -----------------------------------------------------------------------------
// Add-on 入口
// -----------------------------------------------------------------------------
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
    init_paths(addon_module);
    rotate_log_if_large();
    file_log("[PreUI] ===== AddonInit v%s =====", PREUI_VERSION);
    file_log("[PreUI] module dir: %ls", g_base_dir);
    load_config();
    file_log("[PreUI] enabled=%d delay=%d trace=%d",
             g_enabled ? 1 : 0, g_delay_frames, g_trace ? 1 : 0);

    if (!reshade::register_addon(addon_module, reshade_module))
    {
        file_log("[PreUI] register_addon failed");
        return false;
    }

    if (g_enabled)
    {
        reshade::register_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&on_destroy_effect_runtime);
        reshade::register_event<reshade::addon_event::init_command_list>(&on_init_command_list);
        reshade::register_event<reshade::addon_event::reset_command_list>(&on_reset_command_list);
        reshade::register_event<reshade::addon_event::close_command_list>(&on_close_command_list);
        reshade::register_event<reshade::addon_event::execute_command_list>(&on_execute_command_list);
        reshade::register_event<reshade::addon_event::reshade_present>(&on_reshade_present);

        const MH_STATUS status = MH_Initialize();
        file_log("[PreUI] MH_Initialize -> %d", static_cast<int>(status));
    }

    log_line(reshade::log::level::info, "[PreUI] add-on loaded (enabled=%d)", g_enabled ? 1 : 0);
    file_log("[PreUI] AddonInit done");
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
    file_log("[PreUI] ===== AddonUninit =====");

    if (g_slot_count > 0)
    {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_RemoveHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_slot_count = 0;
    }

    if (g_enabled)
    {
        // 先把回调摘掉再注销 add-on，避免 ReShade 之后仍调用到已卸载模块里的函数。
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(&on_init_effect_runtime);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&on_destroy_effect_runtime);
        reshade::unregister_event<reshade::addon_event::init_command_list>(&on_init_command_list);
        reshade::unregister_event<reshade::addon_event::reset_command_list>(&on_reset_command_list);
        reshade::unregister_event<reshade::addon_event::close_command_list>(&on_close_command_list);
        reshade::unregister_event<reshade::addon_event::execute_command_list>(&on_execute_command_list);
        reshade::unregister_event<reshade::addon_event::reshade_present>(&on_reshade_present);
    }

    reshade::unregister_addon(addon_module, reshade_module);
}
