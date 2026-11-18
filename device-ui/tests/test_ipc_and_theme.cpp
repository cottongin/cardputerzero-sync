#include "ipc_client.hpp"
#include "theme.hpp"

#include <sqlite3.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
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

void test_theme_tokens() {
    auto dark = cp0::get_palette(cp0::ThemeMode::Dark);
    assert(dark.background == 0x09090b);
    assert(dark.card == 0x09090b);
    assert(dark.muted == 0x18181b);
    assert(dark.border == 0x27272a);
    assert(dark.ring == 0xd4d4d8);
    assert(dark.foreground == 0xfafafa);
    assert(dark.muted_foreground == 0xa1a1aa);
    assert(dark.active_tab_bg == 0x27272a);
    assert(dark.active_tab_fg == 0xfafafa);
    assert(dark.active_tab_border == 0x27272a);
    assert(dark.status_online == 0x4ade80);
    assert(dark.status_online_bg == 0x11261a);
    assert(dark.status_sleeping == 0xfbbf24);
    assert(dark.status_sleeping_bg == 0x271d0b);
    assert(dark.status_error == 0xf87171);
    assert(dark.status_error_bg == 0x2b1214);

    auto light = cp0::get_palette(cp0::ThemeMode::Light);
    assert(light.background == 0xfafafa);
    assert(light.card == 0xffffff);
    assert(light.muted == 0xf4f4f5);
    assert(light.border == 0xe4e4e7);
    assert(light.ring == 0x71717a);
    assert(light.foreground == 0x09090b);
    assert(light.muted_foreground == 0x71717a);
    assert(light.active_tab_bg == 0xffffff);
    assert(light.active_tab_fg == 0x09090b);
    assert(light.active_tab_border == 0xe4e4e7);
    assert(light.status_online == 0x16a34a);
    assert(light.status_online_bg == 0xdcfce7);
    assert(light.status_sleeping == 0xd97706);
    assert(light.status_sleeping_bg == 0xfef3c7);
    assert(light.status_error == 0xdc2626);
    assert(light.status_error_bg == 0xfee2e2);

    assert(cp0::toggle_theme(cp0::ThemeMode::Dark) == cp0::ThemeMode::Light);
    assert(cp0::toggle_theme(cp0::ThemeMode::Light) == cp0::ThemeMode::Dark);
    assert(cp0::theme_to_string(cp0::ThemeMode::Dark) == "dark");
    assert(cp0::theme_to_string(cp0::ThemeMode::Light) == "light");
    assert(cp0::theme_from_string("light") == cp0::ThemeMode::Light);
    assert(cp0::theme_from_string("dark") == cp0::ThemeMode::Dark);
    assert(cp0::theme_from_string("unknown") == cp0::ThemeMode::Dark);
}

void test_tailscale_peer_parsing() {
    const char* ts_json = R"({
        "Self": {"ID": "self1", "HostName": "cardputerzero", "OS": "linux"},
        "Peer": {
            "self1": {"ID": "self1", "HostName": "cardputerzero", "DNSName": "cp0.ts.net.", "OS": "linux", "Online": true, "TailscaleIPs": ["100.64.0.1"]},
            "p1": {"ID": "p1", "HostName": "linux-box", "DNSName": "linux.ts.net.", "OS": "linux", "Online": true, "TailscaleIPs": ["100.64.0.2"]},
            "p2": {"ID": "p2", "HostName": "macbook-offline", "DNSName": "mac-off.ts.net.", "OS": "macOS", "Online": false, "TailscaleIPs": ["100.64.0.4"]},
            "p3": {"ID": "p3", "HostName": "macbook", "DNSName": "macbook.ts.net.", "OS": "macOS", "Online": true, "TailscaleIPs": ["100.64.0.3", "fd7a:115c:a1e0::3"]},
            "p4": {"ID": "p4", "HostName": "imac-darwin", "DNSName": "imac.ts.net.", "OS": "darwin", "Online": true, "TailscaleIPs": ["100.64.0.5"]}
        }
    })";

    auto peers = cp0::parse_tailscale_peers(ts_json);
    assert(peers.size() == 4);
    // Online macOS/darwin peers first (alphabetical by hostname: imac-darwin, macbook)
    assert(peers[0].hostname == "imac-darwin");
    assert(peers[0].os == "darwin");
    assert(peers[0].online == true);
    assert(peers[0].dns_name == "imac.ts.net");
    assert(peers[1].hostname == "macbook");
    assert(peers[1].os == "macOS");
    assert(peers[1].online == true);
    assert(peers[1].dns_name == "macbook.ts.net");
    assert(peers[1].tailscale_ips.size() == 2);
    assert(peers[1].tailscale_ips[0] == "100.64.0.3");
    // Offline macOS peer next (still macOS group before linux)
    assert(peers[2].hostname == "macbook-offline");
    assert(peers[2].online == false);
    // Online non-macOS peer last
    assert(peers[3].hostname == "linux-box");
    assert(peers[3].os == "linux");

    // Invalid JSON returns empty gracefully
    assert(cp0::parse_tailscale_peers("{not-json").empty());
    assert(cp0::parse_tailscale_peers("").empty());
}

void test_ipc_socket_round_trip() {
    TempDir tmp("cp0_ipc");
    fs::path sock_path = tmp.path / "cp0-sync.sock";

    int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(srv_fd >= 0);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    assert(sock_path.string().size() < sizeof(addr.sun_path));
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    unlink(sock_path.c_str());
    assert(bind(srv_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(listen(srv_fd, 8) == 0);

    std::vector<std::string> received_cmds;
    std::atomic<bool> stop{false};

    std::thread server_thread([&]() {
        while (!stop.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(srv_fd, &rfds);
            timeval tv{0, 50000};
            int ready = select(srv_fd + 1, &rfds, nullptr, nullptr, &tv);
            if (ready <= 0) {
                continue;
            }
            int cli_fd = accept(srv_fd, nullptr, nullptr);
            if (cli_fd < 0) {
                continue;
            }
            std::string req;
            char ch = 0;
            while (read(cli_fd, &ch, 1) == 1) {
                if (ch == '\n') {
                    break;
                }
                req.push_back(ch);
            }
            received_cmds.push_back(req);

            std::string resp;
            if (req.find("\"status\"") != std::string::npos) {
                resp = R"({"ok":true,"target_host":"macbook.ts.net","target_port":8765,"theme":"light","mac_online":true,"mac_dest_root":"/Users/user/Pictures/CardputerZero","syncing":true,"current_file":"IMG_0042.JPG","pending_count":3,"pending_bytes":4096,"synced_count":12,"failed_count":1,"next_retry_sec":15,"last_sync_ts":1731900000000,"last_synced_file":"IMG_0041.JPG","last_error":"none","folders":[{"id":"camera","path":"/home/user/Pictures/DCIM/Camera","label":"Camera","enabled":true,"exists":true,"pending":3,"uploading":1,"synced":12,"failed":1,"pending_bytes":4096}],"logs":[{"id":10,"ts":1731900001000,"level":"INFO","message":"Synced \"IMG_0041.JPG\""}]})"
                       "\n";
            } else if (req.find("\"set_theme\"") != std::string::npos) {
                resp = R"({"ok":true,"theme":"light"})" "\n";
            } else if (req.find("\"add_folder\"") != std::string::npos) {
                resp = R"({"ok":true,"enqueued":2})" "\n";
            } else if (req.find("\"remove_folder\"") != std::string::npos) {
                resp = R"({"ok":true,"purged":0})" "\n";
            } else if (req.find("\"set_target\"") != std::string::npos) {
                resp = R"({"ok":true,"target_host":"mac.local","target_port":9000})" "\n";
            } else if (req.find("\"sync_now\"") != std::string::npos) {
                resp = R"({"ok":true,"enqueued":0})" "\n";
            } else if (req.find("\"retry_failed\"") != std::string::npos) {
                resp = R"({"ok":true,"retried":1})" "\n";
            } else {
                resp = R"({"ok":false,"error":"unknown"})" "\n";
            }

            size_t sent = 0;
            while (sent < resp.size()) {
                ssize_t n = write(cli_fd, resp.data() + sent, resp.size() - sent);
                if (n <= 0) break;
                sent += static_cast<size_t>(n);
            }
            close(cli_fd);
        }
    });

    cp0::IpcClient client(sock_path.string(), (tmp.path / "missing.db").string(), (tmp.path / "missing.json").string());

    auto st = client.fetch_status();
    if (!st.ok) {
        std::cerr << "fetch_status failed: error=" << st.error
                  << " raw=" << client.send_command(R"({"cmd":"status"})", 1000) << "\n";
    }
    assert(st.ok == true);
    assert(st.from_fallback == false);
    assert(st.target_host == "macbook.ts.net");
    assert(st.target_port == 8765);
    assert(st.theme == "light");
    assert(st.mac_online == true);
    assert(st.mac_dest_root == "/Users/user/Pictures/CardputerZero");
    assert(st.syncing == true);
    assert(st.current_file == "IMG_0042.JPG");
    assert(st.pending_count == 3);
    assert(st.pending_bytes == 4096);
    assert(st.synced_count == 12);
    assert(st.failed_count == 1);
    assert(st.next_retry_sec == 15);
    assert(st.last_sync_ts == 1731900000000LL);
    assert(st.last_synced_file == "IMG_0041.JPG");
    assert(st.last_error == "none");
    assert(st.folders.size() == 1);
    assert(st.folders[0].id == "camera");
    assert(st.folders[0].path == "/home/user/Pictures/DCIM/Camera");
    assert(st.folders[0].label == "Camera");
    assert(st.folders[0].enabled == true);
    assert(st.folders[0].exists == true);
    assert(st.folders[0].pending == 3);
    assert(st.folders[0].uploading == 1);
    assert(st.folders[0].synced == 12);
    assert(st.folders[0].failed == 1);
    assert(st.folders[0].pending_bytes == 4096);
    assert(st.logs.size() == 1);
    assert(st.logs[0].id == 10);
    assert(st.logs[0].ts == 1731900001000LL);
    assert(st.logs[0].level == "INFO");
    assert(st.logs[0].message == "Synced \"IMG_0041.JPG\"");

    assert(client.sync_now().ok);
    assert(client.add_folder("/tmp/My \"Photos\"", "Trips").ok);
    assert(client.remove_folder("camera").ok);
    assert(client.set_target("mac.local", 9000).ok);
    assert(client.set_theme(cp0::ThemeMode::Light).ok);
    assert(client.retry_failed().ok);

    // Check escaped JSON serialization in add_folder
    assert(received_cmds.size() == 7);
    assert(received_cmds[2].find("\\\"Photos\\\"") != std::string::npos);

    stop.store(true);
    server_thread.join();
    close(srv_fd);
}

void test_sqlite_fallback_and_config() {
    TempDir tmp("cp0_sqlite");
    fs::path db_path = tmp.path / "state.db";
    fs::path cfg_path = tmp.path / "config.json";
    fs::path cam_dir = tmp.path / "Camera";
    fs::create_directories(cam_dir);

    {
        std::ofstream cfg_out(cfg_path);
        cfg_out << R"({
            "target_host": "studio-mac.ts.net",
            "target_port": 8777,
            "theme": "light",
            "watched_folders": [
                {"id": "cam1", "path": ")" << cam_dir.string() << R"(", "label": "Camera", "enabled": true},
                {"id": "sd1", "path": "/media/missing-sd", "label": "SD", "enabled": true}
            ]
        })";
    }

    sqlite3* db = nullptr;
    assert(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
    const char* schema_sql = R"SQL(
        PRAGMA journal_mode=WAL;
        CREATE TABLE files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            folder_id TEXT NOT NULL,
            abs_path TEXT NOT NULL,
            rel_path TEXT NOT NULL,
            size_bytes INTEGER NOT NULL,
            mtime_ns INTEGER NOT NULL,
            sha256 TEXT NOT NULL,
            status TEXT NOT NULL,
            retry_count INTEGER NOT NULL DEFAULT 0,
            last_error TEXT,
            discovered_at INTEGER NOT NULL,
            synced_at INTEGER
        );
        CREATE TABLE logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts INTEGER NOT NULL,
            level TEXT NOT NULL,
            message TEXT NOT NULL
        );
        CREATE TABLE meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        INSERT INTO meta(key, value) VALUES
            ('mac_online', 'false'),
            ('mac_dest_root', '/Users/user/Photos'),
            ('last_sync_ts', '1731900500000'),
            ('last_synced_file', 'DSC0001.JPG'),
            ('last_error', 'connection refused');
        INSERT INTO files(folder_id, abs_path, rel_path, size_bytes, mtime_ns, sha256, status, discovered_at)
        VALUES
            ('cam1', '/tmp/a.jpg', 'a.jpg', 1000, 1, 'h1', 'PENDING', 100),
            ('cam1', '/tmp/b.jpg', 'b.jpg', 2500, 2, 'h2', 'UPLOADING', 101),
            ('cam1', '/tmp/c.jpg', 'c.jpg', 3000, 3, 'h3', 'SYNCED', 102),
            ('sd1', '/tmp/d.jpg', 'd.jpg', 500, 4, 'h4', 'FAILED', 103);
        INSERT INTO logs(ts, level, message) VALUES
            (1731900100000, 'INFO', 'Daemon booted'),
            (1731900200000, 'WARN', 'Mac unreachable');
    )SQL";
    char* errmsg = nullptr;
    assert(sqlite3_exec(db, schema_sql, nullptr, nullptr, &errmsg) == SQLITE_OK);
    sqlite3_close(db);

    // Point socket at a non-existent path so fetch_status() exercises SQLite fallback too
    cp0::IpcClient client((tmp.path / "nonexistent.sock").string(), db_path.string(), cfg_path.string());

    auto st = client.read_sqlite_fallback();
    assert(st.ok == true);
    assert(st.from_fallback == true);
    assert(st.target_host == "studio-mac.ts.net");
    assert(st.target_port == 8777);
    assert(st.theme == "light");
    assert(st.mac_online == false);
    assert(st.mac_dest_root == "/Users/user/Photos");
    assert(st.last_sync_ts == 1731900500000LL);
    assert(st.last_synced_file == "DSC0001.JPG");
    assert(st.last_error == "connection refused");
    assert(st.pending_count == 2);
    assert(st.pending_bytes == 3500);
    assert(st.synced_count == 1);
    assert(st.failed_count == 1);
    assert(st.folders.size() == 2);
    assert(st.folders[0].id == "cam1");
    assert(st.folders[0].exists == true);
    assert(st.folders[0].pending == 2);
    assert(st.folders[0].uploading == 1);
    assert(st.folders[0].synced == 1);
    assert(st.folders[0].pending_bytes == 3500);
    assert(st.folders[1].id == "sd1");
    assert(st.folders[1].exists == false);
    assert(st.folders[1].failed == 1);
    assert(st.logs.size() == 2);
    assert(st.logs[0].level == "WARN");
    assert(st.logs[0].message == "Mac unreachable");
    assert(st.logs[1].level == "INFO");

    // fetch_status() falls back automatically when socket is unreachable
    auto via_fetch = client.fetch_status();
    assert(via_fetch.ok == true);
    assert(via_fetch.from_fallback == true);
    assert(via_fetch.pending_count == 2);

    // SQLite fallback without config.json still aggregates DB counts & logs
    cp0::IpcClient db_only((tmp.path / "nonexistent.sock").string(), db_path.string(), (tmp.path / "no-cfg.json").string());
    auto db_only_st = db_only.read_sqlite_fallback();
    assert(db_only_st.ok == true);
    assert(db_only_st.pending_count == 2);
    assert(db_only_st.synced_count == 1);
    assert(db_only_st.failed_count == 1);
    assert(db_only_st.folders.size() == 2);

    // Missing DB returns ok=false
    cp0::IpcClient missing_all((tmp.path / "nonexistent.sock").string(), (tmp.path / "no.db").string(), (tmp.path / "no-cfg.json").string());
    assert(missing_all.read_sqlite_fallback().ok == false);
    assert(missing_all.fetch_status().ok == false);

    // Corrupt / non-SQLite DB file returns ok=false with SQLite error
    fs::path corrupt_db = tmp.path / "corrupt.db";
    {
        std::ofstream bad_out(corrupt_db, std::ios::binary);
        bad_out << "this is definitely not a sqlite database header";
    }
    cp0::IpcClient corrupt_client((tmp.path / "nonexistent.sock").string(), corrupt_db.string(), cfg_path.string());
    auto corrupt_st = corrupt_client.read_sqlite_fallback();
    assert(corrupt_st.ok == false);
    assert(!corrupt_st.error.empty());
    assert(!corrupt_st.last_error.empty());

    // Empty / schema-less 0-byte DB file returns ok=false
    fs::path empty_db = tmp.path / "empty.db";
    {
        std::ofstream empty_out(empty_db, std::ios::binary);
    }
    cp0::IpcClient empty_client((tmp.path / "nonexistent.sock").string(), empty_db.string(), cfg_path.string());
    auto empty_st = empty_client.read_sqlite_fallback();
    assert(empty_st.ok == false);
    assert(!empty_st.error.empty());
    assert(!empty_st.last_error.empty());
}

void test_status_parsing_and_env_paths() {
    // Exact daemon.py _cmd_status shape (without optional uploading/failed/pending_bytes in folders)
    const char* daemon_json = R"({
        "ok": true,
        "target_host": "my-mac.ts.net",
        "target_port": 8765,
        "theme": "dark",
        "mac_online": false,
        "mac_dest_root": "",
        "syncing": false,
        "current_file": "",
        "pending_count": 0,
        "pending_bytes": 0,
        "synced_count": 5,
        "failed_count": 0,
        "next_retry_sec": 0,
        "last_sync_ts": 0,
        "last_synced_file": "",
        "last_error": "",
        "folders": [
            {"id": "camera", "path": "/home/cp0/Pictures/DCIM/Camera", "label": "Camera", "enabled": true, "exists": false, "pending": 0, "synced": 5}
        ],
        "logs": []
    })";
    auto st = cp0::parse_status_json(daemon_json);
    assert(st.ok == true);
    assert(st.folders.size() == 1);
    assert(st.folders[0].id == "camera");
    assert(st.folders[0].exists == false);
    assert(st.folders[0].synced == 5);
    assert(st.folders[0].uploading == 0);
    assert(st.folders[0].failed == 0);
    assert(st.folders[0].pending_bytes == 0);

    auto err_st = cp0::parse_status_json(R"({"ok":false,"error":"bad cmd"})");
    assert(err_st.ok == false);
    assert(err_st.error == "bad cmd");

    // Env overrides for default paths
    setenv("CP0_SYNC_SOCK", "/tmp/custom-cp0.sock", 1);
    setenv("CP0_SYNC_DB", "/tmp/custom-cp0.db", 1);
    setenv("CP0_SYNC_CONFIG", "/tmp/custom-cp0.json", 1);
    assert(cp0::default_sock_path() == "/tmp/custom-cp0.sock");
    assert(cp0::default_db_path() == "/tmp/custom-cp0.db");
    assert(cp0::default_config_path() == "/tmp/custom-cp0.json");
    unsetenv("CP0_SYNC_SOCK");
    unsetenv("CP0_SYNC_DB");
    unsetenv("CP0_SYNC_CONFIG");
}

void test_list_tailscale_peers_subprocess() {
    TempDir tmp("cp0_ts_bin");
    fs::path fake_bin = tmp.path / "tailscale";
    {
        std::ofstream out(fake_bin);
        out << "#!/bin/sh\n"
            << "echo '{\"Self\":{\"ID\":\"s1\"},\"Peer\":{\"p1\":{\"HostName\":\"mini\",\"DNSName\":\"mini.ts.net.\",\"OS\":\"macOS\",\"Online\":true,\"TailscaleIPs\":[\"100.64.0.9\"]}}}'\n";
    }
    fs::permissions(fake_bin, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);

    const char* old_path_env = std::getenv("PATH");
    const bool had_path = (old_path_env != nullptr);
    const std::string saved_path = had_path ? old_path_env : "";
    std::string new_path = tmp.path.string() + (had_path ? (std::string(":") + saved_path) : "");
    setenv("PATH", new_path.c_str(), 1);

    cp0::IpcClient client;
    auto peers = client.list_tailscale_peers();
    if (had_path) {
        setenv("PATH", saved_path.c_str(), 1);
    } else {
        unsetenv("PATH");
    }

    assert(peers.size() == 1);
    assert(peers[0].hostname == "mini");
    assert(peers[0].dns_name == "mini.ts.net");
    assert(peers[0].os == "macOS");
    assert(peers[0].online == true);
}

void test_sqlite_fallback_omits_removed_folders() {
    TempDir tmp("cp0_sqlite_removed");
    fs::path db_path = tmp.path / "state.db";
    fs::path cfg_path = tmp.path / "config.json";
    fs::path f1_dir = tmp.path / "f1";
    fs::create_directories(f1_dir);

    sqlite3* db = nullptr;
    assert(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
    const char* schema_sql = R"SQL(
        CREATE TABLE files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            folder_id TEXT NOT NULL,
            abs_path TEXT NOT NULL,
            rel_path TEXT NOT NULL,
            size_bytes INTEGER NOT NULL,
            mtime_ns INTEGER NOT NULL,
            sha256 TEXT NOT NULL,
            status TEXT NOT NULL,
            retry_count INTEGER NOT NULL DEFAULT 0,
            last_error TEXT,
            discovered_at INTEGER NOT NULL,
            synced_at INTEGER
        );
        CREATE TABLE logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts INTEGER NOT NULL,
            level TEXT NOT NULL,
            message TEXT NOT NULL
        );
        CREATE TABLE meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        INSERT INTO files(folder_id, abs_path, rel_path, size_bytes, mtime_ns, sha256, status, discovered_at)
        VALUES
            ('f1', '/tmp/f1/a.jpg', 'a.jpg', 1000, 1, 'h1', 'SYNCED', 100),
            ('f2', '/tmp/f2/b.jpg', 'b.jpg', 2000, 2, 'h2', 'SYNCED', 101);
    )SQL";
    char* errmsg = nullptr;
    assert(sqlite3_exec(db, schema_sql, nullptr, nullptr, &errmsg) == SQLITE_OK);
    sqlite3_close(db);

    // 1. Valid config.json with f1 only: f2 must NOT be resurrected as a ghost folder
    {
        std::ofstream cfg_out(cfg_path);
        cfg_out << R"({
            "target_host": "mac.ts.net",
            "target_port": 8765,
            "theme": "dark",
            "watched_folders": [
                {"id": "f1", "path": ")" << f1_dir.string() << R"(", "label": "F1", "enabled": true}
            ]
        })";
    }
    cp0::IpcClient client((tmp.path / "nonexistent.sock").string(), db_path.string(), cfg_path.string());
    auto st = client.read_sqlite_fallback();
    assert(st.ok == true);
    assert(st.from_fallback == true);
    assert(st.synced_count == 2);
    assert(st.folders.size() == 1);
    assert(st.folders[0].id == "f1");
    assert(st.folders[0].synced == 1);

    // 2. Valid config.json with empty watched_folders []: no folders resurrected
    {
        std::ofstream cfg_out(cfg_path, std::ios::trunc);
        cfg_out << R"({"target_host": "mac.ts.net", "target_port": 8765, "watched_folders": []})";
    }
    auto empty_cfg_st = client.read_sqlite_fallback();
    assert(empty_cfg_st.ok == true);
    assert(empty_cfg_st.folders.empty());

    // 3. Missing config.json: fallback synthesizes both f1 and f2 from files
    cp0::IpcClient missing_cfg((tmp.path / "nonexistent.sock").string(), db_path.string(), (tmp.path / "missing.json").string());
    auto missing_st = missing_cfg.read_sqlite_fallback();
    assert(missing_st.ok == true);
    assert(missing_st.folders.size() == 2);
    assert(missing_st.folders[0].id == "f1");
    assert(missing_st.folders[1].id == "f2");

    // 4. Corrupt config.json: fallback synthesizes both f1 and f2 from files
    fs::path corrupt_cfg = tmp.path / "corrupt.json";
    {
        std::ofstream bad_out(corrupt_cfg);
        bad_out << "{not valid json";
    }
    cp0::IpcClient corrupt_client((tmp.path / "nonexistent.sock").string(), db_path.string(), corrupt_cfg.string());
    auto corrupt_st = corrupt_client.read_sqlite_fallback();
    assert(corrupt_st.ok == true);
    assert(corrupt_st.folders.size() == 2);
}

} // namespace

int main() {
    test_theme_tokens();
    test_tailscale_peer_parsing();
    test_ipc_socket_round_trip();
    test_sqlite_fallback_and_config();
    test_sqlite_fallback_omits_removed_folders();
    test_status_parsing_and_env_paths();
    test_list_tailscale_peers_subprocess();
    std::cout << "All test_ipc_and_theme assertions passed.\n";
    return 0;
}
