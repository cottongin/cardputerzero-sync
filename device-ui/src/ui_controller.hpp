#pragma once

#include "ipc_client.hpp"
#include "theme.hpp"

#include <string>
#include <vector>

namespace cp0 {

/// Top-level segmented tab views on the 320x170 UI.
enum class ActiveTab {
    Status = 0,
    Folders = 1,
    Logs = 2,
};

/// Active overlay modal state.
enum class ModalKind {
    None,
    TargetPicker,
    DirPicker,
    ConfirmRemove,
    TextInput,
};

/// Purpose of the shared TextInput modal.
enum class TextInputPurpose {
    None,
    TargetHost,
    FolderPath,
};

/// Normalized physical/simulator key events handled by UiController.
enum class KeyCode {
    Char,
    Enter,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Backspace,
    Delete,
    Space,
};

/// Side-effect actions emitted by UiController for the event loop / IPC client.
enum class ActionType {
    None,
    Quit,
    SyncNow,
    AddFolder,
    RemoveFolder,
    SetTarget,
    SetTheme,
    RetryFailed,
};

/// Structured command emitted by UiController::handle_key.
struct IpcAction {
    ActionType type = ActionType::None;
    std::string target_host;
    int target_port = 8765;
    std::string folder_path;
    std::string folder_label;
    std::string folder_id;
    ThemeMode theme = ThemeMode::Dark;
};

/// Complete view-model state rendered by LvglAppView.
struct UiState {
    ActiveTab tab = ActiveTab::Status;
    ThemeMode theme = ThemeMode::Dark;
    ModalKind modal = ModalKind::None;
    TextInputPurpose text_purpose = TextInputPurpose::None;

    DaemonStatus status;
    std::vector<TailscalePeer> peers;
    int peer_cursor = 0;

    int folder_cursor = 0;
    int log_scroll = 0;

    std::string dir_current_path;
    std::vector<std::string> dir_entries;
    int dir_cursor = -1;

    std::string confirm_folder_id;
    std::string confirm_folder_path;
    std::string confirm_folder_label;

    std::string text_buffer;
    std::string modal_error;
    std::string feedback_banner;
    bool feedback_is_error = false;

    bool needs_peer_refresh = false;
};

/// Pure keyboard-first UI state machine decoupled from graphics/display backends.
class UiController {
public:
    UiController() = default;

    const UiState& state() const { return state_; }

    /// Apply a freshly polled DaemonStatus snapshot, preserving active cursors safely.
    void update_status(const DaemonStatus& status);

    /// Populate Tailscale peers for TargetPicker.
    void set_peers(const std::vector<TailscalePeer>& peers);

    /// Override starting directory for DirPicker (defaults to $HOME or /).
    void set_dir_picker_start_path(const std::string& path);

    /// Set or clear transient status feedback displayed in the UI.
    void set_feedback(const std::string& message, bool is_error = false);
    void clear_feedback();

    /// Return true once when TargetPicker was opened and needs peer discovery.
    bool consume_peer_refresh();

    /// Process a normalized key event and return any resulting IPC/lifecycle action.
    IpcAction handle_key(char ch, KeyCode key = KeyCode::Char);

private:
    void open_target_picker();
    void open_dir_picker();
    void load_directory(const std::string& path, const std::string& select_name = "");
    void open_text_input(TextInputPurpose purpose);
    void close_modal();

    IpcAction handle_global_key(char ch, KeyCode key);
    IpcAction handle_target_picker_key(char ch, KeyCode key);
    IpcAction handle_dir_picker_key(char ch, KeyCode key);
    IpcAction handle_confirm_remove_key(char ch, KeyCode key);
    IpcAction handle_text_input_key(char ch, KeyCode key);

    UiState state_;
    std::string dir_picker_start_path_;
    bool initial_status_applied_ = false;
    std::string last_daemon_theme_;
};

} // namespace cp0
