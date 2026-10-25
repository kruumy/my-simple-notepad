#pragma once

#include "editor.h"
#include "text_file.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <wrl/client.h>

struct ImFont;
using ImU32 = unsigned int;

namespace simple_notepad
{
    class app
    {
      public:
        int run(const std::wstring& open_path);

      private:
        enum class action : std::uint8_t
        {
            none,
            new_file,
            open_file,
            open_dropped,
            exit,
        };

        struct settings
        {
            bool wrap = true;
            bool minimap = true;
            float zoom = 1.0f;
        };

        // window and device
        HWND hwnd_ = nullptr;
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
        Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain_;
        HANDLE frame_ready_ = nullptr; // signalled when the gpu can take another frame
        std::vector<char> presented_;  // draw data of the frame on screen, to skip identical frames
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_;
        bool running_ = true;
        bool focused_ = true;
        bool in_frame_ = false;
        int frames_ = 3;
        float dpi_scale_ = 1.0f;

        // document
        text_file doc_;
        std::filesystem::path path_;
        editor editor_;
        std::uint64_t saved_state_ = 0;
        bool format_changed_ = false; // encoding or line endings edited
        std::wstring shown_title_;
        std::string title_utf8_;
        RECT caption_{}; // client-space strip of the menu bar that drags the window
        RECT title_{};   // client-space bounds of the title text, right-click copies the path
        bool title_menu_ = false;

        // view
        settings settings_;
        ImFont* ui_font_ = nullptr;
        ImFont* editor_font_ = nullptr;
        float zoom_shown_ = 1.0f;
        bool ready_ = false;

        // find bar
        bool find_open_ = false;
        bool find_focus_ = false;
        bool show_replace_ = false;
        bool match_case_ = false;
        bool find_cycling_ = false;
        float find_anim_ = 0.0f;
        std::string find_text_;
        std::string replace_text_;

        // feedback
        std::string toast_;
        double toast_time_ = -10.0;
        action pending_ = action::none;
        std::wstring dropped_path_;
        double modal_time_ = 0.0;

        static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
        LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);

        bool create_device();
        void create_target();
        void resize(UINT width, UINT height);
        void set_dpi(float scale);
        void frame();
        [[nodiscard]] bool animating() const;
        [[nodiscard]] bool dirty() const;
        [[nodiscard]] bool same_as_presented();

        void draw_menu();
        void draw_caption();
        [[nodiscard]] LRESULT hit_test(LPARAM lparam) const;
        void draw_find_bar();
        void draw_editor();
        void draw_status_bar();
        void draw_unsaved_modal();
        void handle_shortcuts();

        void request(action next);
        void perform(action next);
        void open(const std::filesystem::path& path);
        bool save(bool save_as);
        [[nodiscard]] std::optional<size_t> search(size_t from, bool backward) const;
        void find_next(bool backward);
        void replace_all();
        void set_zoom(float zoom);
        void show_toast(std::string text);
        void update_title();

        static std::filesystem::path settings_path();
        void load_settings();
        void save_settings() const;
        static std::optional<std::filesystem::path> file_dialog(HWND owner, bool save, const std::wstring& initial);
    };
} // namespace simple_notepad
