#include "app.h"
#include "ini.h"
#include <windef.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_internal.h"
#include "motion.h"
#include "misc/cpp/imgui_stdlib.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <commdlg.h>
#include <cstddef>
#include <cstring>
#include <dwmapi.h>
#include <format>
#include <shellapi.h>
#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace simple_notepad
{
    constexpr float BASE_FONT_SIZE = 16.0f;
    constexpr float MENU_FONT_SIZE = 17.0f;
    constexpr float MENU_PADDING_Y = 11.0f;
    constexpr float EDITOR_FONT_SIZE = 17.0f;
    constexpr float MIN_ZOOM = 0.5f;
    constexpr float MAX_ZOOM = 4.0f;
    constexpr float MIN_WINDOW_WIDTH = 480.0f; // unscaled pixels
    constexpr float MIN_WINDOW_HEIGHT = 300.0f;
    constexpr double TOAST_SECONDS = 2.2;
    constexpr double MODAL_FADE_SECONDS = 0.14;
    constexpr std::uintmax_t MAX_FILE_BYTES = std::uintmax_t{1024} * 1024;
    constexpr ImVec4 CLEAR_COLOR{0.122f, 0.122f, 0.122f, 1.0f};
    constexpr COLORREF WINDOW_BRUSH = RGB(31, 31, 31);

    static void apply_theme(ImGuiStyle& style)
    {
        style.WindowPadding = {12, 10};
        style.FramePadding = {10, 6};
        style.ItemSpacing = {8, 6};
        style.ItemInnerSpacing = {6, 4};
        style.ScrollbarSize = 10;
        style.WindowRounding = 10;
        style.ChildRounding = 8;
        style.FrameRounding = 6;
        style.PopupRounding = 8;
        style.ScrollbarRounding = 6;
        style.GrabRounding = 6;
        style.WindowBorderSize = 0;
        style.ChildBorderSize = 0;
        style.PopupBorderSize = 1;
        style.FrameBorderSize = 0;
        style.SelectableTextAlign = {0.0f, 0.5f};

        const ImVec4 bg = CLEAR_COLOR; // vs code dark modern
        const ImVec4 panel{0.094f, 0.094f, 0.094f, 1.0f};
        const ImVec4 raised{0.192f, 0.192f, 0.192f, 1.0f};
        const ImVec4 hover{0.165f, 0.176f, 0.180f, 1.0f};
        const ImVec4 accent{0.000f, 0.471f, 0.831f, 1.0f};
        const ImVec4 text{0.800f, 0.800f, 0.800f, 1.0f};

        ImVec4* c = style.Colors;
        c[ImGuiCol_Text] = text;
        c[ImGuiCol_TextDisabled] = {0.431f, 0.463f, 0.506f, 1.0f};
        c[ImGuiCol_WindowBg] = bg;
        c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
        c[ImGuiCol_PopupBg] = bg;
        c[ImGuiCol_Border] = {0.169f, 0.169f, 0.169f, 1.0f};
        c[ImGuiCol_FrameBg] = raised;
        c[ImGuiCol_FrameBgHovered] = hover;
        c[ImGuiCol_FrameBgActive] = hover;
        c[ImGuiCol_MenuBarBg] = panel;
        c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
        c[ImGuiCol_ScrollbarGrab] = {1, 1, 1, 0.10f};
        c[ImGuiCol_ScrollbarGrabHovered] = {1, 1, 1, 0.18f};
        c[ImGuiCol_ScrollbarGrabActive] = {accent.x, accent.y, accent.z, 0.6f};
        c[ImGuiCol_CheckMark] = accent;
        c[ImGuiCol_Button] = raised;
        c[ImGuiCol_ButtonHovered] = hover;
        c[ImGuiCol_ButtonActive] = {accent.x, accent.y, accent.z, 0.45f};
        c[ImGuiCol_Header] = {accent.x, accent.y, accent.z, 0.22f};
        c[ImGuiCol_HeaderHovered] = {accent.x, accent.y, accent.z, 0.30f};
        c[ImGuiCol_HeaderActive] = {accent.x, accent.y, accent.z, 0.40f};
        c[ImGuiCol_Separator] = {0.169f, 0.169f, 0.169f, 1.0f};
        c[ImGuiCol_TextSelectedBg] = {0.149f, 0.310f, 0.471f, 1.0f};
        c[ImGuiCol_InputTextCursor] = {0.682f, 0.686f, 0.678f, 1.0f};
        c[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, 0.45f};
        c[ImGuiCol_NavCursor] = {0, 0, 0, 0};
    }

    // ---------------------------------------------------------------- lifetime

    int app::run(const std::wstring& open_path)
    {
        ImGui_ImplWin32_EnableDpiAwareness();
        load_settings();

        const WNDCLASSEXW wc{
            .cbSize = sizeof(WNDCLASSEXW),
            .lpfnWndProc = window_proc,
            .hInstance = GetModuleHandleW(nullptr),
            .hIcon = LoadIconW(nullptr, IDI_APPLICATION),
            .hCursor = LoadCursorW(nullptr, IDC_ARROW),
            .hbrBackground = CreateSolidBrush(WINDOW_BRUSH),
            .lpszClassName = L"simple_notepad",
        };
        RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Simple Notepad", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 960, 640, nullptr, nullptr, wc.hInstance, this);
        if (hwnd_ == nullptr || !create_device())
        {
            MessageBoxW(nullptr, L"Could not create a Direct3D 11 device.", L"Simple Notepad", MB_ICONERROR);
            return 1;
        }

        // creation computes the frame before our caption-less layout applies
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImGui_ImplWin32_Init(hwnd_);
        ImGui_ImplDX11_Init(device_.Get(), context_.Get());

        // system fonts keep the exe tiny; the ui font doubles as the editor's fallback for non-latin glyphs
        std::wstring windows_dir(MAX_PATH, L'\0');
        windows_dir.resize(GetWindowsDirectoryW(windows_dir.data(), MAX_PATH));
        const std::filesystem::path fonts = std::filesystem::path(windows_dir) / L"Fonts";
        const std::string ui_path = text_file::to_utf8((fonts / L"segoeui.ttf").wstring());
        const std::string mono_path = text_file::to_utf8((fonts / L"consola.ttf").wstring());
        const bool have_ui = std::filesystem::exists(fonts / L"segoeui.ttf");
        ui_font_ = have_ui ? io.Fonts->AddFontFromFileTTF(ui_path.c_str(), BASE_FONT_SIZE) : io.Fonts->AddFontDefault();
        editor_font_ = std::filesystem::exists(fonts / L"consola.ttf")
                           ? io.Fonts->AddFontFromFileTTF(mono_path.c_str(), EDITOR_FONT_SIZE)
                           : ui_font_;
        if (have_ui && editor_font_ != ui_font_)
        {
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(ui_path.c_str(), EDITOR_FONT_SIZE, &merge);
        }

        set_dpi(ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd_));
        zoom_shown_ = settings_.zoom;

        if (!open_path.empty())
        {
            // a relative argument would otherwise follow the working directory, which the file dialogs change
            std::error_code ec;
            const std::filesystem::path full = std::filesystem::absolute(open_path, ec);
            open(ec ? std::filesystem::path(open_path) : full);
        }
        update_title();
        ready_ = true;
        ShowWindow(hwnd_, SW_SHOWNORMAL);

        // event-driven loop: sleeps until input arrives, renders only while something changes
        while (running_)
        {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                {
                    running_ = false;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (!running_)
            {
                break;
            }
            const bool minimized = IsIconic(hwnd_) != FALSE;
            if (minimized || (frames_ <= 0 && !animating()))
            {
                // wake exactly when the caret blink flips
                const DWORD wait = (!minimized && focused_ && editor_.focused())
                                       ? static_cast<DWORD>(editor_.seconds_until_blink() * 1000.0) + 1
                                       : INFINITE;
                if (MsgWaitForMultipleObjects(0, nullptr, FALSE, wait, QS_ALLINPUT) == WAIT_TIMEOUT)
                {
                    frames_ = 1;
                }
                continue;
            }
            frame();
            --frames_;
        }

        save_settings();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        target_.Reset();
        if (frame_ready_ != nullptr)
        {
            CloseHandle(frame_ready_);
        }
        swap_chain_.Reset();
        context_.Reset();
        device_.Reset();
        DestroyWindow(hwnd_);
        DeleteObject(wc.hbrBackground);
        return 0;
    }

    bool app::create_device()
    {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd_;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        constexpr std::array levels{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        const auto create = [&](D3D_DRIVER_TYPE type) {
            return D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, levels.data(),
                                                 static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &sd, &swap_chain_,
                                                 &device_, nullptr, &context_);
        };
        if (FAILED(create(D3D_DRIVER_TYPE_HARDWARE)) && FAILED(create(D3D_DRIVER_TYPE_WARP)))
        {
            return false;
        }
        create_target();
        // the loop sleeps on this handle until the gpu can take a frame, instead of blocking inside present
        Microsoft::WRL::ComPtr<IDXGISwapChain2> chain2;
        if (SUCCEEDED(swap_chain_.As(&chain2)))
        {
            chain2->SetMaximumFrameLatency(1);
            frame_ready_ = chain2->GetFrameLatencyWaitableObject();
        }
        return true;
    }

    void app::create_target()
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
        swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back));
        device_->CreateRenderTargetView(back.Get(), nullptr, &target_);
    }

    void app::resize(UINT width, UINT height)
    {
        if (!swap_chain_ || width == 0 || height == 0)
        {
            return;
        }
        presented_.clear(); // new buffers start undefined
        target_.Reset();
        swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                   DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        create_target();
    }

    void app::set_dpi(float scale)
    {
        dpi_scale_ = scale;
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();
        apply_theme(style);
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
    }

    // ---------------------------------------------------------------- win32

    LRESULT CALLBACK app::window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
    {
        if (msg == WM_NCCREATE)
        {
            auto* created = static_cast<app*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            created->hwnd_ = hwnd; // handle_message needs it before CreateWindowExW returns
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        }
        auto* self = reinterpret_cast<app*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self != nullptr ? self->handle_message(msg, wparam, lparam) : DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    LRESULT app::handle_message(UINT msg, WPARAM wparam, LPARAM lparam)
    {
        frames_ = 2; // one to react, one for imgui to settle hover/focus changes
        switch (msg)
        {
        case WM_KEYDOWN:
            // after a find, enter keeps jumping to the next match instead of replacing the selection
            if (wparam == VK_RETURN && find_cycling_)
            {
                find_next(GetKeyState(VK_SHIFT) < 0);
                return 0;
            }
            if (wparam != VK_F3 && wparam != VK_SHIFT)
            {
                find_cycling_ = false;
            }
            break;
        case WM_CHAR:
            if (wparam == '\r' && find_cycling_)
            {
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            find_cycling_ = false;
            break;
        case WM_MOUSEWHEEL:
            if (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL)
            {
                set_zoom(settings_.zoom + (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 0.1f : -0.1f));
                return 0;
            }
            break;
        default:
            break;
        }

        if (ImGui::GetCurrentContext() != nullptr && ImGui_ImplWin32_WndProcHandler(hwnd_, msg, wparam, lparam))
        {
            return 1;
        }

        switch (msg)
        {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED)
            {
                resize(LOWORD(lparam), HIWORD(lparam));
                // draw during the modal resize loop so the window never shows stale or stretched content
                if (ready_ && !in_frame_)
                {
                    frame();
                }
            }
            return 0;
        case WM_NCCALCSIZE:
            if (wparam != FALSE)
            {
                // keep the standard side and bottom frames (resize edges, shadow) but take over the caption strip
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                const LONG top = params->rgrc[0].top;
                DefWindowProcW(hwnd_, msg, wparam, lparam);
                params->rgrc[0].top = top;
                if (IsZoomed(hwnd_) != FALSE)
                {
                    // a maximized window hangs its frame off-screen; keep the menu bar on it
                    const UINT dpi = GetDpiForWindow(hwnd_);
                    params->rgrc[0].top +=
                        GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                }
                return 0;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const float scale = static_cast<float>(GetDpiForWindow(hwnd_)) / USER_DEFAULT_SCREEN_DPI;
            info->ptMinTrackSize = {.x = static_cast<LONG>(MIN_WINDOW_WIDTH * scale),
                                    .y = static_cast<LONG>(MIN_WINDOW_HEIGHT * scale)};
            return 0;
        }
        case WM_NCHITTEST: {
            const LRESULT hit = DefWindowProcW(hwnd_, msg, wparam, lparam);
            return hit == HTCLIENT ? hit_test(lparam) : hit;
        }
        case WM_NCRBUTTONUP: {
            // right-clicking the title opens our menu; the rest of the caption keeps the system menu
            POINT p{.x = GET_X_LPARAM(lparam), .y = GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &p);
            if (wparam == HTCAPTION && PtInRect(&title_, p) != FALSE)
            {
                title_menu_ = true;
                return 0;
            }
            break;
        }
        case WM_ACTIVATE:
            focused_ = LOWORD(wparam) != WA_INACTIVE;
            break;
        case WM_DPICHANGED: {
            set_dpi(static_cast<float>(HIWORD(wparam)) / USER_DEFAULT_SCREEN_DPI);
            const auto* rect = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DROPFILES: {
            auto* const drop = reinterpret_cast<HDROP>(wparam);
            std::wstring file(DragQueryFileW(drop, 0, nullptr, 0) + 1, L'\0');
            file.resize(DragQueryFileW(drop, 0, file.data(), static_cast<UINT>(file.size())));
            DragFinish(drop);
            dropped_path_ = std::move(file);
            request(action::open_dropped);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_KEYMENU) // no alt-menu beep
            {
                return 0;
            }
            break;
        case WM_CLOSE:
            request(action::exit);
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, msg, wparam, lparam);
    }

    // ---------------------------------------------------------------- frame

    bool app::animating() const
    {
        const double now = ImGui::GetTime();
        return zoom_shown_ != settings_.zoom || find_anim_ != (find_open_ ? 1.0f : 0.0f) || editor_.animating() ||
               now - toast_time_ < TOAST_SECONDS ||
               (pending_ != action::none && now - modal_time_ < MODAL_FADE_SECONDS);
    }

    bool app::dirty() const
    {
        return format_changed_ || editor_.state() != saved_state_;
    }

    void app::frame()
    {
        in_frame_ = true;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // snaps: animating the size rebakes glyphs, uploads the atlas and relayouts every line each frame
        zoom_shown_ = settings_.zoom;
        find_anim_ = motion::approach(find_anim_, find_open_ ? 1.0f : 0.0f, 16.0f, 0.002f);
        handle_shortcuts();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        // imgui sizes the menu bar from the font and padding active at Begin
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, MENU_PADDING_Y) * dpi_scale_);
        ImGui::PushFont(nullptr, MENU_FONT_SIZE);
        constexpr ImGuiWindowFlags root_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                                                ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("##root", nullptr, root_flags);
        ImGui::PopFont();
        ImGui::PopStyleVar(3);

        draw_menu();
        draw_find_bar();
        draw_editor();
        draw_status_bar();
        draw_unsaved_modal();

        ImGui::End();
        ImGui::Render();

        // hover-settle and blink-free frames often draw what is already on screen and presenting costs far more than
        // comparing; animation frames always differ, so they skip the compare
        const bool animated = animating();
        if (animated)
        {
            presented_.clear(); // the screen will no longer match the recorded copy
        }
        const bool unchanged = !animated && same_as_presented();
        if (unchanged)
        {
            update_title();
            in_frame_ = false;
            // a fast mouse sends ~1000 messages a second; without this wait each one would build a frame
            DwmFlush();
            return;
        }
        if (frame_ready_ != nullptr)
        {
            WaitForSingleObjectEx(frame_ready_, 1000, TRUE);
        }
        context_->OMSetRenderTargets(1, target_.GetAddressOf(), nullptr);
        context_->ClearRenderTargetView(target_.Get(), &CLEAR_COLOR.x);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap_chain_->Present(1, 0);

        update_title();
        in_frame_ = false;
    }

    // compares this frame's draw data with the last presented copy and records it; memcmp is far cheaper than hashing
    bool app::same_as_presented()
    {
        const ImDrawData* data = ImGui::GetDrawData();
        // pending font atlas uploads must reach the gpu, so never skip those frames
        if (data->Textures != nullptr && std::ranges::any_of(*data->Textures, [](const ImTextureData* t) {
                return t->Status != ImTextureStatus_OK;
            }))
        {
            presented_.clear();
            return false;
        }
        size_t at = 0;
        bool same = true;
        const auto take = [&](const void* bytes, size_t size) {
            if (same && at + size <= presented_.size() && std::memcmp(presented_.data() + at, bytes, size) == 0)
            {
                at += size;
                return;
            }
            same = false;
            presented_.resize(at + size);
            std::memcpy(presented_.data() + at, bytes, size);
            at += size;
        };
        take(&data->DisplaySize, sizeof(data->DisplaySize));
        for (const ImDrawList* list : data->CmdLists)
        {
            take(list->VtxBuffer.Data, list->VtxBuffer.size_in_bytes());
            take(list->IdxBuffer.Data, list->IdxBuffer.size_in_bytes());
            take(list->CmdBuffer.Data, list->CmdBuffer.size_in_bytes());
        }
        same = same && at == presented_.size();
        presented_.resize(at);
        return same;
    }

    void app::handle_shortcuts()
    {
        constexpr ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, global))
        {
            request(action::new_file);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global))
        {
            request(action::open_file);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, global))
        {
            save(false);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, global))
        {
            save(true);
        }
        const bool open_find = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, global);
        const bool open_replace = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_H, global);
        if (open_find || open_replace)
        {
            const auto [lo, hi] = editor_.selection();
            const std::string_view selected = std::string_view(editor_.text()).substr(lo, hi - lo);
            if (!selected.empty() && selected.find('\n') == std::string_view::npos)
            {
                find_text_ = selected;
            }
            find_open_ = find_focus_ = true;
            show_replace_ = open_replace;
        }
        if (ImGui::Shortcut(ImGuiKey_F3, global))
        {
            find_next(false);
        }
        if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_F3, global))
        {
            find_next(true);
        }
        if (ImGui::Shortcut(ImGuiKey_F5, global) && editor_.focused())
        {
            const auto now = std::chrono::current_zone()->to_local(std::chrono::system_clock::now());
            editor_.insert(std::format("{:%H:%M %d/%m/%Y}", std::chrono::floor<std::chrono::seconds>(now)));
        }
        if (find_open_ && editor_.focused() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            find_open_ = false;
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Equal, global) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, global))
        {
            set_zoom(settings_.zoom + 0.1f);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Minus, global) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, global))
        {
            set_zoom(settings_.zoom - 0.1f);
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_0, global))
        {
            set_zoom(1.0f);
        }
        if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_Z, global))
        {
            settings_.wrap = !settings_.wrap;
        }
    }

    void app::draw_menu()
    {
        ImGui::PushFont(nullptr, MENU_FONT_SIZE);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, MENU_PADDING_Y) * dpi_scale_);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8) * dpi_scale_);
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New", "Ctrl+N"))
                {
                    request(action::new_file);
                }
                if (ImGui::MenuItem("Open...", "Ctrl+O"))
                {
                    request(action::open_file);
                }
                if (ImGui::MenuItem("Save", "Ctrl+S"))
                {
                    save(false);
                }
                if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
                {
                    save(true);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4"))
                {
                    request(action::exit);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit"))
            {
                if (ImGui::MenuItem("Find", "Ctrl+F"))
                {
                    find_open_ = find_focus_ = true;
                    show_replace_ = false;
                }
                if (ImGui::MenuItem("Replace", "Ctrl+H"))
                {
                    find_open_ = find_focus_ = true;
                    show_replace_ = true;
                }
                if (ImGui::MenuItem("Find Next", "F3"))
                {
                    find_next(false);
                }
                if (ImGui::MenuItem("Find Previous", "Shift+F3"))
                {
                    find_next(true);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View"))
            {
                ImGui::MenuItem("Word Wrap", "Alt+Z", &settings_.wrap);
                ImGui::MenuItem("Minimap", nullptr, &settings_.minimap);
                ImGui::Separator();
                if (ImGui::MenuItem("Zoom In", "Ctrl+="))
                {
                    set_zoom(settings_.zoom + 0.1f);
                }
                if (ImGui::MenuItem("Zoom Out", "Ctrl+-"))
                {
                    set_zoom(settings_.zoom - 0.1f);
                }
                if (ImGui::MenuItem("Reset Zoom", "Ctrl+0"))
                {
                    set_zoom(1.0f);
                }
                ImGui::EndMenu();
            }
            draw_caption();
            ImGui::EndMenuBar();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
    }

    // the menu bar doubles as the title bar: title, drag area and window buttons
    void app::draw_caption()
    {
        const ImRect bar = ImGui::GetCurrentWindow()->MenuBarRect();
        const float button_w = 46.0f * dpi_scale_;
        const float buttons_x = bar.Max.x - (button_w * 3.0f);
        caption_ = {.left = static_cast<LONG>(ImGui::GetCursorScreenPos().x),
                    .top = static_cast<LONG>(bar.Min.y),
                    .right = static_cast<LONG>(buttons_x),
                    .bottom = static_cast<LONG>(bar.Max.y)};

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 title_size = ImGui::CalcTextSize(title_utf8_.c_str());
        const float title_x =
            std::max((bar.Min.x + bar.Max.x - title_size.x) * 0.5f, static_cast<float>(caption_.left));
        const float title_y = (bar.Min.y + bar.Max.y - title_size.y) * 0.5f;
        dl->PushClipRect({static_cast<float>(caption_.left), bar.Min.y}, {buttons_x, bar.Max.y}, true);
        dl->AddText({title_x, title_y}, ImGui::GetColorU32(ImGuiCol_TextDisabled, focused_ ? 1.0f : 0.6f),
                    title_utf8_.c_str());
        dl->PopClipRect();
        title_ = {.left = static_cast<LONG>(title_x),
                  .top = static_cast<LONG>(bar.Min.y),
                  .right = static_cast<LONG>(std::min(title_x + title_size.x, buttons_x)),
                  .bottom = static_cast<LONG>(bar.Max.y)};
        if (title_menu_)
        {
            title_menu_ = false;
            ImGui::OpenPopup("##title");
        }
        if (ImGui::BeginPopup("##title"))
        {
            if (ImGui::MenuItem("Copy File Path", nullptr, false, !path_.empty()))
            {
                ImGui::SetClipboardText(text_file::to_utf8(path_.wstring()).c_str());
                show_toast("Copied file path");
            }
            ImGui::EndPopup();
        }

        const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text, focused_ ? 0.9f : 0.5f);
        const float g = 5.0f * dpi_scale_; // half the glyph size
        const float t = std::max(1.0f, dpi_scale_);
        const bool zoomed = IsZoomed(hwnd_) != FALSE;
        for (int i = 0; i < 3; ++i) // raw loop: minimize, maximize, close by index
        {
            const ImVec2 min{buttons_x + (button_w * static_cast<float>(i)), bar.Min.y};
            const ImVec2 max{min.x + button_w, bar.Max.y};
            const ImVec2 c = ImFloor((min + max) * 0.5f) + ImVec2(0.5f, 0.5f);
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##caption", max - min);
            ImGui::PopID();
            if (ImGui::IsItemHovered())
            {
                const bool held = ImGui::IsItemActive();
                dl->AddRectFilled(min, max,
                                  i == 2 ? IM_COL32(196, 43, 28, held ? 200 : 255)
                                         : ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered));
            }
            switch (i)
            {
            case 0:
                dl->AddLine({c.x - g, c.y}, {c.x + g, c.y}, ink, t);
                if (clicked)
                {
                    ShowWindow(hwnd_, SW_MINIMIZE);
                }
                break;
            case 1:
                if (zoomed)
                {
                    dl->AddRect({c.x - g, c.y - g + (2 * t)}, {c.x + g - (2 * t), c.y + g}, ink, 0.0f, 0, t);
                    dl->AddPolyline(std::array{ImVec2(c.x - g + (2 * t), c.y - g + (2 * t)),
                                               ImVec2(c.x - g + (2 * t), c.y - g), ImVec2(c.x + g, c.y - g),
                                               ImVec2(c.x + g, c.y + g - (2 * t)),
                                               ImVec2(c.x + g - (2 * t), c.y + g - (2 * t))}
                                        .data(),
                                    5, ink, 0, t);
                }
                else
                {
                    dl->AddRect({c.x - g, c.y - g}, {c.x + g, c.y + g}, ink, 0.0f, 0, t);
                }
                if (clicked)
                {
                    ShowWindow(hwnd_, zoomed ? SW_RESTORE : SW_MAXIMIZE);
                }
                break;
            default:
                dl->AddLine({c.x - g, c.y - g}, {c.x + g, c.y + g}, ink, t);
                dl->AddLine({c.x + g, c.y - g}, {c.x - g, c.y + g}, ink, t);
                if (clicked)
                {
                    request(action::exit);
                }
                break;
            }
        }
    }

    LRESULT app::hit_test(LPARAM lparam) const
    {
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd_, &p);
        // the caption strip is gone, so the top resize edge lives inside the client area
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int border = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        if (IsZoomed(hwnd_) == FALSE && p.y < border)
        {
            return HTTOP;
        }
        const bool popup = ImGui::GetCurrentContext() != nullptr &&
                           ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        return !popup && PtInRect(&caption_, p) != FALSE ? HTCAPTION : HTCLIENT;
    }

    void app::draw_find_bar()
    {
        const float eased = motion::ease_out(find_anim_);
        const ImGuiStyle& style = ImGui::GetStyle();
        const float rows = show_replace_ ? 2.0f : 1.0f;
        const float full =
            (rows * ImGui::GetFrameHeight()) + ((rows - 1.0f) * style.ItemSpacing.y) + (style.WindowPadding.y * 2);
        // a height of 0 would mean "fill the rest" to imgui and push the editor out for a frame
        const float height = std::floor(full * eased);
        if (height < 1.0f)
        {
            return;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, eased);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, style.Colors[ImGuiCol_MenuBarBg]);
        ImGui::BeginChild("##find", {0, height}, ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        // slide the contents in from above rather than just clipping them
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (full * (1.0f - eased)));

        const float field = 260.0f * dpi_scale_;
        if (find_focus_)
        {
            ImGui::SetKeyboardFocusHere();
            find_focus_ = false;
        }
        ImGui::SetNextItemWidth(field);
        if (ImGui::InputTextWithHint("##find_text", "Find", &find_text_, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            find_next(ImGui::GetIO().KeyShift);
        }
        if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            find_open_ = false;
            editor_.focus();
        }
        ImGui::SameLine();
        if (ImGui::Button("\xE2\x86\x91")) // up arrow
        {
            find_next(true);
        }
        ImGui::SetItemTooltip("Previous (Shift+F3)");
        ImGui::SameLine();
        if (ImGui::Button("\xE2\x86\x93")) // down arrow
        {
            find_next(false);
        }
        ImGui::SetItemTooltip("Next (F3)");
        ImGui::SameLine();
        if (match_case_)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_HeaderActive]);
        }
        const bool toggled = ImGui::Button("Aa");
        if (match_case_)
        {
            ImGui::PopStyleColor();
        }
        if (toggled)
        {
            match_case_ = !match_case_;
        }
        ImGui::SetItemTooltip("Match case");
        ImGui::SameLine();
        if (ImGui::Button(show_replace_ ? "Hide replace" : "Replace..."))
        {
            show_replace_ = !show_replace_;
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
        if (ImGui::Button("\xC3\x97", {ImGui::GetFrameHeight(), 0})) // multiplication sign as close glyph
        {
            find_open_ = false;
            editor_.focus();
        }

        if (show_replace_)
        {
            ImGui::SetNextItemWidth(field);
            ImGui::InputTextWithHint("##replace_text", "Replace with", &replace_text_);
            ImGui::SameLine();
            if (ImGui::Button("Replace all"))
            {
                replace_all();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void app::draw_editor()
    {
        const float status_height = ImGui::GetFrameHeight() + (4.0f * dpi_scale_);
        // integer pixel sizes keep the dynamic font atlas from baking a new size every animation frame
        editor_.draw({
            .font = editor_font_,
            .font_size = std::round(EDITOR_FONT_SIZE * zoom_shown_),
            .wrap = settings_.wrap,
            .minimap = settings_.minimap,
            .dpi = dpi_scale_,
            .height = ImGui::GetContentRegionAvail().y - status_height,
        });
    }

    void app::draw_status_bar()
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, style.Colors[ImGuiCol_MenuBarBg]);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 2) * dpi_scale_);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 2) * dpi_scale_);
        ImGui::PushStyleColor(ImGuiCol_Button, {0, 0, 0, 0});
        ImGui::BeginChild("##status", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPosY((ImGui::GetWindowHeight() - ImGui::GetFrameHeight()) * 0.5f);

        const double since = ImGui::GetTime() - toast_time_;
        if (since < TOAST_SECONDS)
        {
            // quick fade in, hold, then a slow fade out
            const float alpha = std::min(motion::ease_out(static_cast<float>(since) / 0.15f),
                                         motion::ease_out(static_cast<float>(TOAST_SECONDS - since) / 0.6f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored({0.55f, 0.72f, 1.0f, alpha}, "%s", toast_.c_str());
        }

        const std::string position = std::format("Ln {}, Col {}", editor_.line(), editor_.column());
        const std::string count = std::format("{} characters", editor_.char_count());
        const char* encoding = text_file::encoding_name(doc_.encoding);
        const char* eol = doc_.crlf ? "Windows (CRLF)" : "Unix (LF)";
        const std::string zoom = std::format("{}%", static_cast<int>(std::round(settings_.zoom * 100)));

        const float pad = style.FramePadding.x * 2;
        const float gap = style.ItemSpacing.x * 2;
        const float width = ImGui::CalcTextSize(position.c_str()).x + ImGui::CalcTextSize(count.c_str()).x +
                            ImGui::CalcTextSize(encoding).x + ImGui::CalcTextSize(eol).x +
                            ImGui::CalcTextSize(zoom.c_str()).x + (pad * 3) + (gap * 4);
        ImGui::SameLine(std::max(ImGui::GetContentRegionMax().x - width, 0.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", position.c_str());
        ImGui::SameLine(0, gap);
        ImGui::TextDisabled("%s", count.c_str());
        ImGui::SameLine(0, gap);
        ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
        if (ImGui::Button(encoding))
        {
            ImGui::OpenPopup("##encoding");
        }
        ImGui::SameLine(0, gap);
        if (ImGui::Button(eol))
        {
            doc_.crlf = !doc_.crlf;
            format_changed_ = true;
        }
        ImGui::SameLine(0, gap);
        if (ImGui::Button(zoom.c_str()))
        {
            set_zoom(1.0f);
        }
        ImGui::PopStyleColor();

        if (ImGui::BeginPopup("##encoding"))
        {
            for (const text_encoding e : {text_encoding::utf8, text_encoding::utf8_bom, text_encoding::utf16_le,
                                          text_encoding::utf16_be, text_encoding::ansi})
            {
                if (ImGui::Selectable(text_file::encoding_name(e), doc_.encoding == e) && doc_.encoding != e)
                {
                    doc_.encoding = e;
                    format_changed_ = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    void app::draw_unsaved_modal()
    {
        constexpr const char* id = "Unsaved changes";
        if (pending_ == action::none)
        {
            return;
        }
        if (!ImGui::IsPopupOpen(id))
        {
            ImGui::OpenPopup(id);
            modal_time_ = ImGui::GetTime();
        }
        const float t = motion::ease_out(static_cast<float>((ImGui::GetTime() - modal_time_) / MODAL_FADE_SECONDS));
        ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg];
        dim.w *= t;
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, dim);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18) * dpi_scale_);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        // drift up a few pixels while fading in
        ImGui::SetNextWindowPos(viewport->GetCenter() + ImVec2(0, (1.0f - t) * 12.0f * dpi_scale_), ImGuiCond_Always,
                                {0.5f, 0.5f});

        if (ImGui::BeginPopupModal(
                id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove))
        {
            const std::string name = path_.empty() ? "Untitled" : text_file::to_utf8(path_.filename().wstring());
            ImGui::Text("Save changes to %s?", name.c_str());
            ImGui::TextDisabled("Your changes will be lost if you don't save them.");
            ImGui::Dummy(ImVec2(0, 6) * dpi_scale_);

            const ImVec2 button(110 * dpi_scale_, 0);
            const action next = pending_;
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_HeaderActive]);
            const bool save_clicked = ImGui::Button("Save", button) || ImGui::IsKeyPressed(ImGuiKey_Enter);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            const bool discard = ImGui::Button("Don't save", button);
            ImGui::SameLine();
            const bool cancel = ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape);

            if (save_clicked || discard || cancel)
            {
                pending_ = action::none;
                ImGui::CloseCurrentPopup();
                if ((save_clicked && save(false)) || discard)
                {
                    perform(next);
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    // ---------------------------------------------------------------- actions

    void app::request(action next)
    {
        if (dirty())
        {
            pending_ = next;
        }
        else
        {
            perform(next);
        }
    }

    void app::perform(action next)
    {
        switch (next)
        {
        case action::new_file:
            doc_ = {};
            path_.clear();
            editor_.set_text({});
            saved_state_ = editor_.state();
            format_changed_ = false;
            break;
        case action::open_file:
            if (const auto chosen = file_dialog(hwnd_, false, path_.wstring()))
            {
                open(*chosen);
            }
            break;
        case action::open_dropped:
            open(dropped_path_);
            break;
        case action::exit:
            running_ = false;
            break;
        case action::none:
            break;
        }
    }

    void app::open(const std::filesystem::path& path)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            // like notepad: a missing path starts an empty document that saving will create
            perform(action::new_file);
            path_ = path;
            return;
        }
        if (const auto size = std::filesystem::file_size(path, ec); !ec && size > MAX_FILE_BYTES)
        {
            const std::wstring message =
                std::format(L"\"{}\" is too large ({:.1f} MB).\nThe limit is 1 MB.", path.filename().wstring(),
                            static_cast<double>(size) / (1024.0 * 1024.0));
            MessageBoxW(hwnd_, message.c_str(), L"Simple Notepad", MB_ICONWARNING);
            return;
        }
        try
        {
            text_file loaded = text_file::load(path);
            editor_.set_text(std::move(loaded.text));
            doc_ = std::move(loaded);
            doc_.text.clear(); // the editor owns the text
            path_ = path;
            saved_state_ = editor_.state();
            format_changed_ = false;
        }
        catch (const std::exception& e)
        {
            show_toast(e.what());
        }
    }

    bool app::save(bool save_as)
    {
        if (save_as || path_.empty())
        {
            const auto chosen = file_dialog(hwnd_, true, path_.empty() ? L"Untitled.txt" : path_.wstring());
            if (!chosen)
            {
                return false;
            }
            path_ = *chosen;
        }
        try
        {
            const text_file out{.text = editor_.text(), .encoding = doc_.encoding, .crlf = doc_.crlf};
            out.save(path_);
            saved_state_ = editor_.state();
            format_changed_ = false;
            show_toast("Saved");
            return true;
        }
        catch (const std::exception& e)
        {
            const std::wstring message(e.what(), e.what() + std::strlen(e.what()));
            MessageBoxW(hwnd_, message.c_str(), L"Save failed", MB_ICONERROR);
            return false;
        }
    }

    std::optional<size_t> app::search(size_t from, bool backward) const
    {
        const std::string_view text = editor_.text();
        const std::string_view needle = find_text_;
        const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
        const auto equal = [&](char a, char b) { return match_case_ ? a == b : fold(a) == fold(b); };

        if (backward)
        {
            const auto head = text.substr(0, std::min(from, text.size()));
            const auto hit = std::ranges::find_end(head, needle, equal);
            if (!hit.empty())
            {
                return static_cast<size_t>(hit.begin() - head.begin());
            }
            const auto wrapped = std::ranges::find_end(text, needle, equal);
            return wrapped.empty() ? std::nullopt : std::optional(static_cast<size_t>(wrapped.begin() - text.begin()));
        }
        const auto tail = text.substr(std::min(from, text.size()));
        const auto hit = std::ranges::search(tail, needle, equal);
        if (!hit.empty())
        {
            return from + static_cast<size_t>(hit.begin() - tail.begin());
        }
        const auto wrapped = std::ranges::search(text, needle, equal);
        return wrapped.empty() ? std::nullopt : std::optional(static_cast<size_t>(wrapped.begin() - text.begin()));
    }

    void app::find_next(bool backward)
    {
        if (find_text_.empty())
        {
            find_open_ = find_focus_ = true;
            return;
        }
        const auto [lo, hi] = editor_.selection();
        const auto found = search(static_cast<size_t>(backward ? lo : hi), backward);
        if (!found)
        {
            show_toast(std::format("\"{}\" not found", find_text_));
            return;
        }
        editor_.select(static_cast<int>(*found), static_cast<int>(*found + find_text_.size()));
        find_cycling_ = true;
    }

    void app::replace_all()
    {
        if (find_text_.empty())
        {
            return;
        }
        const std::string& text = editor_.text();
        std::string result;
        result.reserve(text.size());
        size_t from = 0;
        int replaced = 0;
        // raw loop: search() wraps around, so stop once a hit lands before the cursor
        while (const auto hit = search(from, false))
        {
            if (*hit < from)
            {
                break;
            }
            result.append(text, from, *hit - from);
            result += replace_text_;
            from = *hit + find_text_.size();
            ++replaced;
        }
        if (replaced == 0)
        {
            show_toast(std::format("\"{}\" not found", find_text_));
            return;
        }
        result.append(text, from);
        editor_.replace_all(result);
        show_toast(std::format("Replaced {} occurrence{} (Ctrl+Z to undo)", replaced, replaced == 1 ? "" : "s"));
    }

    void app::set_zoom(float zoom)
    {
        settings_.zoom = std::clamp(std::round(zoom * 10.0f) / 10.0f, MIN_ZOOM, MAX_ZOOM);
    }

    void app::show_toast(std::string text)
    {
        toast_ = std::move(text);
        toast_time_ = ImGui::GetTime();
    }

    void app::update_title()
    {
        const std::wstring name = path_.empty() ? L"Untitled" : path_.filename().wstring();
        std::wstring title = std::format(L"{}{} - Simple Notepad", dirty() ? L"*" : L"", name);
        if (title != shown_title_)
        {
            SetWindowTextW(hwnd_, title.c_str());
            title_utf8_ = text_file::to_utf8(title);
            shown_title_ = std::move(title);
        }
    }

    // ---------------------------------------------------------------- persistence

    std::filesystem::path app::settings_path()
    {
        std::wstring appdata(MAX_PATH, L'\0');
        appdata.resize(GetEnvironmentVariableW(L"APPDATA", appdata.data(), MAX_PATH));
        return appdata.empty() ? std::filesystem::path() : std::filesystem::path(appdata) / L"simple_notepad.ini";
    }

    void app::load_settings()
    {
        const ini file = ini::load(settings_path());
        file.get("view", "wrap", settings_.wrap);
        file.get("view", "minimap", settings_.minimap);
    }

    void app::save_settings() const
    {
        ini file;
        file.set("view", "wrap", settings_.wrap);
        file.set("view", "minimap", settings_.minimap);
        file.save(settings_path());
    }

    std::optional<std::filesystem::path> app::file_dialog(HWND owner, bool save, const std::wstring& initial)
    {
        std::wstring buffer = initial;
        buffer.resize(32768, L'\0');
        OPENFILENAMEW ofn{.lStructSize = sizeof(OPENFILENAMEW)};
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = L"Text Documents (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
        ofn.nFilterIndex = 2;
        ofn.lpstrFile = buffer.data();
        ofn.nMaxFile = static_cast<DWORD>(buffer.size());
        ofn.lpstrDefExt = L"txt";
        ofn.Flags =
            OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
        if (!ok)
        {
            return std::nullopt;
        }
        return std::filesystem::path(buffer.c_str());
    }
} // namespace simple_notepad
