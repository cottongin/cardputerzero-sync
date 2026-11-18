#include "fb_blit.hpp"
#include "ui_controller.hpp"

#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& prefix) {
        auto base = fs::temp_directory_path();
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = base / (prefix + "_" + std::to_string(getpid()) + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void type_text(cp0::UiController& ctrl, const std::string& text) {
    for (char ch : text) {
        auto act = ctrl.handle_key(ch, cp0::KeyCode::Char);
        assert(act.type == cp0::ActionType::None);
    }
}

void test_initial_state_and_global_navigation() {
    cp0::UiController ctrl;
    assert(ctrl.state().tab == cp0::ActiveTab::Status);
    assert(ctrl.state().theme == cp0::ThemeMode::Dark);
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // Direct tab jumps: 1 / 2 / 3
    assert(ctrl.handle_key('2', cp0::KeyCode::Char).type == cp0::ActionType::None);
    assert(ctrl.state().tab == cp0::ActiveTab::Folders);
    assert(ctrl.handle_key('3', cp0::KeyCode::Char).type == cp0::ActionType::None);
    assert(ctrl.state().tab == cp0::ActiveTab::Logs);
    assert(ctrl.handle_key('1', cp0::KeyCode::Char).type == cp0::ActionType::None);
    assert(ctrl.state().tab == cp0::ActiveTab::Status);

    // Left / Right cycle tabs
    ctrl.handle_key(0, cp0::KeyCode::Right);
    assert(ctrl.state().tab == cp0::ActiveTab::Folders);
    ctrl.handle_key(0, cp0::KeyCode::Right);
    assert(ctrl.state().tab == cp0::ActiveTab::Logs);
    ctrl.handle_key(0, cp0::KeyCode::Right);
    assert(ctrl.state().tab == cp0::ActiveTab::Status);
    ctrl.handle_key(0, cp0::KeyCode::Left);
    assert(ctrl.state().tab == cp0::ActiveTab::Logs);
    ctrl.handle_key(0, cp0::KeyCode::Left);
    assert(ctrl.state().tab == cp0::ActiveTab::Folders);
    ctrl.handle_key(0, cp0::KeyCode::Left);
    assert(ctrl.state().tab == cp0::ActiveTab::Status);

    // Enter triggers SyncNow on any main tab
    auto sync1 = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(sync1.type == cp0::ActionType::SyncNow);
    ctrl.handle_key('2', cp0::KeyCode::Char);
    auto sync2 = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(sync2.type == cp0::ActionType::SyncNow);

    // Esc and Q emit Quit when no modal is open
    assert(ctrl.handle_key(0, cp0::KeyCode::Escape).type == cp0::ActionType::Quit);
    assert(ctrl.handle_key('q', cp0::KeyCode::Char).type == cp0::ActionType::Quit);
    assert(ctrl.handle_key('Q', cp0::KeyCode::Char).type == cp0::ActionType::Quit);
}

void test_theme_toggle_outside_vs_inside_modals() {
    cp0::UiController ctrl;

    // Initial daemon status seeds theme once
    cp0::DaemonStatus st;
    st.ok = true;
    st.theme = "light";
    cp0::FolderStatus f;
    f.id = "cam1";
    f.path = "/tmp/cam1";
    f.label = "Camera";
    st.folders.push_back(f);
    ctrl.update_status(st);
    assert(ctrl.state().theme == cp0::ThemeMode::Light);

    // 'T' toggles theme on main tab and emits SetTheme
    auto act1 = ctrl.handle_key('t', cp0::KeyCode::Char);
    assert(act1.type == cp0::ActionType::SetTheme);
    assert(act1.theme == cp0::ThemeMode::Dark);
    assert(ctrl.state().theme == cp0::ThemeMode::Dark);

    // Stale daemon status with unchanged "light" does not clobber optimistic toggle
    ctrl.update_status(st);
    assert(ctrl.state().theme == cp0::ThemeMode::Dark);

    // Uppercase 'T' toggles back to Light
    auto act2 = ctrl.handle_key('T', cp0::KeyCode::Char);
    assert(act2.type == cp0::ActionType::SetTheme);
    assert(act2.theme == cp0::ThemeMode::Light);
    assert(ctrl.state().theme == cp0::ThemeMode::Light);

    // Inside DirPicker, 'T' opens TextInput instead of toggling theme
    ctrl.handle_key('2', cp0::KeyCode::Char);
    ctrl.handle_key('a', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::DirPicker);
    auto modal_t = ctrl.handle_key('t', cp0::KeyCode::Char);
    assert(modal_t.type == cp0::ActionType::None);
    assert(ctrl.state().modal == cp0::ModalKind::TextInput);
    assert(ctrl.state().theme == cp0::ThemeMode::Light);

    // Inside TextInput, 't' appends 't' instead of toggling theme or opening another modal
    auto text_t = ctrl.handle_key('t', cp0::KeyCode::Char);
    assert(text_t.type == cp0::ActionType::None);
    assert(ctrl.state().text_buffer == "t");
    assert(ctrl.state().theme == cp0::ThemeMode::Light);

    // Esc closes TextInput without toggling theme or quitting
    auto esc1 = ctrl.handle_key(0, cp0::KeyCode::Escape);
    assert(esc1.type == cp0::ActionType::None);
    assert(ctrl.state().modal == cp0::ModalKind::None);
    assert(ctrl.state().theme == cp0::ThemeMode::Light);

    // Esc again (modal == None) emits Quit
    auto esc2 = ctrl.handle_key(0, cp0::KeyCode::Escape);
    assert(esc2.type == cp0::ActionType::Quit);

    // Inside TargetPicker, 'T' opens TextInput without toggling theme
    ctrl.handle_key('1', cp0::KeyCode::Char);
    ctrl.handle_key('c', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::TargetPicker);
    ctrl.handle_key('T', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::TextInput);
    assert(ctrl.state().theme == cp0::ThemeMode::Light);
    ctrl.handle_key(0, cp0::KeyCode::Escape);
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // Inside ConfirmRemove, 'T' does not toggle theme
    ctrl.handle_key('2', cp0::KeyCode::Char);
    ctrl.handle_key('d', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::ConfirmRemove);
    ctrl.handle_key('t', cp0::KeyCode::Char);
    assert(ctrl.state().theme == cp0::ThemeMode::Light);
    assert(ctrl.state().modal == cp0::ModalKind::ConfirmRemove);
    ctrl.handle_key('n', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::None);
}

void test_target_picker_and_manual_host() {
    cp0::UiController ctrl;

    std::vector<cp0::TailscalePeer> peers;
    cp0::TailscalePeer p1;
    p1.id = "1";
    p1.hostname = "macbook";
    p1.dns_name = "macbook.ts.net";
    p1.os = "macOS";
    p1.online = true;
    p1.tailscale_ips = {"100.64.0.10"};
    peers.push_back(p1);

    cp0::TailscalePeer p2;
    p2.id = "2";
    p2.hostname = "studio";
    p2.dns_name = "";
    p2.os = "macOS";
    p2.online = false;
    p2.tailscale_ips = {"100.64.0.11"};
    peers.push_back(p2);

    // 'c' on Status tab opens TargetPicker and requests peer refresh
    ctrl.handle_key('c', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::TargetPicker);
    assert(ctrl.consume_peer_refresh() == true);
    assert(ctrl.consume_peer_refresh() == false);

    ctrl.set_peers(peers);
    assert(ctrl.state().peers.size() == 2);
    assert(ctrl.state().peer_cursor == 0);

    // Navigate with j/k and Up/Down (clamped at ends)
    ctrl.handle_key('j', cp0::KeyCode::Char);
    assert(ctrl.state().peer_cursor == 1);
    ctrl.handle_key(0, cp0::KeyCode::Down);
    assert(ctrl.state().peer_cursor == 1);
    ctrl.handle_key('k', cp0::KeyCode::Char);
    assert(ctrl.state().peer_cursor == 0);
    ctrl.handle_key(0, cp0::KeyCode::Up);
    assert(ctrl.state().peer_cursor == 0);

    // Select first peer (uses dns_name + default port 8765)
    auto sel1 = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(sel1.type == cp0::ActionType::SetTarget);
    assert(sel1.target_host == "macbook.ts.net");
    assert(sel1.target_port == 8765);
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // Select second peer (falls back to IP when dns_name empty)
    ctrl.handle_key('c', cp0::KeyCode::Char);
    ctrl.set_peers(peers);
    ctrl.handle_key(0, cp0::KeyCode::Down);
    auto sel2 = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(sel2.type == cp0::ActionType::SetTarget);
    assert(sel2.target_host == "100.64.0.11");
    assert(sel2.target_port == 8765);
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // Manual host[:port] via TextInput with Backspace editing
    ctrl.handle_key('c', cp0::KeyCode::Char);
    ctrl.handle_key('t', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::TextInput);
    type_text(ctrl, "imac.ts.net:9000");
    ctrl.handle_key(0, cp0::KeyCode::Backspace);
    type_text(ctrl, "1");
    auto manual_port = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(manual_port.type == cp0::ActionType::SetTarget);
    assert(manual_port.target_host == "imac.ts.net");
    assert(manual_port.target_port == 9001);
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // Manual host without port defaults to 8765
    ctrl.handle_key('c', cp0::KeyCode::Char);
    ctrl.handle_key('t', cp0::KeyCode::Char);
    type_text(ctrl, "100.64.0.42");
    auto manual_default_port = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(manual_default_port.type == cp0::ActionType::SetTarget);
    assert(manual_default_port.target_host == "100.64.0.42");
    assert(manual_default_port.target_port == 8765);

    // Empty host stays in TextInput without emitting SetTarget
    ctrl.handle_key('c', cp0::KeyCode::Char);
    ctrl.handle_key('t', cp0::KeyCode::Char);
    type_text(ctrl, "   ");
    auto empty_submit = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(empty_submit.type == cp0::ActionType::None);
    assert(ctrl.state().modal == cp0::ModalKind::TextInput);
    assert(!ctrl.state().modal_error.empty());
    ctrl.handle_key(0, cp0::KeyCode::Escape);
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // Invalid port stays in TextInput with error
    ctrl.handle_key('c', cp0::KeyCode::Char);
    ctrl.handle_key('t', cp0::KeyCode::Char);
    type_text(ctrl, "mac.ts.net:99999");
    auto bad_port = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(bad_port.type == cp0::ActionType::None);
    assert(ctrl.state().modal == cp0::ModalKind::TextInput);
    assert(!ctrl.state().modal_error.empty());
    ctrl.handle_key(0, cp0::KeyCode::Escape);

    // TargetPicker with empty peers ignores Enter
    ctrl.handle_key('c', cp0::KeyCode::Char);
    ctrl.set_peers({});
    auto no_peer_enter = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(no_peer_enter.type == cp0::ActionType::None);
    assert(ctrl.state().modal == cp0::ModalKind::TargetPicker);
    ctrl.handle_key(0, cp0::KeyCode::Escape);

    // IPv6 host with port and without port
    ctrl.handle_key('c', cp0::KeyCode::Char);
    ctrl.handle_key('t', cp0::KeyCode::Char);
    type_text(ctrl, "[::1]:8765");
    auto ipv6_port = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(ipv6_port.type == cp0::ActionType::SetTarget);
    assert(ipv6_port.target_host == "::1");
    assert(ipv6_port.target_port == 8765);
    assert(ctrl.state().modal == cp0::ModalKind::None);

    ctrl.handle_key('c', cp0::KeyCode::Char);
    ctrl.handle_key('t', cp0::KeyCode::Char);
    type_text(ctrl, "[fd7a:115c:a1e0::1]");
    auto ipv6_default = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(ipv6_default.type == cp0::ActionType::SetTarget);
    assert(ipv6_default.target_host == "fd7a:115c:a1e0::1");
    assert(ipv6_default.target_port == 8765);
}

void test_dir_picker_traversal_and_manual_path() {
    TempDir tmp("cp0_dirpicker");
    fs::path root = tmp.path;
    fs::create_directories(root / "alpha");
    fs::create_directories(root / "beta" / "sub1");
    fs::create_directories(root / "gamma");
    {
        std::ofstream f(root / "IMG_0001.JPG");
        f << "not a directory";
    }

    cp0::UiController ctrl;
    ctrl.set_dir_picker_start_path(root.string());
    ctrl.handle_key('2', cp0::KeyCode::Char);

    // 'a' opens DirPicker at configured start path
    ctrl.handle_key('a', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::DirPicker);
    assert(fs::path(ctrl.state().dir_current_path) == fs::weakly_canonical(root));
    assert(ctrl.state().dir_entries.size() == 3);
    assert(fs::path(ctrl.state().dir_entries[0]).filename() == "alpha");
    assert(fs::path(ctrl.state().dir_entries[1]).filename() == "beta");
    assert(fs::path(ctrl.state().dir_entries[2]).filename() == "gamma");
    assert(ctrl.state().dir_cursor == 0);

    // Move down to 'beta' and drill in with Enter
    ctrl.handle_key('j', cp0::KeyCode::Char);
    assert(ctrl.state().dir_cursor == 1);
    ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(fs::path(ctrl.state().dir_current_path) == fs::weakly_canonical(root / "beta"));
    assert(ctrl.state().dir_entries.size() == 1);
    assert(fs::path(ctrl.state().dir_entries[0]).filename() == "sub1");

    // Backspace goes up to parent ('root') and restores cursor on 'beta'
    ctrl.handle_key(0, cp0::KeyCode::Backspace);
    assert(fs::path(ctrl.state().dir_current_path) == fs::weakly_canonical(root));
    assert(ctrl.state().dir_entries.size() == 3);
    assert(ctrl.state().dir_cursor == 1);

    // Drill back into beta and press Space on highlighted subdir 'sub1'
    ctrl.handle_key(0, cp0::KeyCode::Enter);
    auto add_highlighted = ctrl.handle_key(0, cp0::KeyCode::Space);
    assert(add_highlighted.type == cp0::ActionType::AddFolder);
    assert(fs::path(add_highlighted.folder_path) == fs::weakly_canonical(root / "beta" / "sub1"));
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // Drill into empty leaf dir 'sub1' and press Space (' ' char) to watch current dir
    ctrl.handle_key('a', cp0::KeyCode::Char);
    ctrl.handle_key('j', cp0::KeyCode::Char); // beta
    ctrl.handle_key(0, cp0::KeyCode::Enter);
    ctrl.handle_key(0, cp0::KeyCode::Enter); // sub1 (empty)
    assert(ctrl.state().dir_entries.empty());
    assert(ctrl.state().dir_cursor == -1);
    auto add_current = ctrl.handle_key(' ', cp0::KeyCode::Char);
    assert(add_current.type == cp0::ActionType::AddFolder);
    assert(fs::path(add_current.folder_path) == fs::weakly_canonical(root / "beta" / "sub1"));
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // Manual folder path via [T] inside DirPicker
    ctrl.handle_key('a', cp0::KeyCode::Char);
    ctrl.handle_key('t', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::TextInput);
    type_text(ctrl, "/media/sd/DCIM");
    ctrl.handle_key(0, cp0::KeyCode::Backspace);
    type_text(ctrl, "M2");
    auto add_manual = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(add_manual.type == cp0::ActionType::AddFolder);
    assert(add_manual.folder_path == "/media/sd/DCIM2");
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // '.' inside DirPicker adds dir_current_path directly (even when a subdir is highlighted)
    ctrl.handle_key('a', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::DirPicker);
    ctrl.handle_key('j', cp0::KeyCode::Char); // highlight beta
    auto add_dot = ctrl.handle_key('.', cp0::KeyCode::Char);
    assert(add_dot.type == cp0::ActionType::AddFolder);
    assert(fs::path(add_dot.folder_path) == fs::weakly_canonical(root));
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // '~' tilde path expansion in FolderPath TextInput
    const char* raw_home = std::getenv("HOME");
    bool had_home = (raw_home != nullptr);
    std::string saved_home = had_home ? raw_home : "";
    ::setenv("HOME", "/home/cp0user", 1);

    ctrl.handle_key('a', cp0::KeyCode::Char);
    ctrl.handle_key('t', cp0::KeyCode::Char);
    type_text(ctrl, "~/Pictures/Camera");
    auto add_tilde = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(add_tilde.type == cp0::ActionType::AddFolder);
    assert(add_tilde.folder_path == "/home/cp0user/Pictures/Camera");
    assert(ctrl.state().modal == cp0::ModalKind::None);

    ctrl.handle_key('a', cp0::KeyCode::Char);
    ctrl.handle_key('t', cp0::KeyCode::Char);
    type_text(ctrl, "~");
    auto add_tilde_home = ctrl.handle_key(0, cp0::KeyCode::Enter);
    assert(add_tilde_home.type == cp0::ActionType::AddFolder);
    assert(add_tilde_home.folder_path == "/home/cp0user");

    if (had_home) {
        ::setenv("HOME", saved_home.c_str(), 1);
    } else {
        ::unsetenv("HOME");
    }
}

void test_folders_selection_and_confirm_remove() {
    cp0::UiController ctrl;
    ctrl.handle_key('2', cp0::KeyCode::Char);

    // Empty folders: 'd' and Delete do not open ConfirmRemove
    ctrl.handle_key('d', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::None);
    ctrl.handle_key(0, cp0::KeyCode::Delete);
    assert(ctrl.state().modal == cp0::ModalKind::None);

    cp0::DaemonStatus st;
    st.ok = true;
    cp0::FolderStatus f1;
    f1.id = "cam";
    f1.path = "/home/pi/Pictures/DCIM/Camera";
    f1.label = "Camera";
    f1.exists = true;
    cp0::FolderStatus f2;
    f2.id = "sd";
    f2.path = "/media/sd/DCIM";
    f2.label = "SD";
    f2.exists = false;
    st.folders = {f1, f2};
    ctrl.update_status(st);

    assert(ctrl.state().folder_cursor == 0);
    ctrl.handle_key('j', cp0::KeyCode::Char);
    assert(ctrl.state().folder_cursor == 1);
    ctrl.handle_key(0, cp0::KeyCode::Down);
    assert(ctrl.state().folder_cursor == 1);

    // 'd' opens ConfirmRemove, 'n' cancels
    ctrl.handle_key('d', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::ConfirmRemove);
    assert(ctrl.state().confirm_folder_id == "sd");
    auto cancel_n = ctrl.handle_key('n', cp0::KeyCode::Char);
    assert(cancel_n.type == cp0::ActionType::None);
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // Delete opens ConfirmRemove, Esc cancels
    ctrl.handle_key(0, cp0::KeyCode::Delete);
    assert(ctrl.state().modal == cp0::ModalKind::ConfirmRemove);
    auto cancel_esc = ctrl.handle_key(0, cp0::KeyCode::Escape);
    assert(cancel_esc.type == cp0::ActionType::None);
    assert(ctrl.state().modal == cp0::ModalKind::None);

    // 'd' -> 'y' emits RemoveFolder
    ctrl.handle_key('d', cp0::KeyCode::Char);
    assert(ctrl.state().modal == cp0::ModalKind::ConfirmRemove);
    auto confirm_y = ctrl.handle_key('y', cp0::KeyCode::Char);
    assert(confirm_y.type == cp0::ActionType::RemoveFolder);
    assert(confirm_y.folder_id == "sd");
    assert(ctrl.state().modal == cp0::ModalKind::None);
}

void test_logs_scrolling_and_retry_failed() {
    cp0::UiController ctrl;
    cp0::DaemonStatus st;
    st.ok = true;
    for (int i = 0; i < 10; ++i) {
        cp0::LogEntry e;
        e.id = 100 - i;
        e.ts = 1731900000000LL + i * 1000LL;
        e.level = (i % 3 == 0) ? "ERROR" : "INFO";
        e.message = "Entry " + std::to_string(i);
        st.logs.push_back(e);
    }
    ctrl.update_status(st);

    // 'r' outside Logs tab does not emit RetryFailed
    assert(ctrl.handle_key('r', cp0::KeyCode::Char).type == cp0::ActionType::None);

    // Switch to Logs tab
    ctrl.handle_key('3', cp0::KeyCode::Char);
    assert(ctrl.state().tab == cp0::ActiveTab::Logs);
    assert(ctrl.state().log_scroll == 0);

    ctrl.handle_key('j', cp0::KeyCode::Char);
    assert(ctrl.state().log_scroll == 1);
    ctrl.handle_key(0, cp0::KeyCode::Down);
    assert(ctrl.state().log_scroll == 2);
    ctrl.handle_key('k', cp0::KeyCode::Char);
    assert(ctrl.state().log_scroll == 1);
    ctrl.handle_key(0, cp0::KeyCode::Up);
    assert(ctrl.state().log_scroll == 0);
    ctrl.handle_key(0, cp0::KeyCode::Up);
    assert(ctrl.state().log_scroll == 0);

    for (int i = 0; i < 20; ++i) {
        ctrl.handle_key('j', cp0::KeyCode::Char);
    }
    assert(ctrl.state().log_scroll == 9);

    auto retry = ctrl.handle_key('r', cp0::KeyCode::Char);
    assert(retry.type == cp0::ActionType::RetryFailed);
}

void test_feedback_banner_and_error_flag() {
    cp0::UiController ctrl;
    assert(ctrl.state().feedback_banner.empty());
    assert(ctrl.state().feedback_is_error == false);

    ctrl.set_feedback("Sync triggered");
    assert(ctrl.state().feedback_banner == "Sync triggered");
    assert(ctrl.state().feedback_is_error == false);

    ctrl.set_feedback("Daemon unreachable", true);
    assert(ctrl.state().feedback_banner == "Daemon unreachable");
    assert(ctrl.state().feedback_is_error == true);

    ctrl.set_feedback("Folder added");
    assert(ctrl.state().feedback_banner == "Folder added");
    assert(ctrl.state().feedback_is_error == false);

    ctrl.set_feedback("", true);
    assert(ctrl.state().feedback_banner.empty());
    assert(ctrl.state().feedback_is_error == false);

    ctrl.set_feedback("Failed", true);
    assert(ctrl.state().feedback_is_error == true);
    ctrl.clear_feedback();
    assert(ctrl.state().feedback_banner.empty());
    assert(ctrl.state().feedback_is_error == false);
}

void test_fb_blit_pixel_formats_and_bounds() {
    const uint32_t src[4] = {
        0xff123456U, 0xfff80000U,
        0xff00fc00U, 0xff0000f8U,
    };

    // 32-bit XRGB8888 fast path with xoffset/yoffset and padded stride
    {
        cp0::FbBlitSpec spec;
        spec.xres = 2;
        spec.yres = 2;
        spec.xoffset = 1;
        spec.yoffset = 1;
        spec.bits_per_pixel = 32;
        spec.line_length = 16; // 4 pixels per scanline
        spec.red = {16, 8};
        spec.green = {8, 8};
        spec.blue = {0, 8};

        std::vector<uint8_t> buf(48, 0xaa);
        assert(cp0::blit_framebuffer(buf.data(), buf.size(), spec, src, 2, 2));
        uint32_t p00 = 0;
        uint32_t p11 = 0;
        std::memcpy(&p00, buf.data() + 1 * 16 + 1 * 4, 4);
        std::memcpy(&p11, buf.data() + 2 * 16 + 2 * 4, 4);
        assert(p00 == 0xff123456U);
        assert(p11 == 0xff0000f8U);
        assert(buf[0] == 0xaa);
    }

    // 32-bit XBGR8888 (red/blue swapped in fb layout)
    {
        cp0::FbBlitSpec spec;
        spec.xres = 2;
        spec.yres = 2;
        spec.bits_per_pixel = 32;
        spec.line_length = 8;
        spec.red = {0, 8};
        spec.green = {8, 8};
        spec.blue = {16, 8};

        std::vector<uint8_t> buf(16, 0);
        assert(cp0::blit_framebuffer(buf.data(), buf.size(), spec, src, 2, 2));
        uint32_t p00 = 0;
        std::memcpy(&p00, buf.data(), 4);
        assert(p00 == 0x00563412U);
    }

    // 16-bit RGB565 & BGR565 with odd stride
    {
        cp0::FbBlitSpec spec;
        spec.xres = 2;
        spec.yres = 2;
        spec.bits_per_pixel = 16;
        spec.line_length = 5; // odd byte stride >= 4
        spec.red = {11, 5};
        spec.green = {5, 6};
        spec.blue = {0, 5};

        std::vector<uint8_t> buf(10, 0);
        assert(cp0::blit_framebuffer(buf.data(), buf.size(), spec, src, 2, 2));
        uint16_t p01 = 0; // pure red 0xfff80000 -> 0xf800
        uint16_t p10 = 0; // pure green 0xff00fc00 -> 0x07e0
        std::memcpy(&p01, buf.data() + 2, 2);
        std::memcpy(&p10, buf.data() + 5, 2);
        assert(p01 == 0xf800U);
        assert(p10 == 0x07e0U);

        spec.red = {0, 5};
        spec.blue = {11, 5};
        assert(cp0::blit_framebuffer(buf.data(), buf.size(), spec, src, 2, 2));
        std::memcpy(&p01, buf.data() + 2, 2);
        assert(p01 == 0x001fU);
    }

    // 24-bit BGR888
    {
        cp0::FbBlitSpec spec;
        spec.xres = 2;
        spec.yres = 2;
        spec.bits_per_pixel = 24;
        spec.line_length = 6;
        spec.red = {0, 8};
        spec.green = {8, 8};
        spec.blue = {16, 8};

        std::vector<uint8_t> buf(12, 0);
        assert(cp0::blit_framebuffer(buf.data(), buf.size(), spec, src, 2, 2));
        assert(buf[0] == 0x12 && buf[1] == 0x34 && buf[2] == 0x56);
    }

    // Bounds & invalid format checks (canary byte after map_size untouched)
    {
        cp0::FbBlitSpec spec;
        spec.xres = 2;
        spec.yres = 2;
        spec.bits_per_pixel = 16;
        spec.line_length = 4;
        spec.red = {11, 5};
        spec.green = {5, 6};
        spec.blue = {0, 5};

        std::vector<uint8_t> buf(8, 0x55);
        // Only 6 bytes mapped, so row 1 (bytes 4..7) overflows map_size
        assert(!cp0::blit_framebuffer(buf.data(), 6, spec, src, 2, 2));
        assert(buf[6] == 0x55 && buf[7] == 0x55);

        // Stride smaller than row_bytes rejected
        spec.line_length = 3;
        assert(!cp0::blit_framebuffer(buf.data(), buf.size(), spec, src, 2, 2));

        // Unsupported bpp rejected
        spec.line_length = 4;
        spec.bits_per_pixel = 8;
        assert(!cp0::blit_framebuffer(buf.data(), buf.size(), spec, src, 2, 2));
    }
}

} // namespace

int main() {
    test_initial_state_and_global_navigation();
    test_theme_toggle_outside_vs_inside_modals();
    test_target_picker_and_manual_host();
    test_dir_picker_traversal_and_manual_path();
    test_folders_selection_and_confirm_remove();
    test_logs_scrolling_and_retry_failed();
    test_feedback_banner_and_error_flag();
    test_fb_blit_pixel_formats_and_bounds();
    std::cout << "All test_ui_navigation assertions passed.\n";
    return 0;
}
