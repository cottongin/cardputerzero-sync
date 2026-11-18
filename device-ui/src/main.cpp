#include "app_ui.hpp"
#include "ipc_client.hpp"
#include "ui_controller.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#if defined(CP0_UI_TARGET_DEVICE) && defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include <SDL.h>
#endif

namespace {

volatile std::sig_atomic_t g_signal_stop = 0;

void handle_signal(int /*sig*/) {
    g_signal_stop = 1;
}

void print_usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "  --sim                  Run desktop 320x170 SDL2 simulator\n"
              << "  --scale <1..4>         Integer window scale factor (default: 1)\n"
              << "  --render-test <path>   Render 320x170 snapshot to PPM file and exit\n"
              << "  --theme <dark|light>   Theme override for --render-test\n"
              << "  --tab <status|folders|logs>  Initial tab for --render-test\n"
              << "  --help                 Show this help message\n";
}

cp0::DaemonStatus make_sample_status(const std::string& theme_str) {
    cp0::DaemonStatus st;
    st.ok = true;
    st.target_host = "macbook-pro.ts.net";
    st.target_port = 8765;
    st.theme = theme_str;
    st.mac_online = true;
    st.mac_dest_root = "/Users/user/Pictures/CardputerZero";
    st.syncing = false;
    st.pending_count = 3;
    st.pending_bytes = 4404019;
    st.synced_count = 148;
    st.failed_count = 0;
    st.last_synced_file = "IMG_0148.JPG";

    cp0::FolderStatus f1;
    f1.id = "camera";
    f1.path = "/home/pi/Pictures/DCIM/Camera";
    f1.label = "Camera";
    f1.exists = true;
    f1.pending = 3;
    f1.synced = 148;
    st.folders.push_back(f1);

    cp0::FolderStatus f2;
    f2.id = "sd";
    f2.path = "/media/pi/SDCARD/DCIM";
    f2.label = "SD Card";
    f2.exists = true;
    f2.pending = 0;
    f2.synced = 64;
    st.folders.push_back(f2);

    cp0::LogEntry l1;
    l1.id = 2;
    l1.ts = 1731900500000LL;
    l1.level = "INFO";
    l1.message = "Synced IMG_0148.JPG (1.4 MB)";
    cp0::LogEntry l2;
    l2.id = 1;
    l2.ts = 1731900490000LL;
    l2.level = "WARN";
    l2.message = "Mac sleeping, retry in 10s";
    st.logs = {l1, l2};
    return st;
}

bool dispatch_action(
    const cp0::IpcClient& ipc,
    cp0::UiController& ctrl,
    cp0::LvglAppView& view,
    const cp0::IpcAction& action,
    bool& running,
    std::chrono::steady_clock::time_point& feedback_until
) {
    using cp0::ActionType;
    if (action.type == ActionType::None) {
        return false;
    }

    auto set_timed_feedback = [&](const std::string& msg) {
        ctrl.set_feedback(msg);
        feedback_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    };

    switch (action.type) {
        case ActionType::Quit:
            running = false;
            return true;
        case ActionType::SyncNow: {
            auto res = ipc.sync_now();
            set_timed_feedback(res.ok ? "Sync triggered" : res.error);
            ctrl.update_status(ipc.fetch_status());
            return true;
        }
        case ActionType::AddFolder: {
            auto res = ipc.add_folder(action.folder_path, action.folder_label);
            set_timed_feedback(res.ok ? "Folder added" : res.error);
            ctrl.update_status(ipc.fetch_status());
            return true;
        }
        case ActionType::RemoveFolder: {
            auto res = ipc.remove_folder(action.folder_id);
            set_timed_feedback(res.ok ? "Folder removed" : res.error);
            ctrl.update_status(ipc.fetch_status());
            return true;
        }
        case ActionType::SetTarget: {
            auto res = ipc.set_target(action.target_host, action.target_port);
            set_timed_feedback(res.ok ? ("Target: " + action.target_host) : res.error);
            ctrl.update_status(ipc.fetch_status());
            return true;
        }
        case ActionType::SetTheme: {
            view.apply_theme(action.theme);
            ipc.set_theme(action.theme);
            return true;
        }
        case ActionType::RetryFailed: {
            auto res = ipc.retry_failed();
            set_timed_feedback(res.ok ? "Retrying failed files" : res.error);
            ctrl.update_status(ipc.fetch_status());
            return true;
        }
        case ActionType::None:
            break;
    }
    return false;
}

#if defined(CP0_UI_TARGET_DEVICE) && defined(__linux__)

char map_evdev_code(uint16_t code, bool shift) {
    switch (code) {
        case KEY_1: return shift ? '!' : '1';
        case KEY_2: return shift ? '@' : '2';
        case KEY_3: return shift ? '#' : '3';
        case KEY_4: return shift ? '$' : '4';
        case KEY_5: return shift ? '%' : '5';
        case KEY_6: return shift ? '^' : '6';
        case KEY_7: return shift ? '&' : '7';
        case KEY_8: return shift ? '*' : '8';
        case KEY_9: return shift ? '(' : '9';
        case KEY_0: return shift ? ')' : '0';
        case KEY_A: return shift ? 'A' : 'a';
        case KEY_B: return shift ? 'B' : 'b';
        case KEY_C: return shift ? 'C' : 'c';
        case KEY_D: return shift ? 'D' : 'd';
        case KEY_E: return shift ? 'E' : 'e';
        case KEY_F: return shift ? 'F' : 'f';
        case KEY_G: return shift ? 'G' : 'g';
        case KEY_H: return shift ? 'H' : 'h';
        case KEY_I: return shift ? 'I' : 'i';
        case KEY_J: return shift ? 'J' : 'j';
        case KEY_K: return shift ? 'K' : 'k';
        case KEY_L: return shift ? 'L' : 'l';
        case KEY_M: return shift ? 'M' : 'm';
        case KEY_N: return shift ? 'N' : 'n';
        case KEY_O: return shift ? 'O' : 'o';
        case KEY_P: return shift ? 'P' : 'p';
        case KEY_Q: return shift ? 'Q' : 'q';
        case KEY_R: return shift ? 'R' : 'r';
        case KEY_S: return shift ? 'S' : 's';
        case KEY_T: return shift ? 'T' : 't';
        case KEY_U: return shift ? 'U' : 'u';
        case KEY_V: return shift ? 'V' : 'v';
        case KEY_W: return shift ? 'W' : 'w';
        case KEY_X: return shift ? 'X' : 'x';
        case KEY_Y: return shift ? 'Y' : 'y';
        case KEY_Z: return shift ? 'Z' : 'z';
        case KEY_MINUS: return shift ? '_' : '-';
        case KEY_EQUAL: return shift ? '+' : '=';
        case KEY_LEFTBRACE: return shift ? '{' : '[';
        case KEY_RIGHTBRACE: return shift ? '}' : ']';
        case KEY_SEMICOLON: return shift ? ':' : ';';
        case KEY_APOSTROPHE: return shift ? '"' : '\'';
        case KEY_COMMA: return shift ? '<' : ',';
        case KEY_DOT: return shift ? '>' : '.';
        case KEY_SLASH: return shift ? '?' : '/';
        case KEY_BACKSLASH: return shift ? '|' : '\\';
        default: return 0;
    }
}

void blit_fbdev(
    uint8_t* fb_mem,
    const fb_var_screeninfo& vinfo,
    const fb_fix_screeninfo& finfo,
    const uint32_t* pixels
) {
    int fb_w = static_cast<int>(vinfo.xres);
    int fb_h = static_cast<int>(vinfo.yres);
    int copy_w = std::min(fb_w, cp0::LvglAppView::kWidth);
    int copy_h = std::min(fb_h, cp0::LvglAppView::kHeight);
    int bpp = static_cast<int>(vinfo.bits_per_pixel);
    int stride = static_cast<int>(finfo.line_length);

    for (int y = 0; y < copy_h; ++y) {
        uint8_t* row = fb_mem + (y + static_cast<int>(vinfo.yoffset)) * stride +
                       static_cast<int>(vinfo.xoffset) * (bpp / 8);
        const uint32_t* src = pixels + y * cp0::LvglAppView::kWidth;
        if (bpp == 32) {
            std::memcpy(row, src, static_cast<size_t>(copy_w) * 4);
        } else if (bpp == 16) {
            auto* dst16 = reinterpret_cast<uint16_t*>(row);
            for (int x = 0; x < copy_w; ++x) {
                uint32_t c = src[x];
                uint16_t r = static_cast<uint16_t>((c >> 19) & 0x1fU);
                uint16_t g = static_cast<uint16_t>((c >> 10) & 0x3fU);
                uint16_t b = static_cast<uint16_t>((c >> 3) & 0x1fU);
                dst16[x] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
            }
        }
    }
}

std::vector<int> open_keyboard_evdevs() {
    std::vector<int> fds;
    if (const char* override_dev = std::getenv("CP0_EVDEV_DEVICE")) {
        int fd = ::open(override_dev, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            fds.push_back(fd);
        }
        return fds;
    }

    DIR* dir = ::opendir("/dev/input");
    if (dir == nullptr) {
        return fds;
    }
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(dir)) != nullptr) {
        if (std::strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        std::string dev_path = std::string("/dev/input/") + ent->d_name;
        int fd = ::open(dev_path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }
        unsigned long ev_bits[4]{};
        if (::ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) >= 0 &&
            (ev_bits[0] & (1UL << EV_KEY)) != 0) {
            fds.push_back(fd);
        } else {
            ::close(fd);
        }
    }
    ::closedir(dir);
    return fds;
}

int run_device_loop(cp0::IpcClient& ipc, cp0::UiController& ctrl, cp0::LvglAppView& view) {
    const char* fb_dev = std::getenv("CP0_FB_DEVICE");
    if (fb_dev == nullptr || fb_dev[0] == '\0') {
        fb_dev = "/dev/fb0";
    }
    int fb_fd = ::open(fb_dev, O_RDWR);
    if (fb_fd < 0) {
        std::cerr << "Failed to open framebuffer " << fb_dev << "\n";
        return 1;
    }

    fb_var_screeninfo vinfo{};
    fb_fix_screeninfo finfo{};
    if (::ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ::ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        std::cerr << "Failed to query framebuffer info\n";
        ::close(fb_fd);
        return 1;
    }

    size_t map_size = finfo.smem_len;
    void* mapped = ::mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "Failed to mmap framebuffer\n";
        ::close(fb_fd);
        return 1;
    }
    auto* fb_mem = static_cast<uint8_t*>(mapped);

    std::vector<int> ev_fds = open_keyboard_evdevs();
    std::vector<pollfd> pfds;
    pfds.reserve(ev_fds.size());
    for (int fd : ev_fds) {
        pfds.push_back(pollfd{fd, POLLIN, 0});
    }

    ctrl.update_status(ipc.fetch_status());
    view.apply_theme(ctrl.state().theme);
    view.render(ctrl.state());
    blit_fbdev(fb_mem, vinfo, finfo, view.pixels());

    bool running = true;
    bool shift_down = false;
    auto last_poll = std::chrono::steady_clock::now();
    auto feedback_until = std::chrono::steady_clock::time_point{};

    while (running && !g_signal_stop) {
        bool dirty = false;
        if (!pfds.empty()) {
            int prc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 50);
            if (prc > 0) {
                for (const auto& p : pfds) {
                    if ((p.revents & POLLIN) == 0) {
                        continue;
                    }
                    struct input_event ev{};
                    while (::read(p.fd, &ev, sizeof(ev)) == sizeof(ev)) {
                        if (ev.type != EV_KEY) {
                            continue;
                        }
                        if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                            shift_down = (ev.value != 0);
                            continue;
                        }
                        if (ev.value != 1 && ev.value != 2) {
                            continue;
                        }
                        cp0::IpcAction act;
                        bool handled = true;
                        switch (ev.code) {
                            case KEY_UP:
                                act = ctrl.handle_key(0, cp0::KeyCode::Up);
                                break;
                            case KEY_DOWN:
                                act = ctrl.handle_key(0, cp0::KeyCode::Down);
                                break;
                            case KEY_LEFT:
                                act = ctrl.handle_key(0, cp0::KeyCode::Left);
                                break;
                            case KEY_RIGHT:
                                act = ctrl.handle_key(0, cp0::KeyCode::Right);
                                break;
                            case KEY_ENTER:
                            case KEY_KPENTER:
                                act = ctrl.handle_key(0, cp0::KeyCode::Enter);
                                break;
                            case KEY_ESC:
                                act = ctrl.handle_key(0, cp0::KeyCode::Escape);
                                break;
                            case KEY_BACKSPACE:
                                act = ctrl.handle_key(0, cp0::KeyCode::Backspace);
                                break;
                            case KEY_DELETE:
                                act = ctrl.handle_key(0, cp0::KeyCode::Delete);
                                break;
                            case KEY_SPACE:
                                act = ctrl.handle_key(' ', cp0::KeyCode::Space);
                                break;
                            default: {
                                char c = map_evdev_code(ev.code, shift_down);
                                if (c != 0) {
                                    act = ctrl.handle_key(c, cp0::KeyCode::Char);
                                } else {
                                    handled = false;
                                }
                                break;
                            }
                        }
                        if (handled) {
                            dirty = true;
                            if (ctrl.consume_peer_refresh()) {
                                ctrl.set_peers(ipc.list_tailscale_peers());
                            }
                            dispatch_action(ipc, ctrl, view, act, running, feedback_until);
                        }
                    }
                }
            }
        } else {
            ::usleep(50000);
        }

        auto now = std::chrono::steady_clock::now();
        if (!ctrl.state().feedback_banner.empty() && now >= feedback_until) {
            ctrl.clear_feedback();
            dirty = true;
        }
        if (now - last_poll >= std::chrono::milliseconds(1000)) {
            ctrl.update_status(ipc.fetch_status());
            last_poll = now;
            dirty = true;
        }
        if (dirty) {
            view.render(ctrl.state());
            blit_fbdev(fb_mem, vinfo, finfo, view.pixels());
        }
    }

    for (int fd : ev_fds) {
        ::close(fd);
    }
    ::munmap(mapped, map_size);
    ::close(fb_fd);
    return 0;
}

#else

int run_sdl_loop(
    cp0::IpcClient& ipc,
    cp0::UiController& ctrl,
    cp0::LvglAppView& view,
    int scale
) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "CardputerZero Photo Sync",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        cp0::LvglAppView::kWidth * scale,
        cp0::LvglAppView::kHeight * scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (window == nullptr) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (renderer == nullptr) {
        renderer = SDL_CreateRenderer(window, -1, 0);
    }
    if (renderer == nullptr) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_RenderSetLogicalSize(renderer, cp0::LvglAppView::kWidth, cp0::LvglAppView::kHeight);
    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        cp0::LvglAppView::kWidth,
        cp0::LvglAppView::kHeight
    );
    if (texture == nullptr) {
        std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << "\n";
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_StartTextInput();

    ctrl.update_status(ipc.fetch_status());
    view.apply_theme(ctrl.state().theme);
    view.render(ctrl.state());

    auto present = [&]() {
        SDL_UpdateTexture(
            texture,
            nullptr,
            view.pixels(),
            cp0::LvglAppView::kWidth * static_cast<int>(sizeof(uint32_t))
        );
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    };
    present();

    bool running = true;
    auto last_poll = std::chrono::steady_clock::now();
    auto feedback_until = std::chrono::steady_clock::time_point{};

    while (running && !g_signal_stop) {
        bool dirty = false;
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 50) != 0) {
            do {
                if (ev.type == SDL_QUIT) {
                    running = false;
                    break;
                }
                if (ev.type == SDL_WINDOWEVENT &&
                    (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
                     ev.window.event == SDL_WINDOWEVENT_RESIZED)) {
                    dirty = true;
                    continue;
                }
                if (ev.type == SDL_TEXTINPUT) {
                    for (int i = 0; ev.text.text[i] != '\0'; ++i) {
                        char ch = ev.text.text[i];
                        cp0::IpcAction act = ctrl.handle_key(ch, cp0::KeyCode::Char);
                        dirty = true;
                        if (ctrl.consume_peer_refresh()) {
                            ctrl.set_peers(ipc.list_tailscale_peers());
                        }
                        dispatch_action(ipc, ctrl, view, act, running, feedback_until);
                    }
                    continue;
                }
                if (ev.type == SDL_KEYDOWN) {
                    SDL_Keycode sym = ev.key.keysym.sym;
                    uint16_t mod = ev.key.keysym.mod;
                    if ((mod & (KMOD_CTRL | KMOD_GUI)) != 0 && sym == SDLK_q) {
                        running = false;
                        break;
                    }
                    cp0::IpcAction act;
                    bool handled = true;
                    switch (sym) {
                        case SDLK_UP:
                            act = ctrl.handle_key(0, cp0::KeyCode::Up);
                            break;
                        case SDLK_DOWN:
                            act = ctrl.handle_key(0, cp0::KeyCode::Down);
                            break;
                        case SDLK_LEFT:
                            act = ctrl.handle_key(0, cp0::KeyCode::Left);
                            break;
                        case SDLK_RIGHT:
                            act = ctrl.handle_key(0, cp0::KeyCode::Right);
                            break;
                        case SDLK_RETURN:
                        case SDLK_KP_ENTER:
                            act = ctrl.handle_key(0, cp0::KeyCode::Enter);
                            break;
                        case SDLK_ESCAPE:
                            act = ctrl.handle_key(0, cp0::KeyCode::Escape);
                            break;
                        case SDLK_BACKSPACE:
                            act = ctrl.handle_key(0, cp0::KeyCode::Backspace);
                            break;
                        case SDLK_DELETE:
                            act = ctrl.handle_key(0, cp0::KeyCode::Delete);
                            break;
                        default:
                            handled = false;
                            break;
                    }
                    if (handled) {
                        dirty = true;
                        if (ctrl.consume_peer_refresh()) {
                            ctrl.set_peers(ipc.list_tailscale_peers());
                        }
                        dispatch_action(ipc, ctrl, view, act, running, feedback_until);
                    }
                }
            } while (running && SDL_PollEvent(&ev) != 0);
        }

        auto now = std::chrono::steady_clock::now();
        if (!ctrl.state().feedback_banner.empty() && now >= feedback_until) {
            ctrl.clear_feedback();
            dirty = true;
        }
        if (now - last_poll >= std::chrono::milliseconds(1000)) {
            ctrl.update_status(ipc.fetch_status());
            last_poll = now;
            dirty = true;
        }
        if (running && dirty) {
            view.render(ctrl.state());
            present();
        }
    }

    SDL_StopTextInput();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#endif

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    int scale = 1;
    std::string render_test_path;
    std::string render_theme = "dark";
    std::string render_tab = "status";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--sim") {
            continue;
        }
        if (arg == "--scale" && i + 1 < argc) {
            int parsed = std::atoi(argv[++i]);
            if (parsed >= 1 && parsed <= 4) {
                scale = parsed;
            }
            continue;
        }
        if (arg == "--render-test" && i + 1 < argc) {
            render_test_path = argv[++i];
            continue;
        }
        if (arg == "--theme" && i + 1 < argc) {
            render_theme = argv[++i];
            continue;
        }
        if (arg == "--tab" && i + 1 < argc) {
            render_tab = argv[++i];
            continue;
        }
        std::cerr << "Unknown option: " << arg << "\n";
        print_usage(argv[0]);
        return 2;
    }

    cp0::UiController ctrl;
    cp0::LvglAppView view;

    if (!render_test_path.empty()) {
        ctrl.update_status(make_sample_status(render_theme));
        if (render_tab == "folders") {
            ctrl.handle_key('2', cp0::KeyCode::Char);
        } else if (render_tab == "logs") {
            ctrl.handle_key('3', cp0::KeyCode::Char);
        }
        view.apply_theme(ctrl.state().theme);
        view.render(ctrl.state());
        if (!view.save_ppm(render_test_path)) {
            std::cerr << "Failed to write PPM to " << render_test_path << "\n";
            return 1;
        }
        return 0;
    }

    cp0::IpcClient ipc;
#if defined(CP0_UI_TARGET_DEVICE) && defined(__linux__)
    (void)scale;
    return run_device_loop(ipc, ctrl, view);
#else
    return run_sdl_loop(ipc, ctrl, view, scale);
#endif
}
