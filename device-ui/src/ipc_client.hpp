#pragma once

#include "theme.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cp0 {

/// Per-watched-folder queue and existence snapshot.
struct FolderStatus {
    std::string id;
    std::string path;
    std::string label;
    bool enabled = true;
    bool exists = false;
    int pending = 0;
    int uploading = 0;
    int synced = 0;
    int failed = 0;
    int64_t pending_bytes = 0;
};

/// Single log line from the daemon's SQLite log ring buffer.
struct LogEntry {
    int64_t id = 0;
    int64_t ts = 0;
    std::string level;
    std::string message;
};

/// Full status snapshot from IPC `status` or read-only SQLite + config fallback.
struct DaemonStatus {
    bool ok = false;
    bool from_fallback = false;
    std::string error;
    std::string target_host;
    int target_port = 8765;
    std::string theme = "dark";
    bool mac_online = false;
    std::string mac_dest_root;
    bool syncing = false;
    std::string current_file;
    int pending_count = 0;
    int64_t pending_bytes = 0;
    int synced_count = 0;
    int failed_count = 0;
    int next_retry_sec = 0;
    int64_t last_sync_ts = 0;
    std::string last_synced_file;
    std::string last_error;
    std::vector<FolderStatus> folders;
    std::vector<LogEntry> logs;
};

/// Result of a mutating IPC command (`sync_now`, `add_folder`, etc.).
struct CommandResult {
    bool ok = false;
    std::string error;
    std::string raw_json;
};

/// Non-self Tailscale peer extracted from `tailscale status --json`.
struct TailscalePeer {
    std::string id;
    std::string hostname;
    std::string dns_name;
    std::string os;
    bool online = false;
    std::vector<std::string> tailscale_ips;
};

/// Return default Unix socket path (`CP0_SYNC_SOCK` or `${XDG_RUNTIME_DIR:-/tmp}/cp0-sync.sock`).
std::string default_sock_path();

/// Return default SQLite state DB path (`CP0_SYNC_DB` or `$HOME/.local/share/cp0-sync/state.db`).
std::string default_db_path();

/// Return default daemon config path (`CP0_SYNC_CONFIG` or `$HOME/.config/cp0-sync/config.json`).
std::string default_config_path();

/// Parse `status` JSON response string into a DaemonStatus struct.
DaemonStatus parse_status_json(const std::string& json_str);

/// Parse `tailscale status --json` output, excluding Self and sorting online macOS/darwin peers first.
std::vector<TailscalePeer> parse_tailscale_peers(const std::string& json_str);

/// Unix domain socket IPC client with read-only SQLite fallback and Tailscale peer discovery.
class IpcClient {
public:
    IpcClient() = default;
    explicit IpcClient(std::string sock_path, std::string db_path = "", std::string config_path = "");

    /// Fetch live daemon status over AF_UNIX, falling back to read-only SQLite if socket is unreachable.
    DaemonStatus fetch_status() const;

    /// Send a single newline-delimited JSON command over AF_UNIX with a non-blocking timeout.
    std::string send_command(const std::string& json_line, int timeout_ms = 1000) const;

    /// Read cached state directly from SQLite (`SQLITE_OPEN_READONLY`) and `config.json`.
    DaemonStatus read_sqlite_fallback() const;

    /// Execute `tailscale status --json` locally and return sorted non-self peers.
    std::vector<TailscalePeer> list_tailscale_peers() const;

    // Typed command helpers matching Task 4 IPC protocol
    CommandResult sync_now() const;
    CommandResult add_folder(const std::string& path, const std::string& label = "") const;
    CommandResult remove_folder(const std::string& folder_id) const;
    CommandResult set_target(const std::string& host, int port = 8765) const;
    CommandResult set_theme(ThemeMode theme) const;
    CommandResult retry_failed() const;

private:
    std::string resolved_sock_path() const;
    std::string resolved_db_path() const;
    std::string resolved_config_path() const;
    CommandResult run_json_command(const std::string& payload) const;

    std::string sock_path_;
    std::string db_path_;
    std::string config_path_;
};

} // namespace cp0
