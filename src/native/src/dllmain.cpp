#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "ue4ss_compat.hpp"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <kiero.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
constexpr char kBuild[] = "pirate-signals-client-1";
constexpr size_t kHistoryLimit = 100;
constexpr size_t kVisibleLines = 1;
constexpr size_t kInputCapacity = 1025;
constexpr uint16_t kExecuteCommandListsIndex = 54;
constexpr uint16_t kPresentIndex = 140;
constexpr uint16_t kResizeBuffersIndex = 145;
constexpr UINT kSrvDescriptorCount = 64;

struct ChatLine
{
    std::string display{};
};

struct ChatState
{
    std::mutex mutex{};
    std::vector<ChatLine> history{};
    std::deque<std::string> submissions{};
    std::string notice{"Connecting"};
    bool ready{};
};

struct FrameContext
{
    ID3D12CommandAllocator* allocator{};
    ID3D12Resource* back_buffer{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    uint64_t fence_value{};
};

ChatState g_chat{};
HMODULE g_module{};
std::atomic_bool g_stop{};
std::atomic_bool g_hook_ready{};
std::atomic_bool g_request_toggle{};
std::atomic_bool g_request_hide{};
std::jthread g_hook_thread{};

std::recursive_mutex g_render_mutex{};
ID3D12Device* g_device{};
ID3D12CommandQueue* g_command_queue{};
ID3D12GraphicsCommandList* g_command_list{};
ID3D12DescriptorHeap* g_rtv_heap{};
ID3D12DescriptorHeap* g_srv_heap{};
ID3D12Fence* g_fence{};
HANDLE g_fence_event{};
std::vector<FrameContext> g_frames{};
std::array<bool, kSrvDescriptorCount> g_srv_used{};
UINT g_srv_stride{};
DXGI_FORMAT g_rtv_format{DXGI_FORMAT_UNKNOWN};
HWND g_window{};
WNDPROC g_original_wndproc{};
ImGuiContext* g_imgui_context{};
bool g_win32_initialized{};
bool g_dx12_initialized{};
bool g_chat_visible{};
bool g_chat_open{};
bool g_focus_input{};
std::array<char, kInputCapacity> g_input{};
uint64_t g_next_fence_value{1};

using ExecuteCommandListsFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain3*, UINT, UINT);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
ExecuteCommandListsFn g_original_execute_command_lists{};
PresentFn g_original_present{};
ResizeBuffersFn g_original_resize_buffers{};

LRESULT CALLBACK chat_wndproc(HWND, UINT, WPARAM, LPARAM);

void log_line(std::string_view message)
{
    wchar_t module_path[MAX_PATH]{};
    if (!g_module || !GetModuleFileNameW(g_module, module_path, MAX_PATH)) return;
    std::wstring path{module_path};
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    path.replace(slash + 1, std::wstring::npos, L"PirateSignalsNative.log");
    std::ofstream output(path, std::ios::app);
    output << "[PirateSignalsNative " << kBuild << "] " << message << '\n';
}

void safe_release(IUnknown*& object)
{
    if (!object) return;
    object->Release();
    object = nullptr;
}

template <typename T>
void safe_release(T*& object)
{
    auto* unknown = static_cast<IUnknown*>(object);
    safe_release(unknown);
    object = nullptr;
}

bool wait_for_frame(FrameContext& frame)
{
    if (frame.fence_value == 0) return true;
    if (!g_fence || !g_fence_event) return false;
    if (g_fence->GetCompletedValue() < frame.fence_value)
    {
        if (FAILED(g_fence->SetEventOnCompletion(frame.fence_value, g_fence_event))) return false;
        if (WaitForSingleObject(g_fence_event, INFINITE) != WAIT_OBJECT_0) return false;
    }
    frame.fence_value = 0;
    return true;
}

void wait_for_all_frames()
{
    for (auto& frame : g_frames) (void)wait_for_frame(frame);
}

void restore_wndproc()
{
    if (g_window && g_original_wndproc && IsWindow(g_window))
    {
        const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_window, GWLP_WNDPROC));
        if (current == chat_wndproc)
            SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
    }
    g_original_wndproc = nullptr;
    g_window = nullptr;
}

void release_renderer_resources(bool destroy_context)
{
    std::scoped_lock lock{g_render_mutex};
    wait_for_all_frames();

    if (g_dx12_initialized)
    {
        ImGui::SetCurrentContext(g_imgui_context);
        ImGui_ImplDX12_Shutdown();
        g_dx12_initialized = false;
    }
    for (auto& frame : g_frames)
    {
        safe_release(frame.back_buffer);
        safe_release(frame.allocator);
    }
    g_frames.clear();
    safe_release(g_command_list);
    safe_release(g_rtv_heap);
    safe_release(g_srv_heap);
    safe_release(g_fence);
    safe_release(g_device);
    g_srv_used.fill(false);
    g_srv_stride = 0;
    g_rtv_format = DXGI_FORMAT_UNKNOWN;
    if (g_fence_event)
    {
        CloseHandle(g_fence_event);
        g_fence_event = nullptr;
    }

    if (destroy_context)
    {
        if (g_win32_initialized)
        {
            ImGui::SetCurrentContext(g_imgui_context);
            ImGui_ImplWin32_Shutdown();
            g_win32_initialized = false;
        }
        restore_wndproc();
        if (g_imgui_context)
        {
            ImGui::DestroyContext(g_imgui_context);
            g_imgui_context = nullptr;
        }
        safe_release(g_command_queue);
    }
}

void allocate_srv(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    if (!g_srv_heap || !cpu || !gpu) return;
    for (UINT index = 0; index < kSrvDescriptorCount; ++index)
    {
        if (g_srv_used[index]) continue;
        g_srv_used[index] = true;
        *cpu = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
        *gpu = g_srv_heap->GetGPUDescriptorHandleForHeapStart();
        cpu->ptr += static_cast<SIZE_T>(index) * g_srv_stride;
        gpu->ptr += static_cast<UINT64>(index) * g_srv_stride;
        return;
    }
    cpu->ptr = 0;
    gpu->ptr = 0;
}

void free_srv(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    if (!g_srv_heap || !g_srv_stride) return;
    const auto start = g_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (cpu.ptr < start) return;
    const auto index = static_cast<UINT>((cpu.ptr - start) / g_srv_stride);
    if (index < kSrvDescriptorCount) g_srv_used[index] = false;
}

LRESULT CALLBACK chat_wndproc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_KEYDOWN && wparam == VK_F8 && (lparam & (1LL << 30)) == 0)
        g_request_toggle.store(true);

    std::scoped_lock lock{g_render_mutex};
    if (g_imgui_context)
    {
        ImGui::SetCurrentContext(g_imgui_context);
        const auto handled = ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        if (g_chat_open)
        {
            const auto& io = ImGui::GetIO();
            const bool keyboard = message == WM_CHAR || message == WM_SYSCHAR ||
                                  (message >= WM_KEYFIRST && message <= WM_KEYLAST);
            const bool mouse = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
            if (handled || (keyboard && io.WantCaptureKeyboard) || (mouse && io.WantCaptureMouse)) return TRUE;
        }
    }
    return g_original_wndproc ? CallWindowProcW(g_original_wndproc, window, message, wparam, lparam)
                              : DefWindowProcW(window, message, wparam, lparam);
}

void style_chat()
{
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 3.0f;
    style.ScrollbarRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.045f, 0.035f, 0.92f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.58f, 0.43f, 0.22f, 0.80f);
    style.Colors[ImGuiCol_Text] = ImVec4(0.93f, 0.88f, 0.76f, 1.0f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.095f, 0.065f, 0.98f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.14f, 0.09f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.17f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.50f, 0.36f, 0.18f, 0.85f);
}

bool initialize_renderer(IDXGISwapChain3* swap_chain)
{
    if (g_dx12_initialized) return true;
    if (!swap_chain || !g_command_queue) return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap_chain->GetDesc(&desc)) || desc.BufferCount == 0 || !desc.OutputWindow) return false;
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&g_device))) || !g_device) return false;
    g_rtv_format = desc.BufferDesc.Format;
    g_window = desc.OutputWindow;

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = desc.BufferCount;
    if (FAILED(g_device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&g_rtv_heap)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = kSrvDescriptorCount;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&g_srv_heap)))) return false;
    g_srv_stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    g_frames.resize(desc.BufferCount);
    auto rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const auto rtv_stride = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT index = 0; index < desc.BufferCount; ++index)
    {
        auto& frame = g_frames[index];
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator)))) return false;
        if (FAILED(swap_chain->GetBuffer(index, IID_PPV_ARGS(&frame.back_buffer)))) return false;
        frame.rtv = rtv;
        g_device->CreateRenderTargetView(frame.back_buffer, nullptr, frame.rtv);
        rtv.ptr += rtv_stride;
    }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator, nullptr,
                                           IID_PPV_ARGS(&g_command_list)))) return false;
    g_command_list->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fence_event) return false;

    if (!g_imgui_context)
    {
        IMGUI_CHECKVERSION();
        g_imgui_context = ImGui::CreateContext();
        ImGui::SetCurrentContext(g_imgui_context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        style_chat();
    }
    ImGui::SetCurrentContext(g_imgui_context);
    if (!g_win32_initialized)
    {
        if (!ImGui_ImplWin32_Init(g_window)) return false;
        g_win32_initialized = true;
        SetLastError(0);
        const auto previous = SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(chat_wndproc));
        if (!previous && GetLastError() != 0) return false;
        g_original_wndproc = reinterpret_cast<WNDPROC>(previous);
    }

    ImGui_ImplDX12_InitInfo info{};
    info.Device = g_device;
    info.CommandQueue = g_command_queue;
    info.NumFramesInFlight = static_cast<int>(desc.BufferCount);
    info.RTVFormat = g_rtv_format;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = g_srv_heap;
    info.SrvDescriptorAllocFn = allocate_srv;
    info.SrvDescriptorFreeFn = free_srv;
    if (!ImGui_ImplDX12_Init(&info)) return false;
    g_dx12_initialized = true;
    log_line("renderer initialized");
    return true;
}

std::string format_line(const ChatLine& line)
{
    return line.display;
}

void queue_submission(std::string message)
{
    while (!message.empty() && (message.back() == ' ' || message.back() == '\t')) message.pop_back();
    const auto first = message.find_first_not_of(" \t");
    if (first == std::string::npos) return;
    if (first > 0) message.erase(0, first);
    std::scoped_lock lock{g_chat.mutex};
    if (g_chat.submissions.size() < 8) g_chat.submissions.emplace_back(std::move(message));
}

void render_chat_window()
{
    if (g_request_hide.exchange(false))
    {
        const bool was_visible = g_chat_visible;
        g_request_toggle.store(false);
        g_chat_visible = false;
        g_chat_open = false;
        g_focus_input = false;
        if (was_visible) log_line("chat hidden by client state");
    }
    else if (g_request_toggle.exchange(false))
    {
        if (!g_chat_visible)
        {
            g_chat_visible = true;
            g_chat_open = false;
            g_focus_input = false;
            log_line("chat minimized line shown with F8");
        }
        else if (!g_chat_open)
        {
            g_chat_open = true;
            g_focus_input = true;
            log_line("chat expanded with F8");
        }
        else
        {
            g_chat_visible = false;
            g_chat_open = false;
            g_focus_input = false;
            log_line("chat hidden with F8");
        }
    }
    if (!g_chat_visible) return;

    std::vector<ChatLine> history{};
    std::string notice{};
    bool ready{};
    {
        std::scoped_lock lock{g_chat.mutex};
        history = g_chat.history;
        notice = g_chat.notice;
        ready = g_chat.ready;
    }

    const auto display = ImGui::GetIO().DisplaySize;
    const float width = std::clamp(display.x * 0.34f, 420.0f, 640.0f);
    const float margin = 28.0f;
    const float top = std::clamp(display.y * 0.09f, 75.0f, 195.0f);
    ImGui::SetNextWindowPos(ImVec2(margin, top), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, g_chat_open ? 285.0f : 0.0f), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (!g_chat_open)
    {
        flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    }

    bool keep_open = true;
    if (ImGui::Begin("Pirate Signals", g_chat_open ? &keep_open : nullptr, flags))
    {
        if (g_chat_open)
        {
            ImGui::TextColored(ready ? ImVec4(0.55f, 0.82f, 0.48f, 1.0f) : ImVec4(0.90f, 0.60f, 0.35f, 1.0f),
                               "%s", ready ? "Connected" : (notice.empty() ? "Connecting" : notice.c_str()));
            ImGui::Separator();
            ImGui::BeginChild("History", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - 6.0f), false,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar);
            const bool was_at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
                ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - ImGui::GetTextLineHeightWithSpacing() * 5.0f));
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
                ImGui::SetScrollY(std::min(ImGui::GetScrollMaxY(), ImGui::GetScrollY() + ImGui::GetTextLineHeightWithSpacing() * 5.0f));
            for (const auto& item : history)
            {
                const auto text = format_line(item);
                ImGui::TextWrapped("%s", text.c_str());
            }
            if (!notice.empty() && !ready)
            {
                ImGui::TextColored(ImVec4(0.85f, 0.66f, 0.38f, 1.0f), "[Chat] %s", notice.c_str());
            }
            if (was_at_bottom || g_focus_input) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();

            ImGui::SetNextItemWidth(-1.0f);
            if (g_focus_input)
            {
                ImGui::SetKeyboardFocusHere();
                g_focus_input = false;
            }
            const auto submitted = ImGui::InputTextWithHint("##Message", "Type a public message...", g_input.data(), g_input.size(),
                                                            ImGuiInputTextFlags_EnterReturnsTrue);
            if (submitted)
            {
                if (g_input[0] != '\0')
                {
                    queue_submission(g_input.data());
                    log_line("message queued with Enter");
                }
                g_input.fill('\0');
                g_focus_input = true;
            }
        }
        else
        {
            const size_t count = std::min(kVisibleLines, history.size());
            const size_t start = history.size() - count;
            for (size_t index = start; index < history.size(); ++index)
            {
                const auto text = format_line(history[index]);
                ImGui::TextUnformatted(text.c_str());
            }
            if (count == 0 && !notice.empty())
            {
                ImGui::TextColored(ImVec4(0.78f, 0.66f, 0.48f, 1.0f), "%s", notice.c_str());
            }
        }
    }
    ImGui::End();
    if (!keep_open)
    {
        g_chat_visible = false;
        g_chat_open = false;
        g_focus_input = false;
        log_line("chat hidden with title-bar button");
    }
}

void render_frame(IDXGISwapChain3* swap_chain)
{
    std::scoped_lock lock{g_render_mutex};
    if (!initialize_renderer(swap_chain) || g_frames.empty()) return;

    ImGui::SetCurrentContext(g_imgui_context);
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    render_chat_window();
    ImGui::Render();

    const auto index = swap_chain->GetCurrentBackBufferIndex();
    if (index >= g_frames.size()) return;
    auto& frame = g_frames[index];
    if (!wait_for_frame(frame)) return;
    if (FAILED(frame.allocator->Reset())) return;
    if (FAILED(g_command_list->Reset(frame.allocator, nullptr))) return;

    D3D12_RESOURCE_BARRIER to_render{};
    to_render.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_render.Transition.pResource = frame.back_buffer;
    to_render.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    to_render.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_render.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_command_list->ResourceBarrier(1, &to_render);
    g_command_list->OMSetRenderTargets(1, &frame.rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[]{g_srv_heap};
    g_command_list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_command_list);
    std::swap(to_render.Transition.StateBefore, to_render.Transition.StateAfter);
    g_command_list->ResourceBarrier(1, &to_render);
    if (FAILED(g_command_list->Close())) return;

    ID3D12CommandList* lists[]{g_command_list};
    g_command_queue->ExecuteCommandLists(1, lists);
    frame.fence_value = g_next_fence_value++;
    g_command_queue->Signal(g_fence, frame.fence_value);
}

void __stdcall hook_execute_command_lists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (!g_command_queue && queue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        std::scoped_lock lock{g_render_mutex};
        if (!g_command_queue)
        {
            queue->AddRef();
            g_command_queue = queue;
            log_line("captured DirectX 12 direct command queue");
        }
    }
    g_original_execute_command_lists(queue, count, lists);
}

HRESULT __stdcall hook_present(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags)
{
    if (!g_stop.load()) render_frame(swap_chain);
    return g_original_present(swap_chain, sync_interval, flags);
}

HRESULT __stdcall hook_resize_buffers(IDXGISwapChain3* swap_chain, UINT count, UINT width, UINT height,
                                      DXGI_FORMAT format, UINT flags)
{
    release_renderer_resources(false);
    return g_original_resize_buffers(swap_chain, count, width, height, format, flags);
}

void hook_worker(std::stop_token stop_token)
{
    while (!stop_token.stop_requested() && !g_stop.load())
    {
        const auto result = kiero::init(kiero::RenderType::D3D12);
        if (result == kiero::Status::Success || result == kiero::Status::AlreadyInitializedError)
        {
            const auto execute_ok = kiero::bind(kExecuteCommandListsIndex,
                reinterpret_cast<void**>(&g_original_execute_command_lists), reinterpret_cast<void*>(hook_execute_command_lists));
            const auto present_ok = kiero::bind(kPresentIndex,
                reinterpret_cast<void**>(&g_original_present), reinterpret_cast<void*>(hook_present));
            const auto resize_ok = kiero::bind(kResizeBuffersIndex,
                reinterpret_cast<void**>(&g_original_resize_buffers), reinterpret_cast<void*>(hook_resize_buffers));
            if (execute_ok == kiero::Status::Success && present_ok == kiero::Status::Success && resize_ok == kiero::Status::Success)
            {
                g_hook_ready.store(true);
                log_line("DirectX 12 hooks installed");
                return;
            }
            log_line("DirectX 12 hook binding failed");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

int lua_native_available(const RC::LuaMadeSimple::Lua& lua)
{
    lua.set_bool(g_hook_ready.load());
    return 1;
}

int lua_set_ready(const RC::LuaMadeSimple::Lua& lua)
{
    const bool ready = lua.get_stack_size() >= 1 && lua.is_bool(1) && lua.get_bool(1);
    std::scoped_lock lock{g_chat.mutex};
    g_chat.ready = ready;
    if (ready) g_chat.notice.clear();
    return 0;
}

int lua_set_notice(const RC::LuaMadeSimple::Lua& lua)
{
    std::string notice{};
    if (lua.get_stack_size() >= 1 && lua.is_string(1)) notice = lua.get_string(1);
    std::scoped_lock lock{g_chat.mutex};
    g_chat.notice = std::move(notice);
    return 0;
}

int lua_hide(const RC::LuaMadeSimple::Lua&)
{
    g_request_hide.store(true);
    return 0;
}

int lua_reset_history(const RC::LuaMadeSimple::Lua&)
{
    std::scoped_lock lock{g_chat.mutex};
    g_chat.history.clear();
    return 0;
}

int lua_add_message(const RC::LuaMadeSimple::Lua& lua)
{
    // Keep this bridge deliberately to one string argument. The UE4SS build in
    // Windrose faults while extracting the previous four mixed arguments.
    if (lua.get_stack_size() < 1 || !lua.is_string(1)) return 0;
    ChatLine line{};
    line.display = lua.get_string(1);
    std::scoped_lock lock{g_chat.mutex};
    g_chat.history.emplace_back(std::move(line));
    if (g_chat.history.size() > kHistoryLimit) g_chat.history.erase(g_chat.history.begin());
    return 0;
}

int lua_take_submission(const RC::LuaMadeSimple::Lua& lua)
{
    std::scoped_lock lock{g_chat.mutex};
    if (g_chat.submissions.empty())
    {
        lua.set_nil();
        return 1;
    }
    auto message = std::move(g_chat.submissions.front());
    g_chat.submissions.pop_front();
    lua.set_string(message);
    return 1;
}

void register_bridge(RC::LuaMadeSimple::Lua& lua)
{
    lua.register_function("WRChatNativeAvailable", lua_native_available);
    lua.register_function("WRChatNativeSetReady", lua_set_ready);
    lua.register_function("WRChatNativeSetNotice", lua_set_notice);
    lua.register_function("WRChatNativeHide", lua_hide);
    lua.register_function("WRChatNativeResetHistory", lua_reset_history);
    lua.register_function("WRChatNativeAddMessage", lua_add_message);
    lua.register_function("WRChatNativeTakeSubmission", lua_take_submission);
}
} // namespace

class WindroseChatNative final : public RC::CppUserModBase
{
public:
    WindroseChatNative()
    {
        ModName = STR("PirateSignals");
        ModVersion = STR("1.0.0");
        ModDescription = STR("Pirate Signals dedicated-server chat");
        ModAuthors = STR("Pirate Signals project");
    }

    ~WindroseChatNative() override
    {
        g_stop.store(true);
        if (g_hook_thread.joinable())
        {
            g_hook_thread.request_stop();
            g_hook_thread.join();
        }
        if (g_hook_ready.exchange(false))
        {
            kiero::unbind(kResizeBuffersIndex);
            kiero::unbind(kPresentIndex);
            kiero::unbind(kExecuteCommandListsIndex);
        }
        release_renderer_resources(true);
        kiero::shutdown();
        log_line("unloaded");
    }

    void on_unreal_init() override
    {
        g_request_hide.store(true);
        if (!g_hook_thread.joinable()) g_hook_thread = std::jthread(hook_worker);
        log_line("loaded hidden; F8=one-line/expanded/hidden Enter=send");
    }

    void on_lua_start(RC::LuaMadeSimple::Lua& lua,
                      RC::LuaMadeSimple::Lua& main_lua,
                      RC::LuaMadeSimple::Lua&,
                      RC::LuaMadeSimple::Lua*) override
    {
        register_bridge(lua);
        if (main_lua.get_lua_state() != lua.get_lua_state()) register_bridge(main_lua);
        log_line("Lua bridge registered");
    }
};

#define WINDROSE_CHAT_NATIVE_API __declspec(dllexport)
extern "C"
{
WINDROSE_CHAT_NATIVE_API RC::CppUserModBase* start_mod()
{
    return new WindroseChatNative();
}

WINDROSE_CHAT_NATIVE_API void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
