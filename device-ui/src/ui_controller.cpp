#include "ui_controller.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace cp0 {

namespace {

std::string trim_ascii(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

std::string expand_tilde(const std::string& path) {
    if (path.empty() || path[0] != '~') {
        return path;
    }
    if (path.size() == 1 || path[1] == '/') {
        const char* home = std::getenv("HOME");
        if (home != nullptr && home[0] != '\0') {
            return std::string(home) + path.substr(1);
        }
    }
    return path;
}

bool is_valid_dir(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return fs::is_directory(fs::path(path), ec);
}

bool parse_strict_port(const std::string& s, int& port_out) {
    if (s.empty()) {
        return false;
    }
    long val = 0;
    for (char ch : s) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        val = val * 10 + (ch - '0');
        if (val > 65535) {
            return false;
        }
    }
    if (val < 1 || val > 65535) {
        return false;
    }
    port_out = static_cast<int>(val);
    return true;
}

bool parse_host_port(
    const std::string& raw,
    std::string& host_out,
    int& port_out,
    std::string& err_out
) {
    std::string trimmed = trim_ascii(raw);
    if (trimmed.empty()) {
        err_out = "Host cannot be empty";
        return false;
    }

    port_out = 8765;
    if (trimmed.front() == '[') {
        size_t close = trimmed.find(']');
        if (close == std::string::npos || close <= 1) {
            err_out = "Invalid bracketed host";
            return false;
        }
        host_out = trimmed.substr(1, close - 1);
        if (close + 1 < trimmed.size()) {
            if (trimmed[close + 1] != ':') {
                err_out = "Invalid host:port syntax";
                return false;
            }
            if (!parse_strict_port(trimmed.substr(close + 2), port_out)) {
                err_out = "Port must be 1..65535";
                return false;
            }
        }
    } else {
        size_t first_colon = trimmed.find(':');
        size_t last_colon = trimmed.rfind(':');
        if (first_colon != std::string::npos && first_colon == last_colon) {
            host_out = trim_ascii(trimmed.substr(0, first_colon));
            std::string port_str = trim_ascii(trimmed.substr(first_colon + 1));
            if (!parse_strict_port(port_str, port_out)) {
                err_out = "Port must be 1..65535";
                return false;
            }
        } else {
            host_out = trimmed;
        }
    }

    if (host_out.empty() || host_out.find('/') != std::string::npos) {
        err_out = "Invalid target host";
        return false;
    }
    for (char ch : host_out) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            err_out = "Host cannot contain spaces";
            return false;
        }
    }
    return true;
}

std::string resolve_peer_host(const TailscalePeer& peer) {
    if (!peer.dns_name.empty()) {
        return peer.dns_name;
    }
    if (!peer.tailscale_ips.empty()) {
        return peer.tailscale_ips[0];
    }
    return peer.hostname;
}

KeyCode normalize_key(char ch, KeyCode key) {
    if (key != KeyCode::Char) {
        return key;
    }
    if (ch == ' ') {
        return KeyCode::Space;
    }
    if (ch == '\n' || ch == '\r') {
        return KeyCode::Enter;
    }
    if (ch == '\b' || static_cast<unsigned char>(ch) == 0x7f) {
        return KeyCode::Backspace;
    }
    if (static_cast<unsigned char>(ch) == 0x1b) {
        return KeyCode::Escape;
    }
    return KeyCode::Char;
}

} // namespace

void UiController::update_status(const DaemonStatus& status) {
    state_.status = status;

    if (status.ok && !status.theme.empty()) {
        if (!initial_status_applied_) {
            state_.theme = theme_from_string(status.theme);
            initial_status_applied_ = true;
            last_daemon_theme_ = status.theme;
        } else if (status.theme != last_daemon_theme_) {
            state_.theme = theme_from_string(status.theme);
            last_daemon_theme_ = status.theme;
        }
    }

    if (state_.status.folders.empty()) {
        state_.folder_cursor = 0;
    } else if (state_.folder_cursor >= static_cast<int>(state_.status.folders.size())) {
        state_.folder_cursor = static_cast<int>(state_.status.folders.size()) - 1;
    } else if (state_.folder_cursor < 0) {
        state_.folder_cursor = 0;
    }

    if (state_.status.logs.empty()) {
        state_.log_scroll = 0;
    } else if (state_.log_scroll >= static_cast<int>(state_.status.logs.size())) {
        state_.log_scroll = static_cast<int>(state_.status.logs.size()) - 1;
    } else if (state_.log_scroll < 0) {
        state_.log_scroll = 0;
    }
}

void UiController::set_peers(const std::vector<TailscalePeer>& peers) {
    state_.peers = peers;
    state_.needs_peer_refresh = false;
    if (state_.peers.empty()) {
        state_.peer_cursor = 0;
    } else if (state_.peer_cursor >= static_cast<int>(state_.peers.size())) {
        state_.peer_cursor = static_cast<int>(state_.peers.size()) - 1;
    } else if (state_.peer_cursor < 0) {
        state_.peer_cursor = 0;
    }
}

void UiController::set_dir_picker_start_path(const std::string& path) {
    dir_picker_start_path_ = path;
}

void UiController::set_feedback(const std::string& message, bool is_error) {
    state_.feedback_banner = message;
    state_.feedback_is_error = is_error && !message.empty();
}

void UiController::clear_feedback() {
    state_.feedback_banner.clear();
    state_.feedback_is_error = false;
}

bool UiController::consume_peer_refresh() {
    bool v = state_.needs_peer_refresh;
    state_.needs_peer_refresh = false;
    return v;
}

IpcAction UiController::handle_key(char ch, KeyCode key) {
    if (state_.modal == ModalKind::TextInput) {
        return handle_text_input_key(ch, key);
    }

    KeyCode norm = normalize_key(ch, key);
    switch (state_.modal) {
        case ModalKind::None:
            return handle_global_key(ch, norm);
        case ModalKind::TargetPicker:
            return handle_target_picker_key(ch, norm);
        case ModalKind::DirPicker:
            return handle_dir_picker_key(ch, norm);
        case ModalKind::ConfirmRemove:
            return handle_confirm_remove_key(ch, norm);
        case ModalKind::TextInput:
            return handle_text_input_key(ch, key);
    }
    return IpcAction{};
}

void UiController::open_target_picker() {
    state_.modal = ModalKind::TargetPicker;
    state_.peer_cursor = 0;
    state_.modal_error.clear();
    state_.needs_peer_refresh = true;
}

void UiController::open_dir_picker() {
    std::string start = "/";
    if (is_valid_dir(dir_picker_start_path_)) {
        start = dir_picker_start_path_;
    } else {
        const char* home = std::getenv("HOME");
        if (home != nullptr && is_valid_dir(home)) {
            start = home;
        }
    }
    state_.modal = ModalKind::DirPicker;
    state_.modal_error.clear();
    load_directory(start);
}

void UiController::load_directory(const std::string& path, const std::string& select_name) {
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(fs::path(path), ec);
    if (ec || resolved.empty()) {
        resolved = fs::path(path);
    }

    state_.dir_current_path = resolved.string();
    state_.dir_entries.clear();

    for (fs::directory_iterator it(
             resolved,
             fs::directory_options::skip_permission_denied,
             ec
         ),
         end;
         !ec && it != end;
         it.increment(ec)) {
        const auto& entry = *it;
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec) || entry_ec) {
            continue;
        }
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') {
            continue;
        }
        state_.dir_entries.push_back(entry.path().string());
    }

    std::sort(
        state_.dir_entries.begin(),
        state_.dir_entries.end(),
        [](const std::string& a, const std::string& b) {
            std::string na = fs::path(a).filename().string();
            std::string nb = fs::path(b).filename().string();
            std::string la = to_lower_ascii(na);
            std::string lb = to_lower_ascii(nb);
            if (la != lb) {
                return la < lb;
            }
            return na < nb;
        }
    );

    if (state_.dir_entries.empty()) {
        state_.dir_cursor = -1;
        return;
    }

    state_.dir_cursor = 0;
    if (!select_name.empty()) {
        for (size_t i = 0; i < state_.dir_entries.size(); ++i) {
            if (fs::path(state_.dir_entries[i]).filename().string() == select_name) {
                state_.dir_cursor = static_cast<int>(i);
                break;
            }
        }
    }
}

void UiController::open_text_input(TextInputPurpose purpose) {
    state_.modal = ModalKind::TextInput;
    state_.text_purpose = purpose;
    state_.text_buffer.clear();
    state_.modal_error.clear();
}

void UiController::close_modal() {
    state_.modal = ModalKind::None;
    state_.text_purpose = TextInputPurpose::None;
    state_.text_buffer.clear();
    state_.modal_error.clear();
    state_.confirm_folder_id.clear();
    state_.confirm_folder_path.clear();
    state_.confirm_folder_label.clear();
}

IpcAction UiController::handle_global_key(char ch, KeyCode key) {
    IpcAction action;

    if (key == KeyCode::Escape) {
        action.type = ActionType::Quit;
        return action;
    }

    if (key == KeyCode::Enter) {
        action.type = ActionType::SyncNow;
        return action;
    }

    if (key == KeyCode::Left) {
        int idx = static_cast<int>(state_.tab);
        state_.tab = static_cast<ActiveTab>((idx + 2) % 3);
        return action;
    }

    if (key == KeyCode::Right) {
        int idx = static_cast<int>(state_.tab);
        state_.tab = static_cast<ActiveTab>((idx + 1) % 3);
        return action;
    }

    if (key == KeyCode::Up || (key == KeyCode::Char && std::tolower(static_cast<unsigned char>(ch)) == 'k')) {
        if (state_.tab == ActiveTab::Folders && state_.folder_cursor > 0) {
            --state_.folder_cursor;
        } else if (state_.tab == ActiveTab::Logs && state_.log_scroll > 0) {
            --state_.log_scroll;
        }
        return action;
    }

    if (key == KeyCode::Down || (key == KeyCode::Char && std::tolower(static_cast<unsigned char>(ch)) == 'j')) {
        if (state_.tab == ActiveTab::Folders) {
            int max_idx = static_cast<int>(state_.status.folders.size()) - 1;
            if (state_.folder_cursor < max_idx) {
                ++state_.folder_cursor;
            }
        } else if (state_.tab == ActiveTab::Logs) {
            int max_idx = static_cast<int>(state_.status.logs.size()) - 1;
            if (state_.log_scroll < max_idx) {
                ++state_.log_scroll;
            }
        }
        return action;
    }

    if (key == KeyCode::Delete) {
        if (state_.tab == ActiveTab::Folders && !state_.status.folders.empty()) {
            const auto& f = state_.status.folders[static_cast<size_t>(state_.folder_cursor)];
            state_.confirm_folder_id = f.id;
            state_.confirm_folder_path = f.path;
            state_.confirm_folder_label = f.label;
            state_.modal = ModalKind::ConfirmRemove;
        }
        return action;
    }

    if (key != KeyCode::Char) {
        return action;
    }

    char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    switch (lower) {
        case '1':
            state_.tab = ActiveTab::Status;
            break;
        case '2':
            state_.tab = ActiveTab::Folders;
            break;
        case '3':
            state_.tab = ActiveTab::Logs;
            break;
        case 't':
            state_.theme = toggle_theme(state_.theme);
            action.type = ActionType::SetTheme;
            action.theme = state_.theme;
            break;
        case 'q':
            action.type = ActionType::Quit;
            break;
        case 'c':
            if (state_.tab == ActiveTab::Status) {
                open_target_picker();
            }
            break;
        case 'a':
            if (state_.tab == ActiveTab::Folders) {
                open_dir_picker();
            }
            break;
        case 'd':
            if (state_.tab == ActiveTab::Folders && !state_.status.folders.empty()) {
                const auto& f = state_.status.folders[static_cast<size_t>(state_.folder_cursor)];
                state_.confirm_folder_id = f.id;
                state_.confirm_folder_path = f.path;
                state_.confirm_folder_label = f.label;
                state_.modal = ModalKind::ConfirmRemove;
            }
            break;
        case 'r':
            if (state_.tab == ActiveTab::Logs) {
                action.type = ActionType::RetryFailed;
            }
            break;
        default:
            break;
    }

    return action;
}

IpcAction UiController::handle_target_picker_key(char ch, KeyCode key) {
    IpcAction action;

    if (key == KeyCode::Escape) {
        close_modal();
        return action;
    }

    if (key == KeyCode::Up || (key == KeyCode::Char && std::tolower(static_cast<unsigned char>(ch)) == 'k')) {
        if (state_.peer_cursor > 0) {
            --state_.peer_cursor;
        }
        return action;
    }

    if (key == KeyCode::Down || (key == KeyCode::Char && std::tolower(static_cast<unsigned char>(ch)) == 'j')) {
        int max_idx = static_cast<int>(state_.peers.size()) - 1;
        if (state_.peer_cursor < max_idx) {
            ++state_.peer_cursor;
        }
        return action;
    }

    if (key == KeyCode::Enter) {
        if (!state_.peers.empty() && state_.peer_cursor >= 0 &&
            state_.peer_cursor < static_cast<int>(state_.peers.size())) {
            std::string host = resolve_peer_host(state_.peers[static_cast<size_t>(state_.peer_cursor)]);
            if (!host.empty()) {
                action.type = ActionType::SetTarget;
                action.target_host = host;
                action.target_port = 8765;
                close_modal();
            }
        }
        return action;
    }

    if (key == KeyCode::Char && std::tolower(static_cast<unsigned char>(ch)) == 't') {
        open_text_input(TextInputPurpose::TargetHost);
    }

    return action;
}

IpcAction UiController::handle_dir_picker_key(char ch, KeyCode key) {
    IpcAction action;

    if (key == KeyCode::Escape) {
        close_modal();
        return action;
    }

    if (key == KeyCode::Up || (key == KeyCode::Char && std::tolower(static_cast<unsigned char>(ch)) == 'k')) {
        if (state_.dir_cursor > 0) {
            --state_.dir_cursor;
        }
        return action;
    }

    if (key == KeyCode::Down || (key == KeyCode::Char && std::tolower(static_cast<unsigned char>(ch)) == 'j')) {
        int max_idx = static_cast<int>(state_.dir_entries.size()) - 1;
        if (state_.dir_cursor < max_idx) {
            ++state_.dir_cursor;
        }
        return action;
    }

    if (key == KeyCode::Enter) {
        if (state_.dir_cursor >= 0 &&
            state_.dir_cursor < static_cast<int>(state_.dir_entries.size())) {
            load_directory(state_.dir_entries[static_cast<size_t>(state_.dir_cursor)]);
        }
        return action;
    }

    if (key == KeyCode::Backspace) {
        fs::path cur(state_.dir_current_path);
        fs::path parent = cur.parent_path();
        if (!parent.empty() && parent != cur) {
            load_directory(parent.string(), cur.filename().string());
        }
        return action;
    }

    if (key == KeyCode::Space) {
        std::string chosen = state_.dir_current_path;
        if (state_.dir_cursor >= 0 &&
            state_.dir_cursor < static_cast<int>(state_.dir_entries.size())) {
            chosen = state_.dir_entries[static_cast<size_t>(state_.dir_cursor)];
        }
        if (!chosen.empty()) {
            action.type = ActionType::AddFolder;
            action.folder_path = chosen;
            close_modal();
        }
        return action;
    }

    if (key == KeyCode::Char) {
        char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lower == 't') {
            open_text_input(TextInputPurpose::FolderPath);
        } else if (lower == '.') {
            if (!state_.dir_current_path.empty()) {
                action.type = ActionType::AddFolder;
                action.folder_path = state_.dir_current_path;
                close_modal();
            }
        }
    }

    return action;
}

IpcAction UiController::handle_confirm_remove_key(char ch, KeyCode key) {
    IpcAction action;

    if (key == KeyCode::Escape) {
        close_modal();
        return action;
    }

    if (key == KeyCode::Char) {
        char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lower == 'y') {
            action.type = ActionType::RemoveFolder;
            action.folder_id = state_.confirm_folder_id;
            close_modal();
        } else if (lower == 'n') {
            close_modal();
        }
    }

    return action;
}

IpcAction UiController::handle_text_input_key(char ch, KeyCode key) {
    IpcAction action;

    if (key == KeyCode::Escape || (key == KeyCode::Char && static_cast<unsigned char>(ch) == 0x1b)) {
        close_modal();
        return action;
    }

    if (key == KeyCode::Backspace ||
        (key == KeyCode::Char && (ch == '\b' || static_cast<unsigned char>(ch) == 0x7f))) {
        if (!state_.text_buffer.empty()) {
            state_.text_buffer.pop_back();
            state_.modal_error.clear();
        }
        return action;
    }

    if (key == KeyCode::Enter || (key == KeyCode::Char && (ch == '\n' || ch == '\r'))) {
        if (state_.text_purpose == TextInputPurpose::TargetHost) {
            std::string host;
            int port = 8765;
            std::string err;
            if (!parse_host_port(state_.text_buffer, host, port, err)) {
                state_.modal_error = err;
                return action;
            }
            action.type = ActionType::SetTarget;
            action.target_host = host;
            action.target_port = port;
            close_modal();
        } else if (state_.text_purpose == TextInputPurpose::FolderPath) {
            std::string path = expand_tilde(trim_ascii(state_.text_buffer));
            if (path.empty()) {
                state_.modal_error = "Path cannot be empty";
                return action;
            }
            action.type = ActionType::AddFolder;
            action.folder_path = path;
            close_modal();
        }
        return action;
    }

    if (key == KeyCode::Space) {
        if (state_.text_buffer.size() < 256) {
            state_.text_buffer.push_back(' ');
            state_.modal_error.clear();
        }
        return action;
    }

    if (key == KeyCode::Char) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (uch >= 0x20 && uch <= 0x7e && state_.text_buffer.size() < 256) {
            state_.text_buffer.push_back(ch);
            state_.modal_error.clear();
        }
    }

    return action;
}

} // namespace cp0
