#include "ipc_client.hpp"

#include <sqlite3.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace cp0 {

namespace {

// ---------------------------------------------------------------------------
// Path & string helpers
// ---------------------------------------------------------------------------

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

std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

bool dir_exists(const std::string& raw_path) {
    if (raw_path.empty()) {
        return false;
    }
    std::string p = expand_tilde(raw_path);
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

int64_t parse_int64_safe(const std::string& s, int64_t fallback = 0) {
    if (s.empty()) {
        return fallback;
    }
    char* end = nullptr;
    errno = 0;
    long long val = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') {
        return fallback;
    }
    return static_cast<int64_t>(val);
}

// ---------------------------------------------------------------------------
// Dependency-free JSON parser & serializer
// ---------------------------------------------------------------------------

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool bool_val = false;
    double num_val = 0.0;
    int64_t int_val = 0;
    bool is_int = false;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::vector<std::pair<std::string, JsonValue>> obj_val;

    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) {
            return nullptr;
        }
        for (const auto& kv : obj_val) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }

    std::string as_string(const std::string& def = "") const {
        return type == Type::String ? str_val : def;
    }

    bool as_bool(bool def = false) const {
        return type == Type::Bool ? bool_val : def;
    }

    int64_t as_int64(int64_t def = 0) const {
        if (type != Type::Number) {
            return def;
        }
        return is_int ? int_val : static_cast<int64_t>(num_val);
    }

    int as_int(int def = 0) const {
        return static_cast<int>(as_int64(def));
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : src_(src) {}

    bool parse(JsonValue& out) {
        skip_ws();
        if (!parse_value(out, 0)) {
            return false;
        }
        skip_ws();
        return pos_ == src_.size();
    }

private:
    static constexpr int kMaxDepth = 64;

    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool parse_value(JsonValue& out, int depth) {
        if (depth > kMaxDepth || pos_ >= src_.size()) {
            return false;
        }
        char c = src_[pos_];
        if (c == 'n') return parse_null(out);
        if (c == 't' || c == 'f') return parse_bool(out);
        if (c == '"') return parse_string_value(out);
        if (c == '[') return parse_array(out, depth + 1);
        if (c == '{') return parse_object(out, depth + 1);
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
        return false;
    }

    bool parse_null(JsonValue& out) {
        if (src_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            out = JsonValue{};
            out.type = JsonValue::Type::Null;
            return true;
        }
        return false;
    }

    bool parse_bool(JsonValue& out) {
        if (src_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = JsonValue{};
            out.type = JsonValue::Type::Bool;
            out.bool_val = true;
            return true;
        }
        if (src_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = JsonValue{};
            out.type = JsonValue::Type::Bool;
            out.bool_val = false;
            return true;
        }
        return false;
    }

    static void append_utf8(std::string& dst, uint32_t cp) {
        if (cp <= 0x7F) {
            dst.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            dst.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            dst.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            dst.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_hex4(uint32_t& code) {
        if (pos_ + 4 > src_.size()) {
            return false;
        }
        code = 0;
        for (int i = 0; i < 4; ++i) {
            char c = src_[pos_++];
            code <<= 4;
            if (c >= '0' && c <= '9') {
                code |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool parse_raw_string(std::string& out) {
        if (pos_ >= src_.size() || src_[pos_] != '"') {
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= src_.size()) {
                return false;
            }
            char esc = src_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!parse_hex4(cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 <= src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t low = 0;
                            if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                                return false;
                            }
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                        } else {
                            return false;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool parse_string_value(JsonValue& out) {
        out = JsonValue{};
        out.type = JsonValue::Type::String;
        return parse_raw_string(out.str_val);
    }

    bool parse_number(JsonValue& out) {
        size_t start = pos_;
        if (src_[pos_] == '-') {
            ++pos_;
        }
        if (pos_ >= src_.size()) {
            return false;
        }
        if (src_[pos_] == '0') {
            ++pos_;
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        } else {
            return false;
        }

        bool is_float = false;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            is_float = true;
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                return false;
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            is_float = true;
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                return false;
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') {
                ++pos_;
            }
        }

        std::string token = src_.substr(start, pos_ - start);
        out = JsonValue{};
        out.type = JsonValue::Type::Number;

        char* end = nullptr;
        errno = 0;
        out.num_val = std::strtod(token.c_str(), &end);
        if (errno != 0 || end != token.c_str() + token.size()) {
            return false;
        }

        if (!is_float) {
            errno = 0;
            end = nullptr;
            long long iv = std::strtoll(token.c_str(), &end, 10);
            if (errno == 0 && end == token.c_str() + token.size()) {
                out.is_int = true;
                out.int_val = static_cast<int64_t>(iv);
            }
        }
        return true;
    }

    bool parse_array(JsonValue& out, int depth) {
        ++pos_; // '['
        out = JsonValue{};
        out.type = JsonValue::Type::Array;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (pos_ < src_.size()) {
            JsonValue elem;
            if (!parse_value(elem, depth)) {
                return false;
            }
            out.arr_val.push_back(std::move(elem));
            skip_ws();
            if (pos_ >= src_.size()) {
                return false;
            }
            if (src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            if (src_[pos_] != ',') {
                return false;
            }
            ++pos_;
            skip_ws();
        }
        return false;
    }

    bool parse_object(JsonValue& out, int depth) {
        ++pos_; // '{'
        out = JsonValue{};
        out.type = JsonValue::Type::Object;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < src_.size()) {
            skip_ws();
            std::string key;
            if (!parse_raw_string(key)) {
                return false;
            }
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                return false;
            }
            ++pos_;
            skip_ws();
            JsonValue val;
            if (!parse_value(val, depth)) {
                return false;
            }
            out.obj_val.emplace_back(std::move(key), std::move(val));
            skip_ws();
            if (pos_ >= src_.size()) {
                return false;
            }
            if (src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            if (src_[pos_] != ',') {
                return false;
            }
            ++pos_;
        }
        return false;
    }

    const std::string& src_;
    size_t pos_ = 0;
};

bool parse_json(const std::string& src, JsonValue& out) {
    JsonParser parser(src);
    return parser.parse(out);
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

// ---------------------------------------------------------------------------
// RAII helpers for fd & sqlite3
// ---------------------------------------------------------------------------

struct FdGuard {
    int fd = -1;
    ~FdGuard() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

struct SqliteDbGuard {
    sqlite3* db = nullptr;
    ~SqliteDbGuard() {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }
};

struct SqliteStmtGuard {
    sqlite3_stmt* stmt = nullptr;
    ~SqliteStmtGuard() {
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
        }
    }
};

std::string col_text(sqlite3_stmt* stmt, int col) {
    const unsigned char* txt = sqlite3_column_text(stmt, col);
    return txt != nullptr ? reinterpret_cast<const char*>(txt) : "";
}

int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return diff > 0 ? static_cast<int>(diff) : 1;
}

bool wait_poll(int fd, short events, std::chrono::steady_clock::time_point deadline) {
    while (true) {
        int rem = remaining_ms(deadline);
        if (rem <= 0) {
            return false;
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = events;
        int rc = ::poll(&pfd, 1, rem);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (rc == 0) {
            return false;
        }
        return (pfd.revents & (events | POLLHUP | POLLERR)) != 0;
    }
}

bool is_macos_os(const std::string& os) {
    std::string lower = to_lower_ascii(os);
    return lower == "macos" || lower == "darwin";
}

} // namespace

// ---------------------------------------------------------------------------
// Default paths
// ---------------------------------------------------------------------------

std::string default_sock_path() {
    const char* env_sock = std::getenv("CP0_SYNC_SOCK");
    if (env_sock != nullptr) {
        std::string trimmed = trim_ascii(env_sock);
        if (!trimmed.empty()) {
            return expand_tilde(trimmed);
        }
    }
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    std::string base = (runtime_dir != nullptr && !trim_ascii(runtime_dir).empty())
                           ? trim_ascii(runtime_dir)
                           : "/tmp";
    if (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/cp0-sync.sock";
}

std::string default_db_path() {
    const char* env_db = std::getenv("CP0_SYNC_DB");
    if (env_db != nullptr) {
        std::string trimmed = trim_ascii(env_db);
        if (!trimmed.empty()) {
            return expand_tilde(trimmed);
        }
    }
    const char* home = std::getenv("HOME");
    std::string home_str = (home != nullptr && home[0] != '\0') ? std::string(home) : "/tmp";
    if (!home_str.empty() && home_str.back() == '/') {
        home_str.pop_back();
    }
    return home_str + "/.local/share/cp0-sync/state.db";
}

std::string default_config_path() {
    const char* env_cfg = std::getenv("CP0_SYNC_CONFIG");
    if (env_cfg != nullptr) {
        std::string trimmed = trim_ascii(env_cfg);
        if (!trimmed.empty()) {
            return expand_tilde(trimmed);
        }
    }
    const char* home = std::getenv("HOME");
    std::string home_str = (home != nullptr && home[0] != '\0') ? std::string(home) : "/tmp";
    if (!home_str.empty() && home_str.back() == '/') {
        home_str.pop_back();
    }
    return home_str + "/.config/cp0-sync/config.json";
}

// ---------------------------------------------------------------------------
// JSON parsers
// ---------------------------------------------------------------------------

DaemonStatus parse_status_json(const std::string& json_str) {
    DaemonStatus status;
    JsonValue root;
    if (!parse_json(json_str, root) || root.type != JsonValue::Type::Object) {
        status.ok = false;
        status.error = "Invalid status JSON";
        return status;
    }

    if (const auto* v = root.find("ok")) {
        status.ok = v->as_bool(false);
    }
    if (const auto* v = root.find("error")) {
        status.error = v->as_string();
    }
    if (!status.ok) {
        if (status.error.empty()) {
            status.error = "Daemon returned error";
        }
        return status;
    }

    if (const auto* v = root.find("target_host")) status.target_host = v->as_string();
    if (const auto* v = root.find("target_port")) status.target_port = v->as_int(8765);
    if (const auto* v = root.find("theme")) status.theme = v->as_string("dark");
    if (const auto* v = root.find("mac_online")) status.mac_online = v->as_bool(false);
    if (const auto* v = root.find("mac_dest_root")) status.mac_dest_root = v->as_string();
    if (const auto* v = root.find("syncing")) status.syncing = v->as_bool(false);
    if (const auto* v = root.find("current_file")) status.current_file = v->as_string();
    if (const auto* v = root.find("pending_count")) status.pending_count = v->as_int(0);
    if (const auto* v = root.find("pending_bytes")) status.pending_bytes = v->as_int64(0);
    if (const auto* v = root.find("synced_count")) status.synced_count = v->as_int(0);
    if (const auto* v = root.find("failed_count")) status.failed_count = v->as_int(0);
    if (const auto* v = root.find("next_retry_sec")) status.next_retry_sec = v->as_int(0);
    if (const auto* v = root.find("last_sync_ts")) status.last_sync_ts = v->as_int64(0);
    if (const auto* v = root.find("last_synced_file")) status.last_synced_file = v->as_string();
    if (const auto* v = root.find("last_error")) status.last_error = v->as_string();

    if (const auto* folders = root.find("folders")) {
        if (folders->type == JsonValue::Type::Array) {
            for (const auto& item : folders->arr_val) {
                if (item.type != JsonValue::Type::Object) continue;
                FolderStatus fs;
                if (const auto* f = item.find("id")) fs.id = f->as_string();
                if (const auto* f = item.find("path")) fs.path = f->as_string();
                if (const auto* f = item.find("label")) fs.label = f->as_string();
                if (const auto* f = item.find("enabled")) fs.enabled = f->as_bool(true);
                if (const auto* f = item.find("exists")) fs.exists = f->as_bool(false);
                if (const auto* f = item.find("pending")) fs.pending = f->as_int(0);
                if (const auto* f = item.find("uploading")) fs.uploading = f->as_int(0);
                if (const auto* f = item.find("synced")) fs.synced = f->as_int(0);
                if (const auto* f = item.find("failed")) fs.failed = f->as_int(0);
                if (const auto* f = item.find("pending_bytes")) fs.pending_bytes = f->as_int64(0);
                status.folders.push_back(std::move(fs));
            }
        }
    }

    if (const auto* logs = root.find("logs")) {
        if (logs->type == JsonValue::Type::Array) {
            for (const auto& item : logs->arr_val) {
                if (item.type != JsonValue::Type::Object) continue;
                LogEntry entry;
                if (const auto* l = item.find("id")) entry.id = l->as_int64(0);
                if (const auto* l = item.find("ts")) entry.ts = l->as_int64(0);
                if (const auto* l = item.find("level")) entry.level = l->as_string();
                if (const auto* l = item.find("message")) entry.message = l->as_string();
                status.logs.push_back(std::move(entry));
            }
        }
    }

    return status;
}

std::vector<TailscalePeer> parse_tailscale_peers(const std::string& json_str) {
    std::vector<TailscalePeer> peers;
    JsonValue root;
    if (!parse_json(json_str, root) || root.type != JsonValue::Type::Object) {
        return peers;
    }

    std::string self_id;
    if (const auto* self_obj = root.find("Self")) {
        if (const auto* id_val = self_obj->find("ID")) {
            self_id = id_val->as_string();
        }
    }

    const auto* peer_map = root.find("Peer");
    if (peer_map == nullptr || peer_map->type != JsonValue::Type::Object) {
        return peers;
    }

    for (const auto& kv : peer_map->obj_val) {
        if (kv.second.type != JsonValue::Type::Object) {
            continue;
        }
        const JsonValue& p = kv.second;
        std::string pid = kv.first;
        if (const auto* id_val = p.find("ID")) {
            std::string explicit_id = id_val->as_string();
            if (!explicit_id.empty()) {
                pid = explicit_id;
            }
        }
        if (!self_id.empty() && (pid == self_id || kv.first == self_id)) {
            continue;
        }

        TailscalePeer peer;
        peer.id = pid;
        if (const auto* v = p.find("HostName")) peer.hostname = v->as_string();
        if (const auto* v = p.find("DNSName")) {
            peer.dns_name = v->as_string();
            while (!peer.dns_name.empty() && peer.dns_name.back() == '.') {
                peer.dns_name.pop_back();
            }
        }
        if (const auto* v = p.find("OS")) peer.os = v->as_string();
        if (const auto* v = p.find("Online")) peer.online = v->as_bool(false);
        if (const auto* ips = p.find("TailscaleIPs")) {
            if (ips->type == JsonValue::Type::Array) {
                for (const auto& ip : ips->arr_val) {
                    if (ip.type == JsonValue::Type::String && !ip.str_val.empty()) {
                        peer.tailscale_ips.push_back(ip.str_val);
                    }
                }
            }
        }
        peers.push_back(std::move(peer));
    }

    std::sort(peers.begin(), peers.end(), [](const TailscalePeer& a, const TailscalePeer& b) {
        bool a_mac = is_macos_os(a.os);
        bool b_mac = is_macos_os(b.os);
        if (a_mac != b_mac) {
            return a_mac > b_mac;
        }
        if (a.online != b.online) {
            return a.online > b.online;
        }
        std::string a_name = to_lower_ascii(a.hostname);
        std::string b_name = to_lower_ascii(b.hostname);
        if (a_name != b_name) {
            return a_name < b_name;
        }
        return a.id < b.id;
    });

    return peers;
}

// ---------------------------------------------------------------------------
// IpcClient
// ---------------------------------------------------------------------------

IpcClient::IpcClient(std::string sock_path, std::string db_path, std::string config_path)
    : sock_path_(std::move(sock_path)),
      db_path_(std::move(db_path)),
      config_path_(std::move(config_path)) {}

std::string IpcClient::resolved_sock_path() const {
    return sock_path_.empty() ? default_sock_path() : expand_tilde(sock_path_);
}

std::string IpcClient::resolved_db_path() const {
    return db_path_.empty() ? default_db_path() : expand_tilde(db_path_);
}

std::string IpcClient::resolved_config_path() const {
    return config_path_.empty() ? default_config_path() : expand_tilde(config_path_);
}

std::string IpcClient::send_command(const std::string& json_line, int timeout_ms) const {
    if (timeout_ms <= 0) {
        timeout_ms = 1000;
    }
    std::string path = resolved_sock_path();
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        return "";
    }

    FdGuard fd_guard;
    fd_guard.fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_guard.fd < 0) {
        return "";
    }

#ifdef SO_NOSIGPIPE
    int yes = 1;
    ::setsockopt(fd_guard.fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif

    int flags = ::fcntl(fd_guard.fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd_guard.fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return "";
    }

    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    int conn_rc = ::connect(fd_guard.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (conn_rc < 0) {
        if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
            return "";
        }
        if (!wait_poll(fd_guard.fd, POLLOUT, deadline)) {
            return "";
        }
        int so_err = 0;
        socklen_t len = sizeof(so_err);
        if (::getsockopt(fd_guard.fd, SOL_SOCKET, SO_ERROR, &so_err, &len) != 0 || so_err != 0) {
            return "";
        }
    }

    std::string payload = json_line;
    if (payload.empty() || payload.back() != '\n') {
        payload.push_back('\n');
    }

    size_t sent = 0;
    while (sent < payload.size()) {
        int send_flags = 0;
#ifdef MSG_NOSIGNAL
        send_flags |= MSG_NOSIGNAL;
#endif
        ssize_t n = ::send(fd_guard.fd, payload.data() + sent, payload.size() - sent, send_flags);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_poll(fd_guard.fd, POLLOUT, deadline)) {
                return "";
            }
            continue;
        }
        return "";
    }

    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd_guard.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    if (!response.empty() && response.back() == '\r') {
                        response.pop_back();
                    }
                    return response;
                }
                response.push_back(buf[i]);
            }
            continue;
        }
        if (n == 0) {
            if (!response.empty() && response.back() == '\r') {
                response.pop_back();
            }
            return response;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_poll(fd_guard.fd, POLLIN, deadline)) {
                return "";
            }
            continue;
        }
        return "";
    }
}

DaemonStatus IpcClient::fetch_status() const {
    std::string raw = send_command(R"({"cmd":"status"})", 1000);
    if (!raw.empty()) {
        DaemonStatus parsed = parse_status_json(raw);
        if (parsed.ok || parsed.error != "Invalid status JSON") {
            return parsed;
        }
    }

    DaemonStatus fallback = read_sqlite_fallback();
    if (fallback.ok) {
        return fallback;
    }

    DaemonStatus err;
    err.ok = false;
    err.error = "Daemon unreachable and SQLite fallback unavailable";
    return err;
}

DaemonStatus IpcClient::read_sqlite_fallback() const {
    DaemonStatus status;
    status.from_fallback = true;

    // Populate config fields even if daemon is offline
    std::vector<FolderStatus> configured_folders;
    {
        std::ifstream cfg_in(resolved_config_path());
        if (cfg_in.is_open()) {
            std::ostringstream ss;
            ss << cfg_in.rdbuf();
            JsonValue cfg_root;
            if (parse_json(ss.str(), cfg_root) && cfg_root.type == JsonValue::Type::Object) {
                if (const auto* v = cfg_root.find("target_host")) {
                    status.target_host = v->as_string();
                }
                if (const auto* v = cfg_root.find("target_port")) {
                    int port = v->as_int(8765);
                    if (port >= 1 && port <= 65535) {
                        status.target_port = port;
                    }
                }
                if (const auto* v = cfg_root.find("theme")) {
                    std::string th = v->as_string("dark");
                    if (th == "dark" || th == "light") {
                        status.theme = th;
                    }
                }
                if (const auto* wf = cfg_root.find("watched_folders")) {
                    if (wf->type == JsonValue::Type::Array) {
                        for (const auto& item : wf->arr_val) {
                            if (item.type != JsonValue::Type::Object) continue;
                            FolderStatus fs;
                            if (const auto* f = item.find("id")) fs.id = f->as_string();
                            if (const auto* f = item.find("path")) fs.path = f->as_string();
                            if (const auto* f = item.find("label")) fs.label = f->as_string();
                            if (const auto* f = item.find("enabled")) fs.enabled = f->as_bool(true);
                            if (!fs.id.empty() && !fs.path.empty()) {
                                fs.exists = dir_exists(fs.path);
                                configured_folders.push_back(std::move(fs));
                            }
                        }
                    }
                }
            }
        }
    }

    std::string db_path = resolved_db_path();
    SqliteDbGuard db_guard;
    if (sqlite3_open_v2(db_path.c_str(), &db_guard.db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::string err = db_guard.db != nullptr ? sqlite3_errmsg(db_guard.db) : "Failed to open SQLite DB";
        status.ok = false;
        status.error = err;
        status.last_error = err;
        return status;
    }
    sqlite3_busy_timeout(db_guard.db, 1000);

    auto fail_sqlite = [&](const char* fallback_msg) -> DaemonStatus {
        const char* raw_err = db_guard.db != nullptr ? sqlite3_errmsg(db_guard.db) : nullptr;
        std::string err = (raw_err != nullptr && raw_err[0] != '\0') ? raw_err : fallback_msg;
        status.ok = false;
        status.error = err;
        status.last_error = err;
        return status;
    };

    // 1. Read meta table
    {
        SqliteStmtGuard stmt;
        if (sqlite3_prepare_v2(db_guard.db, "SELECT key, value FROM meta", -1, &stmt.stmt, nullptr) != SQLITE_OK) {
            return fail_sqlite("Failed to prepare SQLite meta query");
        }
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(stmt.stmt)) == SQLITE_ROW) {
            std::string k = col_text(stmt.stmt, 0);
            std::string v = col_text(stmt.stmt, 1);
            if (k == "mac_online") {
                status.mac_online = (v == "true");
            } else if (k == "mac_dest_root") {
                status.mac_dest_root = v;
            } else if (k == "last_sync_ts") {
                status.last_sync_ts = parse_int64_safe(v, 0);
            } else if (k == "last_synced_file") {
                status.last_synced_file = v;
            } else if (k == "last_error") {
                status.last_error = v;
            }
        }
        if (rc != SQLITE_DONE) {
            return fail_sqlite("Failed while reading SQLite meta table");
        }
    }

    // 2. Aggregate files table per folder_id and status
    struct FolderCounts {
        int pending = 0;
        int uploading = 0;
        int synced = 0;
        int failed = 0;
        int64_t pending_bytes = 0;
    };
    std::map<std::string, FolderCounts> by_folder;
    std::vector<std::string> seen_folder_order;

    {
        SqliteStmtGuard stmt;
        const char* sql =
            "SELECT folder_id, status, COUNT(*), COALESCE(SUM(size_bytes), 0) "
            "FROM files GROUP BY folder_id, status";
        if (sqlite3_prepare_v2(db_guard.db, sql, -1, &stmt.stmt, nullptr) != SQLITE_OK) {
            return fail_sqlite("Failed to prepare SQLite files query");
        }
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(stmt.stmt)) == SQLITE_ROW) {
            std::string fid = col_text(stmt.stmt, 0);
            std::string st = col_text(stmt.stmt, 1);
            int cnt = sqlite3_column_int(stmt.stmt, 2);
            int64_t bytes = sqlite3_column_int64(stmt.stmt, 3);

            if (by_folder.find(fid) == by_folder.end()) {
                seen_folder_order.push_back(fid);
            }
            auto& bucket = by_folder[fid];
            if (st == "PENDING") {
                bucket.pending += cnt;
                bucket.pending_bytes += bytes;
                status.pending_count += cnt;
                status.pending_bytes += bytes;
            } else if (st == "UPLOADING") {
                bucket.pending += cnt;
                bucket.uploading += cnt;
                bucket.pending_bytes += bytes;
                status.pending_count += cnt;
                status.pending_bytes += bytes;
            } else if (st == "SYNCED") {
                bucket.synced += cnt;
                status.synced_count += cnt;
            } else if (st == "FAILED") {
                bucket.failed += cnt;
                status.failed_count += cnt;
            }
        }
        if (rc != SQLITE_DONE) {
            return fail_sqlite("Failed while reading SQLite files table");
        }
    }

    std::map<std::string, bool> included;
    for (auto& wf : configured_folders) {
        auto it = by_folder.find(wf.id);
        if (it != by_folder.end()) {
            wf.pending = it->second.pending;
            wf.uploading = it->second.uploading;
            wf.synced = it->second.synced;
            wf.failed = it->second.failed;
            wf.pending_bytes = it->second.pending_bytes;
        }
        included[wf.id] = true;
        status.folders.push_back(std::move(wf));
    }

    for (const auto& fid : seen_folder_order) {
        if (included[fid]) {
            continue;
        }
        const auto& c = by_folder[fid];
        FolderStatus fs;
        fs.id = fid;
        fs.label = fid;
        fs.pending = c.pending;
        fs.uploading = c.uploading;
        fs.synced = c.synced;
        fs.failed = c.failed;
        fs.pending_bytes = c.pending_bytes;
        status.folders.push_back(std::move(fs));
    }

    // 3. Read newest 50 log entries
    {
        SqliteStmtGuard stmt;
        const char* sql = "SELECT id, ts, level, message FROM logs ORDER BY id DESC LIMIT 50";
        if (sqlite3_prepare_v2(db_guard.db, sql, -1, &stmt.stmt, nullptr) != SQLITE_OK) {
            return fail_sqlite("Failed to prepare SQLite logs query");
        }
        int rc = SQLITE_OK;
        while ((rc = sqlite3_step(stmt.stmt)) == SQLITE_ROW) {
            LogEntry entry;
            entry.id = sqlite3_column_int64(stmt.stmt, 0);
            entry.ts = sqlite3_column_int64(stmt.stmt, 1);
            entry.level = col_text(stmt.stmt, 2);
            entry.message = col_text(stmt.stmt, 3);
            status.logs.push_back(std::move(entry));
        }
        if (rc != SQLITE_DONE) {
            return fail_sqlite("Failed while reading SQLite logs table");
        }
    }

    status.ok = true;
    return status;
}

std::vector<TailscalePeer> IpcClient::list_tailscale_peers() const {
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        return {};
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return {};
    }

    if (pid == 0) {
        ::close(pipe_fds[0]);
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::close(pipe_fds[1]);

        const char* argv[] = {"tailscale", "status", "--json", nullptr};
        ::execvp("tailscale", const_cast<char* const*>(argv));
        ::_exit(127);
    }

    ::close(pipe_fds[1]);
    FdGuard read_guard{pipe_fds[0]};

    int flags = ::fcntl(read_guard.fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(read_guard.fd, F_SETFL, flags | O_NONBLOCK);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    std::string output;
    char buf[4096];
    bool timed_out = false;

    while (true) {
        ssize_t n = ::read(read_guard.fd, buf, sizeof(buf));
        if (n > 0) {
            output.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!wait_poll(read_guard.fd, POLLIN, deadline)) {
                timed_out = true;
                break;
            }
            continue;
        }
        break;
    }

    if (timed_out) {
        ::kill(pid, SIGKILL);
    }

    int wstatus = 0;
    while (::waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {
    }

    if (timed_out || !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        return {};
    }

    return parse_tailscale_peers(output);
}

CommandResult IpcClient::run_json_command(const std::string& payload) const {
    CommandResult res;
    res.raw_json = send_command(payload, 1000);
    if (res.raw_json.empty()) {
        res.ok = false;
        res.error = "No response from daemon";
        return res;
    }

    JsonValue root;
    if (!parse_json(res.raw_json, root) || root.type != JsonValue::Type::Object) {
        res.ok = false;
        res.error = "Invalid JSON response";
        return res;
    }

    if (const auto* ok_val = root.find("ok")) {
        res.ok = ok_val->as_bool(false);
    }
    if (const auto* err_val = root.find("error")) {
        res.error = err_val->as_string();
    }
    if (!res.ok && res.error.empty()) {
        res.error = "Command failed";
    }
    return res;
}

CommandResult IpcClient::sync_now() const {
    return run_json_command(R"({"cmd":"sync_now"})");
}

CommandResult IpcClient::add_folder(const std::string& path, const std::string& label) const {
    std::ostringstream oss;
    oss << R"({"cmd":"add_folder","path":)" << json_escape(path);
    if (!label.empty()) {
        oss << R"(,"label":)" << json_escape(label);
    }
    oss << "}";
    return run_json_command(oss.str());
}

CommandResult IpcClient::remove_folder(const std::string& folder_id) const {
    std::ostringstream oss;
    oss << R"({"cmd":"remove_folder","folder_id":)" << json_escape(folder_id) << "}";
    return run_json_command(oss.str());
}

CommandResult IpcClient::set_target(const std::string& host, int port) const {
    std::ostringstream oss;
    oss << R"({"cmd":"set_target","host":)" << json_escape(host) << R"(,"port":)" << port << "}";
    return run_json_command(oss.str());
}

CommandResult IpcClient::set_theme(ThemeMode theme) const {
    std::ostringstream oss;
    oss << R"({"cmd":"set_theme","theme":)" << json_escape(theme_to_string(theme)) << "}";
    return run_json_command(oss.str());
}

CommandResult IpcClient::retry_failed() const {
    return run_json_command(R"({"cmd":"retry_failed"})");
}

} // namespace cp0
