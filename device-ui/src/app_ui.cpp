#include "app_ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(CP0_HAS_LVGL)
#include <lvgl.h>
#endif

#if defined(CP0_HAS_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace cp0 {

namespace {

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

std::string format_bytes(int64_t bytes) {
    if (bytes < 0) {
        bytes = 0;
    }
    std::ostringstream oss;
    if (bytes < 1024) {
        oss << bytes << " B";
    } else if (bytes < 1024LL * 1024) {
        oss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / 1024.0) << " KB";
    } else if (bytes < 1024LL * 1024 * 1024) {
        oss << std::fixed << std::setprecision(1)
            << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    } else {
        oss << std::fixed << std::setprecision(2)
            << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GB";
    }
    return oss.str();
}

std::string format_hhmmss(int64_t ts_ms) {
    if (ts_ms <= 0) {
        return "--:--:--";
    }
    std::time_t secs = static_cast<std::time_t>(ts_ms / 1000);
    std::tm tm_val{};
    if (::localtime_r(&secs, &tm_val) == nullptr) {
        return "--:--:--";
    }
    char buf[16];
    std::snprintf(
        buf,
        sizeof(buf),
        "%02d:%02d:%02d",
        tm_val.tm_hour,
        tm_val.tm_min,
        tm_val.tm_sec
    );
    return std::string(buf);
}

std::string short_log_tag(const std::string& level) {
    if (level == "ERROR" || level == "ERR") {
        return "ER";
    }
    if (level == "WARN" || level == "WARNING") {
        return "WN";
    }
    if (level == "OK") {
        return "OK";
    }
    return "IN";
}

// ---------------------------------------------------------------------------
// Built-in 5x7 ASCII bitmap font (0x20..0x7E) fallback
// ---------------------------------------------------------------------------

// Each glyph is 5 columns x 7 rows, packed as 5 bytes where bit 0 = top row, bit 6 = bottom row.
constexpr std::array<std::array<uint8_t, 5>, 95> kFont5x7 = {{
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x00, 0x00, 0x5f, 0x00, 0x00}, // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00}, // '"'
    {0x14, 0x7f, 0x14, 0x7f, 0x14}, // '#'
    {0x24, 0x2a, 0x7f, 0x2a, 0x12}, // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50}, // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00}, // '\''
    {0x00, 0x1c, 0x22, 0x41, 0x00}, // '('
    {0x00, 0x41, 0x22, 0x1c, 0x00}, // ')'
    {0x14, 0x08, 0x3e, 0x08, 0x14}, // '*'
    {0x08, 0x08, 0x3e, 0x08, 0x08}, // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00}, // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02}, // '/'
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, // '0'
    {0x00, 0x42, 0x7f, 0x40, 0x00}, // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46}, // '2'
    {0x21, 0x41, 0x45, 0x4b, 0x31}, // '3'
    {0x18, 0x14, 0x12, 0x7f, 0x10}, // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // '5'
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03}, // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1e}, // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00}, // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ';'
    {0x08, 0x14, 0x22, 0x41, 0x00}, // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // '='
    {0x00, 0x41, 0x22, 0x14, 0x08}, // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06}, // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3e}, // '@'
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, // 'A'
    {0x7f, 0x49, 0x49, 0x49, 0x36}, // 'B'
    {0x3e, 0x41, 0x41, 0x41, 0x22}, // 'C'
    {0x7f, 0x41, 0x41, 0x22, 0x1c}, // 'D'
    {0x7f, 0x49, 0x49, 0x49, 0x41}, // 'E'
    {0x7f, 0x09, 0x09, 0x09, 0x01}, // 'F'
    {0x3e, 0x41, 0x49, 0x49, 0x7a}, // 'G'
    {0x7f, 0x08, 0x08, 0x08, 0x7f}, // 'H'
    {0x00, 0x41, 0x7f, 0x41, 0x00}, // 'I'
    {0x20, 0x40, 0x41, 0x3f, 0x01}, // 'J'
    {0x7f, 0x08, 0x14, 0x22, 0x41}, // 'K'
    {0x7f, 0x40, 0x40, 0x40, 0x40}, // 'L'
    {0x7f, 0x02, 0x0c, 0x02, 0x7f}, // 'M'
    {0x7f, 0x04, 0x08, 0x10, 0x7f}, // 'N'
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, // 'O'
    {0x7f, 0x09, 0x09, 0x09, 0x06}, // 'P'
    {0x3e, 0x41, 0x51, 0x21, 0x5e}, // 'Q'
    {0x7f, 0x09, 0x19, 0x29, 0x46}, // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 'S'
    {0x01, 0x01, 0x7f, 0x01, 0x01}, // 'T'
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, // 'U'
    {0x1f, 0x20, 0x40, 0x20, 0x1f}, // 'V'
    {0x3f, 0x40, 0x38, 0x40, 0x3f}, // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07}, // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 'Z'
    {0x00, 0x7f, 0x41, 0x41, 0x00}, // '['
    {0x02, 0x04, 0x08, 0x10, 0x20}, // '\'
    {0x00, 0x41, 0x41, 0x7f, 0x00}, // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04}, // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40}, // '_'
    {0x00, 0x01, 0x02, 0x04, 0x00}, // '`'
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 'a'
    {0x7f, 0x48, 0x44, 0x44, 0x38}, // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20}, // 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7f}, // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 'e'
    {0x08, 0x7e, 0x09, 0x01, 0x02}, // 'f'
    {0x0c, 0x52, 0x52, 0x52, 0x3e}, // 'g'
    {0x7f, 0x08, 0x04, 0x04, 0x78}, // 'h'
    {0x00, 0x44, 0x7d, 0x40, 0x00}, // 'i'
    {0x20, 0x40, 0x44, 0x3d, 0x00}, // 'j'
    {0x7f, 0x10, 0x28, 0x44, 0x00}, // 'k'
    {0x00, 0x41, 0x7f, 0x40, 0x00}, // 'l'
    {0x7c, 0x04, 0x18, 0x04, 0x78}, // 'm'
    {0x7c, 0x08, 0x04, 0x04, 0x78}, // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 'o'
    {0x7c, 0x14, 0x14, 0x14, 0x08}, // 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7c}, // 'q'
    {0x7c, 0x08, 0x04, 0x04, 0x08}, // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20}, // 's'
    {0x04, 0x3f, 0x44, 0x40, 0x20}, // 't'
    {0x3c, 0x40, 0x40, 0x20, 0x7c}, // 'u'
    {0x1c, 0x20, 0x40, 0x20, 0x1c}, // 'v'
    {0x3c, 0x40, 0x30, 0x40, 0x3c}, // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 'x'
    {0x0c, 0x50, 0x50, 0x50, 0x3c}, // 'y'
    {0x44, 0x64, 0x54, 0x4c, 0x44}, // 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00}, // '{'
    {0x00, 0x00, 0x7f, 0x00, 0x00}, // '|'
    {0x00, 0x41, 0x36, 0x08, 0x00}, // '}'
    {0x08, 0x04, 0x08, 0x10, 0x08}, // '~'
}};

enum class FontRole {
    Sans,
    Mono,
};

#if defined(CP0_HAS_FREETYPE)
struct CachedGlyph {
    int width = 0;
    int rows = 0;
    int left = 0;
    int top = 0;
    int advance_x = 0;
    std::vector<uint8_t> alpha;
};
#endif

// ---------------------------------------------------------------------------
// 320x170 Anti-Aliased Software Framebuffer + FreeType2 / Fallback Renderer
// ---------------------------------------------------------------------------

class ShadcnCanvas {
public:
    static constexpr int kW = LvglAppView::kWidth;
    static constexpr int kH = LvglAppView::kHeight;

    ShadcnCanvas() : buf_(static_cast<size_t>(kW * kH), 0xff09090bU) {
        init_fonts();
    }

    ~ShadcnCanvas() {
#if defined(CP0_HAS_FREETYPE)
        if (sans_face_ != nullptr) {
            FT_Done_Face(sans_face_);
            sans_face_ = nullptr;
        }
        if (mono_face_ != nullptr) {
            FT_Done_Face(mono_face_);
            mono_face_ = nullptr;
        }
        if (ft_lib_ != nullptr) {
            FT_Done_FreeType(ft_lib_);
            ft_lib_ = nullptr;
        }
#endif
    }

    bool using_freetype() const { return has_ft_sans_ || has_ft_mono_; }

    const uint32_t* pixels() const { return buf_.data(); }

    void clear(uint32_t rgb) {
        uint32_t argb = 0xff000000U | (rgb & 0x00ffffffU);
        std::fill(buf_.begin(), buf_.end(), argb);
    }

    void dim_rect(int x, int y, int w, int h, uint32_t rgb, uint8_t alpha) {
        int x0 = std::max(0, x);
        int y0 = std::max(0, y);
        int x1 = std::min(kW, x + w);
        int y1 = std::min(kH, y + h);
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                blend_pixel(px, py, rgb, alpha);
            }
        }
    }

    void fill_rect(int x, int y, int w, int h, uint32_t rgb) {
        int x0 = std::max(0, x);
        int y0 = std::max(0, y);
        int x1 = std::min(kW, x + w);
        int y1 = std::min(kH, y + h);
        uint32_t argb = 0xff000000U | (rgb & 0x00ffffffU);
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                buf_[static_cast<size_t>(py * kW + px)] = argb;
            }
        }
    }

    void draw_round_box(
        int x,
        int y,
        int w,
        int h,
        int radius,
        uint32_t fill_rgb,
        uint32_t border_rgb,
        int border_w
    ) {
        if (w <= 0 || h <= 0) {
            return;
        }
        float r = static_cast<float>(std::max(0, std::min(radius, std::min(w, h) / 2)));
        float hx = static_cast<float>(w) * 0.5f;
        float hy = static_cast<float>(h) * 0.5f;
        float cx = static_cast<float>(x) + hx;
        float cy = static_cast<float>(y) + hy;
        float bw = static_cast<float>(std::max(0, border_w));

        int x0 = std::max(0, x);
        int y0 = std::max(0, y);
        int x1 = std::min(kW, x + w);
        int y1 = std::min(kH, y + h);

        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                float dx = std::abs((static_cast<float>(px) + 0.5f) - cx) - (hx - r);
                float dy = std::abs((static_cast<float>(py) + 0.5f) - cy) - (hy - r);
                float ox = std::max(dx, 0.0f);
                float oy = std::max(dy, 0.0f);
                float dist = std::hypot(ox, oy) + std::min(std::max(dx, dy), 0.0f) - r;

                float outer = std::clamp(0.5f - dist, 0.0f, 1.0f);
                if (outer <= 0.001f) {
                    continue;
                }
                if (bw <= 0.0f) {
                    uint8_t a = static_cast<uint8_t>(std::lround(outer * 255.0f));
                    blend_pixel(px, py, fill_rgb, a);
                    continue;
                }
                float inner = std::clamp(0.5f - (dist + bw), 0.0f, 1.0f);
                float edge = std::max(0.0f, outer - inner);
                if (inner > 0.001f) {
                    uint8_t ia = static_cast<uint8_t>(std::lround(inner * 255.0f));
                    blend_pixel(px, py, fill_rgb, ia);
                }
                if (edge > 0.001f) {
                    uint8_t ba = static_cast<uint8_t>(std::lround(edge * 255.0f));
                    blend_pixel(px, py, border_rgb, ba);
                }
            }
        }
    }

    void draw_circle(float cx, float cy, float radius, uint32_t rgb, bool hollow = false) {
        int x0 = std::max(0, static_cast<int>(std::floor(cx - radius - 1.0f)));
        int y0 = std::max(0, static_cast<int>(std::floor(cy - radius - 1.0f)));
        int x1 = std::min(kW, static_cast<int>(std::ceil(cx + radius + 1.0f)));
        int y1 = std::min(kH, static_cast<int>(std::ceil(cy + radius + 1.0f)));
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                float d = std::hypot(
                    (static_cast<float>(px) + 0.5f) - cx,
                    (static_cast<float>(py) + 0.5f) - cy
                );
                float alpha_f = 0.0f;
                if (hollow) {
                    float ring_d = std::abs(d - radius);
                    alpha_f = std::clamp(0.75f - ring_d, 0.0f, 1.0f);
                } else {
                    alpha_f = std::clamp(radius + 0.5f - d, 0.0f, 1.0f);
                }
                if (alpha_f > 0.01f) {
                    blend_pixel(px, py, rgb, static_cast<uint8_t>(std::lround(alpha_f * 255.0f)));
                }
            }
        }
    }

    void draw_checkmark(int x, int y, uint32_t rgb) {
        // Crisp 7x6 checkmark stroke
        static constexpr std::array<std::pair<int, int>, 7> pts = {{
            {0, 3}, {1, 4}, {2, 5}, {3, 4}, {4, 3}, {5, 2}, {6, 1}
        }};
        for (const auto& pt : pts) {
            blend_pixel(x + pt.first, y + pt.second, rgb, 255);
            blend_pixel(x + pt.first, y + pt.second + 1, rgb, 170);
        }
    }

    int measure_text(FontRole role, int px_size, const std::string& text) {
#if defined(CP0_HAS_FREETYPE)
        FT_Face face = select_face(role);
        if (face != nullptr) {
            int width = 0;
            for (unsigned char ch : text) {
                if (ch < 0x20 || ch > 0x7e) {
                    ch = '?';
                }
                const auto* g = get_glyph(role, face, px_size, ch);
                width += (g != nullptr) ? g->advance_x : (px_size / 2);
            }
            return width;
        }
#else
        (void)role;
#endif
        int scale = (px_size >= 14) ? 2 : 1;
        return static_cast<int>(text.size()) * 6 * scale;
    }

    int line_height(FontRole role, int px_size) {
#if defined(CP0_HAS_FREETYPE)
        FT_Face face = select_face(role);
        if (face != nullptr) {
            FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(px_size));
            int h = static_cast<int>(face->size->metrics.height >> 6);
            return std::max(px_size, h);
        }
#else
        (void)role;
#endif
        int scale = (px_size >= 14) ? 2 : 1;
        return 8 * scale;
    }

    std::string truncate_right(FontRole role, int px_size, const std::string& s, int max_w) {
        if (max_w <= 0) {
            return "";
        }
        if (measure_text(role, px_size, s) <= max_w) {
            return s;
        }
        const std::string ell = "...";
        if (measure_text(role, px_size, ell) > max_w) {
            return "";
        }
        std::string cur = s;
        while (!cur.empty() && measure_text(role, px_size, cur + ell) > max_w) {
            cur.pop_back();
        }
        return cur + ell;
    }

    std::string truncate_left_path(FontRole role, int px_size, const std::string& s, int max_w) {
        if (max_w <= 0) {
            return "";
        }
        if (measure_text(role, px_size, s) <= max_w) {
            return s;
        }
        const std::string ell = "...";
        int ell_w = measure_text(role, px_size, ell);
        if (ell_w > max_w) {
            return "";
        }
        size_t pos = 0;
        while (pos < s.size() && measure_text(role, px_size, ell + s.substr(pos)) > max_w) {
            ++pos;
        }
        return ell + s.substr(pos);
    }

    void draw_text(
        FontRole role,
        int px_size,
        int x,
        int y,
        const std::string& text,
        uint32_t rgb
    ) {
#if defined(CP0_HAS_FREETYPE)
        FT_Face face = select_face(role);
        if (face != nullptr) {
            FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(px_size));
            int ascender = static_cast<int>(face->size->metrics.ascender >> 6);
            int baseline_y = y + ascender;
            int pen_x = x;
            for (unsigned char ch : text) {
                if (ch < 0x20 || ch > 0x7e) {
                    ch = '?';
                }
                const auto* g = get_glyph(role, face, px_size, ch);
                if (g == nullptr) {
                    pen_x += px_size / 2;
                    continue;
                }
                int gx = pen_x + g->left;
                int gy = baseline_y - g->top;
                for (int r = 0; r < g->rows; ++r) {
                    for (int c = 0; c < g->width; ++c) {
                        uint8_t cov = g->alpha[static_cast<size_t>(r * g->width + c)];
                        if (cov > 0) {
                            blend_pixel(gx + c, gy + r, rgb, cov);
                        }
                    }
                }
                pen_x += g->advance_x;
            }
            return;
        }
#else
        (void)role;
#endif
        int scale = (px_size >= 14) ? 2 : 1;
        int pen_x = x;
        for (unsigned char ch : text) {
            if (ch < 0x20 || ch > 0x7e) {
                ch = '?';
            }
            const auto& glyph = kFont5x7[static_cast<size_t>(ch - 0x20)];
            for (int col = 0; col < 5; ++col) {
                uint8_t bits = glyph[static_cast<size_t>(col)];
                for (int row = 0; row < 7; ++row) {
                    if ((bits & (1U << row)) != 0) {
                        fill_rect(pen_x + col * scale, y + row * scale, scale, scale, rgb);
                    }
                }
            }
            pen_x += 6 * scale;
        }
    }

private:
    void init_fonts() {
#if defined(CP0_HAS_FREETYPE)
        if (FT_Init_FreeType(&ft_lib_) != 0) {
            ft_lib_ = nullptr;
            return;
        }

        auto try_load_list = [&](const std::vector<std::string>& paths, FT_Face& out_face) -> bool {
            for (const auto& p : paths) {
                if (p.empty()) {
                    continue;
                }
                if (FT_New_Face(ft_lib_, p.c_str(), 0, &out_face) == 0) {
                    return true;
                }
            }
            return false;
        };

        std::vector<std::string> sans_candidates;
        if (const char* env_sans = std::getenv("CP0_FONT_SANS")) {
            sans_candidates.emplace_back(env_sans);
        } else {
            sans_candidates = {
                "/usr/share/cp0-sync/fonts/Inter-Regular.ttf",
                "/usr/share/cp0-sync/fonts/Inter.ttf",
                "assets/fonts/Inter-Regular.ttf",
                "assets/fonts/Inter.ttf",
                "/usr/share/fonts/truetype/inter/Inter-Regular.ttf",
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                "/System/Library/Fonts/SFNS.ttf",
                "/System/Library/Fonts/Geneva.ttf",
                "/System/Library/Fonts/Helvetica.ttc",
            };
        }

        std::vector<std::string> mono_candidates;
        if (const char* env_mono = std::getenv("CP0_FONT_MONO")) {
            mono_candidates.emplace_back(env_mono);
        } else {
            mono_candidates = {
                "/usr/share/cp0-sync/fonts/JetBrainsMono-Regular.ttf",
                "/usr/share/cp0-sync/fonts/JetBrainsMono.ttf",
                "assets/fonts/JetBrainsMono-Regular.ttf",
                "assets/fonts/JetBrainsMono.ttf",
                "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
                "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
                "/System/Library/Fonts/SFNSMono.ttf",
                "/System/Library/Fonts/Monaco.ttf",
                "/System/Library/Fonts/Menlo.ttc",
            };
        }

        has_ft_sans_ = try_load_list(sans_candidates, sans_face_);
        has_ft_mono_ = try_load_list(mono_candidates, mono_face_);
#endif
    }

#if defined(CP0_HAS_FREETYPE)
    FT_Face select_face(FontRole role) const {
        if (role == FontRole::Mono) {
            return mono_face_ != nullptr ? mono_face_ : sans_face_;
        }
        return sans_face_ != nullptr ? sans_face_ : mono_face_;
    }

    const CachedGlyph* get_glyph(FontRole role, FT_Face face, int px_size, uint32_t codepoint) {
        uint64_t key = (static_cast<uint64_t>(role == FontRole::Mono ? 1 : 0) << 48) |
                       (static_cast<uint64_t>(px_size & 0xffff) << 32) |
                       static_cast<uint64_t>(codepoint);
        auto it = glyph_cache_.find(key);
        if (it != glyph_cache_.end()) {
            return &it->second;
        }

        FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(px_size));
        if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) != 0) {
            return nullptr;
        }

        FT_GlyphSlot slot = face->glyph;
        CachedGlyph cg;
        cg.width = static_cast<int>(slot->bitmap.width);
        cg.rows = static_cast<int>(slot->bitmap.rows);
        cg.left = slot->bitmap_left;
        cg.top = slot->bitmap_top;
        cg.advance_x = static_cast<int>(slot->advance.x >> 6);
        cg.alpha.resize(static_cast<size_t>(cg.width * cg.rows));

        if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
            for (int r = 0; r < cg.rows; ++r) {
                const uint8_t* src = slot->bitmap.buffer + r * slot->bitmap.pitch;
                std::memcpy(cg.alpha.data() + r * cg.width, src, static_cast<size_t>(cg.width));
            }
        } else if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
            for (int r = 0; r < cg.rows; ++r) {
                const uint8_t* src = slot->bitmap.buffer + r * slot->bitmap.pitch;
                for (int c = 0; c < cg.width; ++c) {
                    uint8_t byte = src[c >> 3];
                    bool on = (byte & (0x80U >> (c & 7))) != 0;
                    cg.alpha[static_cast<size_t>(r * cg.width + c)] = on ? 255 : 0;
                }
            }
        }

        auto [ins_it, _] = glyph_cache_.emplace(key, std::move(cg));
        return &ins_it->second;
    }
#endif

    void blend_pixel(int x, int y, uint32_t fg_rgb, uint8_t alpha) {
        if (x < 0 || x >= kW || y < 0 || y >= kH || alpha == 0) {
            return;
        }
        size_t idx = static_cast<size_t>(y * kW + x);
        if (alpha == 255) {
            buf_[idx] = 0xff000000U | (fg_rgb & 0x00ffffffU);
            return;
        }
        uint32_t bg = buf_[idx];
        uint32_t a = alpha;
        uint32_t inv = 255U - a;
        uint32_t fr = (fg_rgb >> 16) & 0xffU;
        uint32_t fg = (fg_rgb >> 8) & 0xffU;
        uint32_t fb = fg_rgb & 0xffU;
        uint32_t br = (bg >> 16) & 0xffU;
        uint32_t bg_c = (bg >> 8) & 0xffU;
        uint32_t bb = bg & 0xffU;
        uint32_t rr = (fr * a + br * inv + 127U) / 255U;
        uint32_t rg = (fg * a + bg_c * inv + 127U) / 255U;
        uint32_t rb = (fb * a + bb * inv + 127U) / 255U;
        buf_[idx] = 0xff000000U | (rr << 16) | (rg << 8) | rb;
    }

    std::vector<uint32_t> buf_;
    bool has_ft_sans_ = false;
    bool has_ft_mono_ = false;

#if defined(CP0_HAS_FREETYPE)
    FT_Library ft_lib_ = nullptr;
    FT_Face sans_face_ = nullptr;
    FT_Face mono_face_ = nullptr;
    std::map<uint64_t, CachedGlyph> glyph_cache_;
#endif
};

// ---------------------------------------------------------------------------
// Component drawing helpers matching Spec Section 5.4
// ---------------------------------------------------------------------------

void draw_kbd_hint(
    ShadcnCanvas& canvas,
    const ShadcnPalette& pal,
    int& cursor_x,
    int y,
    const std::string& key_text,
    const std::string& label
) {
    int kw = canvas.measure_text(FontRole::Mono, 9, key_text) + 6;
    canvas.draw_round_box(cursor_x, y, kw, 13, 3, pal.card, pal.border, 1);
    canvas.draw_text(FontRole::Mono, 9, cursor_x + 3, y + 2, key_text, pal.foreground);
    cursor_x += kw + 3;
    canvas.draw_text(FontRole::Sans, 9, cursor_x, y + 2, label, pal.muted_foreground);
    cursor_x += canvas.measure_text(FontRole::Sans, 9, label) + 7;
}

void draw_pill_badge(
    ShadcnCanvas& canvas,
    int x,
    int y,
    int h,
    const std::string& text,
    uint32_t bg_rgb,
    uint32_t border_rgb,
    uint32_t fg_rgb,
    bool with_dot,
    bool hollow_dot = false,
    bool with_check = false
) {
    int text_w = canvas.measure_text(FontRole::Sans, 9, text);
    int icon_w = (with_dot || with_check) ? 10 : 0;
    int w = icon_w + text_w + 10;
    canvas.draw_round_box(x, y, w, h, h / 2, bg_rgb, border_rgb, 1);
    int tx = x + 5;
    if (with_dot) {
        canvas.draw_circle(
            static_cast<float>(x + 8),
            static_cast<float>(y) + static_cast<float>(h) * 0.5f,
            2.5f,
            fg_rgb,
            hollow_dot
        );
        tx += 9;
    } else if (with_check) {
        canvas.draw_checkmark(x + 5, y + (h - 7) / 2, fg_rgb);
        tx += 9;
    }
    canvas.draw_text(FontRole::Sans, 9, tx, y + (h - 10) / 2, text, fg_rgb);
}

int measure_pill_badge_w(ShadcnCanvas& canvas, const std::string& text, bool with_icon) {
    int text_w = canvas.measure_text(FontRole::Sans, 9, text);
    return (with_icon ? 10 : 0) + text_w + 10;
}

} // namespace

// ---------------------------------------------------------------------------
// LvglAppView::Impl
// ---------------------------------------------------------------------------

struct LvglAppView::Impl {
    ThemeMode theme = ThemeMode::Dark;
    ShadcnPalette pal = get_palette(ThemeMode::Dark);
    ShadcnCanvas canvas;

#if defined(CP0_HAS_LVGL)
    lv_obj_t* screen = nullptr;
    lv_style_t style_bg{};
    lv_style_t style_card{};
    bool lvgl_styles_inited = false;

    void sync_lvgl_styles() {
        if (!lv_is_initialized()) {
            lv_init();
        }
        if (!lvgl_styles_inited) {
            lv_style_init(&style_bg);
            lv_style_init(&style_card);
            lvgl_styles_inited = true;
        }
        lv_style_set_bg_color(&style_bg, lv_color_hex(pal.background));
        lv_style_set_bg_color(&style_card, lv_color_hex(pal.card));
        lv_style_set_border_color(&style_card, lv_color_hex(pal.border));
        if (screen != nullptr) {
            lv_obj_report_style_change(&style_bg);
            lv_obj_report_style_change(&style_card);
        }
    }
#endif

    ~Impl() {
#if defined(CP0_HAS_LVGL)
        if (lvgl_styles_inited) {
            lv_style_reset(&style_bg);
            lv_style_reset(&style_card);
            lvgl_styles_inited = false;
        }
#endif
    }

    void render_top_bar(const UiState& state) {
        // Segmented TabsList pill container (radius 5px, padding 2px)
        const char* tab_labels[3] = {"1 Status", "2 Folders", "3 Logs"};
        int tab_widths[3]{};
        int list_w = 4;
        for (int i = 0; i < 3; ++i) {
            tab_widths[i] = canvas.measure_text(FontRole::Sans, 9, tab_labels[i]) + 10;
            list_w += tab_widths[i];
        }
        canvas.draw_round_box(6, 3, list_w, 18, 5, pal.muted, pal.border, 1);

        int tx = 8;
        for (int i = 0; i < 3; ++i) {
            bool active = (static_cast<int>(state.tab) == i);
            if (active) {
                int border_w = (pal.active_tab_border != pal.active_tab_bg) ? 1 : 0;
                canvas.draw_round_box(
                    tx,
                    5,
                    tab_widths[i],
                    14,
                    4,
                    pal.active_tab_bg,
                    pal.active_tab_border,
                    border_w
                );
            }
            uint32_t fg = active ? pal.active_tab_fg : pal.muted_foreground;
            canvas.draw_text(FontRole::Sans, 9, tx + 5, 7, tab_labels[i], fg);
            tx += tab_widths[i];
        }

        // Right-aligned pill status Badge
        std::string badge_text;
        uint32_t badge_bg = pal.muted;
        uint32_t badge_fg = pal.muted_foreground;
        uint32_t badge_border = pal.border;
        bool hollow = false;

        if (!state.status.ok) {
            badge_text = "Daemon Off";
            badge_bg = pal.status_error_bg;
            badge_fg = pal.status_error;
            badge_border = pal.status_error;
        } else if (state.status.target_host.empty()) {
            badge_text = "No Target";
            badge_bg = pal.muted;
            badge_fg = pal.muted_foreground;
            badge_border = pal.border;
            hollow = true;
        } else if (state.status.syncing) {
            badge_text = "Syncing";
            badge_bg = pal.status_online_bg;
            badge_fg = pal.status_online;
            badge_border = pal.status_online;
        } else if (state.status.mac_online) {
            badge_text = "Mac Online";
            badge_bg = pal.status_online_bg;
            badge_fg = pal.status_online;
            badge_border = pal.status_online;
        } else {
            badge_text = "Mac Sleeping";
            badge_bg = pal.status_sleeping_bg;
            badge_fg = pal.status_sleeping;
            badge_border = pal.status_sleeping;
        }

        int bw = measure_pill_badge_w(canvas, badge_text, true);
        draw_pill_badge(
            canvas,
            LvglAppView::kWidth - 6 - bw,
            3,
            18,
            badge_text,
            badge_bg,
            badge_border,
            badge_fg,
            true,
            hollow,
            false
        );
    }

    void render_status_tab(const UiState& state) {
        // Two side-by-side Bento Card stat tiles (radius 6px, 1px border)
        canvas.draw_round_box(6, 27, 151, 56, 6, pal.card, pal.border, 1);
        canvas.draw_text(FontRole::Sans, 9, 14, 33, "PENDING QUEUE", pal.muted_foreground);

        std::string pending_num = std::to_string(state.status.pending_count);
        canvas.draw_text(FontRole::Mono, 18, 14, 46, pending_num, pal.foreground);
        int num_w = canvas.measure_text(FontRole::Mono, 18, pending_num);
        canvas.draw_text(
            FontRole::Sans,
            9,
            14 + num_w + 5,
            53,
            state.status.pending_count == 1 ? "file" : "files",
            pal.muted_foreground
        );
        canvas.draw_text(
            FontRole::Mono,
            9,
            14,
            68,
            format_bytes(state.status.pending_bytes) + " queued",
            pal.muted_foreground
        );

        canvas.draw_round_box(163, 27, 151, 56, 6, pal.card, pal.border, 1);
        canvas.draw_text(FontRole::Sans, 9, 171, 33, "TOTAL SYNCED", pal.muted_foreground);
        canvas.draw_text(
            FontRole::Mono,
            18,
            171,
            46,
            std::to_string(state.status.synced_count),
            pal.foreground
        );
        if (state.status.failed_count > 0) {
            canvas.draw_text(
                FontRole::Sans,
                9,
                171,
                68,
                std::to_string(state.status.failed_count) + " failed (Tab 3)",
                pal.status_error
            );
        } else {
            canvas.draw_text(FontRole::Sans, 9, 171, 68, "1-way verified", pal.status_online);
        }

        // Full-width Target summary Card
        canvas.draw_round_box(6, 87, 308, 59, 6, pal.card, pal.border, 1);

        canvas.draw_text(FontRole::Sans, 9, 14, 94, "TARGET", pal.muted_foreground);
        int label_w = canvas.measure_text(FontRole::Sans, 9, "TARGET") + 6;

        std::string target_str = state.status.target_host.empty()
                                     ? "Not set (press C)"
                                     : (state.status.target_host + ":" +
                                        std::to_string(state.status.target_port));
        std::string right_note;
        uint32_t right_color = pal.muted_foreground;
        if (state.status.syncing && !state.status.current_file.empty()) {
            right_note = "Uploading";
            right_color = pal.status_online;
        } else if (state.status.next_retry_sec > 0 && !state.status.mac_online) {
            right_note = "Retry " + std::to_string(state.status.next_retry_sec) + "s";
            right_color = pal.status_sleeping;
        } else if (state.status.from_fallback) {
            right_note = "Cached";
        }
        int right_w = right_note.empty() ? 0 : (canvas.measure_text(FontRole::Sans, 9, right_note) + 8);
        int max_target_w = 292 - label_w - right_w;
        std::string target_fit = canvas.truncate_right(FontRole::Mono, 10, target_str, max_target_w);
        canvas.draw_text(FontRole::Mono, 10, 14 + label_w, 93, target_fit, pal.foreground);
        if (!right_note.empty()) {
            int rw = canvas.measure_text(FontRole::Sans, 9, right_note);
            canvas.draw_text(FontRole::Sans, 9, 306 - rw, 94, right_note, right_color);
        }

        std::string dest = state.status.mac_dest_root.empty()
                               ? "~/Pictures/CardputerZero/<folder>"
                               : (state.status.mac_dest_root + "/<folder>");
        std::string dest_fit = canvas.truncate_left_path(FontRole::Mono, 9, "-> " + dest, 292);
        canvas.draw_text(FontRole::Mono, 9, 14, 110, dest_fit, pal.muted_foreground);

        std::string footer_msg;
        uint32_t footer_color = pal.muted_foreground;
        if (state.status.syncing && !state.status.current_file.empty()) {
            footer_msg = "File: " + state.status.current_file;
            footer_color = pal.foreground;
        } else if (!state.status.last_error.empty() && state.status.last_error != "none") {
            footer_msg = "Err: " + state.status.last_error;
            footer_color = pal.status_error;
        } else if (!state.status.ok && !state.status.error.empty()) {
            footer_msg = state.status.error;
            footer_color = pal.status_error;
        } else if (!state.status.last_synced_file.empty()) {
            footer_msg = "Last sync: " + state.status.last_synced_file;
        } else {
            footer_msg = "Watching for new photos...";
        }
        std::string msg_fit = canvas.truncate_right(FontRole::Mono, 9, footer_msg, 292);
        canvas.draw_text(FontRole::Mono, 9, 14, 126, msg_fit, footer_color);
    }

    void render_folders_tab(const UiState& state) {
        if (state.status.folders.empty()) {
            canvas.draw_round_box(6, 27, 308, 119, 6, pal.card, pal.border, 1);
            std::string empty_msg = "No watched folders yet. Press [A] to add.";
            int ew = canvas.measure_text(FontRole::Sans, 10, empty_msg);
            canvas.draw_text(
                FontRole::Sans,
                10,
                6 + (308 - ew) / 2,
                80,
                empty_msg,
                pal.muted_foreground
            );
            return;
        }

        constexpr int kVisible = 4;
        int total = static_cast<int>(state.status.folders.size());
        int start = 0;
        if (state.folder_cursor >= kVisible) {
            start = state.folder_cursor - kVisible + 1;
        }
        if (start + kVisible > total) {
            start = std::max(0, total - kVisible);
        }

        for (int slot = 0; slot < kVisible && (start + slot) < total; ++slot) {
            int idx = start + slot;
            const auto& f = state.status.folders[static_cast<size_t>(idx)];
            bool selected = (idx == state.folder_cursor);
            int ry = 27 + slot * 30;
            canvas.draw_round_box(
                6,
                ry,
                308,
                27,
                6,
                selected ? pal.muted : pal.card,
                selected ? pal.ring : pal.border,
                1
            );

            std::string badge_text;
            uint32_t b_bg = pal.status_online_bg;
            uint32_t b_fg = pal.status_online;
            bool check = false;
            if (!f.exists) {
                badge_text = "! missing";
                b_bg = pal.status_error_bg;
                b_fg = pal.status_error;
            } else if (f.failed > 0) {
                badge_text = std::to_string(f.failed) + " err";
                b_bg = pal.status_error_bg;
                b_fg = pal.status_error;
            } else if (f.pending > 0) {
                badge_text = std::to_string(f.pending) + " queued";
                b_bg = pal.status_sleeping_bg;
                b_fg = pal.status_sleeping;
            } else {
                badge_text = "synced";
                check = true;
            }

            int bw = measure_pill_badge_w(canvas, badge_text, check);
            draw_pill_badge(
                canvas,
                308 - bw,
                ry + 5,
                17,
                badge_text,
                b_bg,
                b_fg,
                b_fg,
                false,
                false,
                check
            );

            int max_text_w = 308 - 14 - bw - 12;
            std::string title = f.label.empty() ? "Folder" : f.label;
            std::string title_fit = canvas.truncate_right(FontRole::Sans, 9, title, max_text_w);
            canvas.draw_text(FontRole::Sans, 9, 13, ry + 4, title_fit, pal.foreground);

            std::string path_fit = canvas.truncate_left_path(FontRole::Mono, 9, f.path, max_text_w);
            canvas.draw_text(FontRole::Mono, 9, 13, ry + 15, path_fit, pal.muted_foreground);
        }
    }

    void render_logs_tab(const UiState& state) {
        canvas.draw_round_box(6, 27, 308, 119, 6, pal.card, pal.border, 1);
        if (state.status.logs.empty()) {
            std::string msg = "No daemon logs recorded yet.";
            int mw = canvas.measure_text(FontRole::Sans, 10, msg);
            canvas.draw_text(FontRole::Sans, 10, 6 + (308 - mw) / 2, 80, msg, pal.muted_foreground);
            return;
        }

        constexpr int kRows = 6;
        int total = static_cast<int>(state.status.logs.size());
        int start = std::min(state.log_scroll, std::max(0, total - kRows));

        for (int r = 0; r < kRows && (start + r) < total; ++r) {
            const auto& entry = state.status.logs[static_cast<size_t>(start + r)];
            int ry = 30 + r * 19;
            if (r > 0) {
                canvas.fill_rect(12, ry - 1, 296, 1, pal.border);
            }
            std::string tstr = format_hhmmss(entry.ts);
            canvas.draw_text(FontRole::Mono, 9, 12, ry + 4, tstr, pal.muted_foreground);

            std::string tag = short_log_tag(entry.level);
            uint32_t tag_color = pal.status_online;
            if (tag == "ER") {
                tag_color = pal.status_error;
            } else if (tag == "WN") {
                tag_color = pal.status_sleeping;
            }
            canvas.draw_text(FontRole::Mono, 9, 68, ry + 4, tag, tag_color);

            std::string msg_fit = canvas.truncate_right(FontRole::Mono, 9, entry.message, 218);
            canvas.draw_text(FontRole::Mono, 9, 86, ry + 4, msg_fit, pal.foreground);
        }
    }

    void render_modal(const UiState& state) {
        if (state.modal == ModalKind::None) {
            return;
        }
        canvas.dim_rect(0, 0, LvglAppView::kWidth, 150, 0x000000, 145);

        if (state.modal == ModalKind::TargetPicker) {
            canvas.draw_round_box(12, 14, 296, 130, 6, pal.card, pal.ring, 1);
            canvas.draw_text(FontRole::Sans, 10, 20, 20, "Select Target Mac", pal.foreground);
            std::string manual_hint = "[T] Manual";
            int hw = canvas.measure_text(FontRole::Mono, 9, manual_hint);
            canvas.draw_text(FontRole::Mono, 9, 300 - hw, 21, manual_hint, pal.muted_foreground);
            canvas.fill_rect(18, 34, 284, 1, pal.border);

            if (state.peers.empty()) {
                canvas.draw_text(
                    FontRole::Sans,
                    10,
                    22,
                    66,
                    "No Tailscale peers found.",
                    pal.muted_foreground
                );
                canvas.draw_text(
                    FontRole::Sans,
                    9,
                    22,
                    82,
                    "Press [T] to type host[:port] manually.",
                    pal.muted_foreground
                );
                return;
            }

            constexpr int kRows = 4;
            int total = static_cast<int>(state.peers.size());
            int start = 0;
            if (state.peer_cursor >= kRows) {
                start = state.peer_cursor - kRows + 1;
            }
            if (start + kRows > total) {
                start = std::max(0, total - kRows);
            }

            for (int r = 0; r < kRows && (start + r) < total; ++r) {
                int idx = start + r;
                const auto& peer = state.peers[static_cast<size_t>(idx)];
                bool sel = (idx == state.peer_cursor);
                int ry = 38 + r * 25;
                canvas.draw_round_box(
                    18,
                    ry,
                    284,
                    22,
                    4,
                    sel ? pal.muted : pal.card,
                    sel ? pal.ring : pal.border,
                    1
                );
                canvas.draw_circle(
                    28.0f,
                    static_cast<float>(ry + 11),
                    3.0f,
                    peer.online ? pal.status_online : pal.muted_foreground,
                    !peer.online
                );
                std::string host = !peer.dns_name.empty()
                                       ? peer.dns_name
                                       : (!peer.tailscale_ips.empty() ? peer.tailscale_ips[0] : peer.hostname);
                std::string line = peer.hostname + " (" + peer.os + ") " + host;
                std::string fit = canvas.truncate_right(FontRole::Mono, 9, line, 258);
                canvas.draw_text(FontRole::Mono, 9, 36, ry + 6, fit, pal.foreground);
            }
            return;
        }

        if (state.modal == ModalKind::DirPicker) {
            canvas.draw_round_box(12, 14, 296, 130, 6, pal.card, pal.ring, 1);
            canvas.draw_text(FontRole::Sans, 10, 20, 19, "Add Watch Folder", pal.foreground);
            std::string manual_hint = "[T] Manual";
            int hw = canvas.measure_text(FontRole::Mono, 9, manual_hint);
            canvas.draw_text(FontRole::Mono, 9, 300 - hw, 20, manual_hint, pal.muted_foreground);

            canvas.draw_round_box(18, 33, 284, 16, 3, pal.muted, pal.border, 1);
            std::string cur_fit = canvas.truncate_left_path(
                FontRole::Mono,
                9,
                state.dir_current_path,
                272
            );
            canvas.draw_text(FontRole::Mono, 9, 24, 36, cur_fit, pal.foreground);

            if (state.dir_entries.empty()) {
                canvas.draw_text(
                    FontRole::Sans,
                    9,
                    22,
                    72,
                    "(No subdirectories — Space adds current dir)",
                    pal.muted_foreground
                );
                return;
            }

            constexpr int kRows = 4;
            int total = static_cast<int>(state.dir_entries.size());
            int start = 0;
            if (state.dir_cursor >= kRows) {
                start = state.dir_cursor - kRows + 1;
            }
            if (start + kRows > total) {
                start = std::max(0, total - kRows);
            }

            for (int r = 0; r < kRows && (start + r) < total; ++r) {
                int idx = start + r;
                bool sel = (idx == state.dir_cursor);
                int ry = 53 + r * 22;
                canvas.draw_round_box(
                    18,
                    ry,
                    284,
                    19,
                    4,
                    sel ? pal.muted : pal.card,
                    sel ? pal.ring : pal.border,
                    1
                );
                const std::string& full = state.dir_entries[static_cast<size_t>(idx)];
                size_t slash = full.rfind('/');
                std::string name = (slash != std::string::npos) ? full.substr(slash + 1) : full;
                std::string disp = "> " + name + "/";
                std::string fit = canvas.truncate_right(FontRole::Mono, 9, disp, 268);
                canvas.draw_text(FontRole::Mono, 9, 24, ry + 5, fit, pal.foreground);
            }
            return;
        }

        if (state.modal == ModalKind::ConfirmRemove) {
            canvas.draw_round_box(22, 30, 276, 98, 6, pal.card, pal.ring, 1);
            canvas.draw_text(FontRole::Sans, 11, 34, 40, "Remove Watched Folder?", pal.foreground);
            std::string path_fit = canvas.truncate_left_path(
                FontRole::Mono,
                9,
                state.confirm_folder_path,
                252
            );
            canvas.draw_text(FontRole::Mono, 9, 34, 60, path_fit, pal.muted_foreground);
            canvas.draw_text(
                FontRole::Sans,
                9,
                34,
                78,
                "Local files on disk will NOT be deleted.",
                pal.status_online
            );
            canvas.draw_text(
                FontRole::Mono,
                9,
                34,
                100,
                "[Y] Confirm Remove    [N / Esc] Cancel",
                pal.foreground
            );
            return;
        }

        if (state.modal == ModalKind::TextInput) {
            canvas.draw_round_box(18, 32, 284, 92, 6, pal.card, pal.ring, 1);
            std::string title = (state.text_purpose == TextInputPurpose::TargetHost)
                                    ? "Enter Mac Host[:Port]"
                                    : "Enter Folder Path";
            canvas.draw_text(FontRole::Sans, 10, 28, 42, title, pal.foreground);

            canvas.draw_round_box(28, 60, 264, 24, 4, pal.muted, pal.ring, 1);
            std::string shown = canvas.truncate_left_path(
                FontRole::Mono,
                10,
                state.text_buffer + "_",
                250
            );
            canvas.draw_text(FontRole::Mono, 10, 35, 67, shown, pal.foreground);

            if (!state.modal_error.empty()) {
                std::string err = canvas.truncate_right(FontRole::Sans, 9, state.modal_error, 264);
                canvas.draw_text(FontRole::Sans, 9, 28, 94, err, pal.status_error);
            } else {
                std::string ex = (state.text_purpose == TextInputPurpose::TargetHost)
                                     ? "Example: macbook.ts.net or 100.64.0.5:8765"
                                     : "Example: /media/sdcard/DCIM";
                canvas.draw_text(FontRole::Mono, 9, 28, 94, ex, pal.muted_foreground);
            }
        }
    }

    void render_footer(const UiState& state) {
        canvas.fill_rect(0, 150, LvglAppView::kWidth, 20, pal.muted);
        canvas.fill_rect(0, 150, LvglAppView::kWidth, 1, pal.border);

        int x = 6;
        int y = 154;

        switch (state.modal) {
            case ModalKind::TargetPicker:
                draw_kbd_hint(canvas, pal, x, y, "j/k", "Move");
                draw_kbd_hint(canvas, pal, x, y, "Ent", "Use");
                draw_kbd_hint(canvas, pal, x, y, "T", "Type");
                draw_kbd_hint(canvas, pal, x, y, "Esc", "Back");
                return;
            case ModalKind::DirPicker:
                draw_kbd_hint(canvas, pal, x, y, "Ent", "Open");
                draw_kbd_hint(canvas, pal, x, y, "Bksp", "Up");
                draw_kbd_hint(canvas, pal, x, y, "Spc", "Watch");
                draw_kbd_hint(canvas, pal, x, y, "T", "Type");
                return;
            case ModalKind::ConfirmRemove:
                draw_kbd_hint(canvas, pal, x, y, "Y", "Remove");
                draw_kbd_hint(canvas, pal, x, y, "N/Esc", "Cancel");
                return;
            case ModalKind::TextInput:
                draw_kbd_hint(canvas, pal, x, y, "Bksp", "Del");
                draw_kbd_hint(canvas, pal, x, y, "Ent", "Save");
                draw_kbd_hint(canvas, pal, x, y, "Esc", "Cancel");
                return;
            case ModalKind::None:
                break;
        }

        switch (state.tab) {
            case ActiveTab::Status:
                draw_kbd_hint(canvas, pal, x, y, "Ent", "Sync");
                draw_kbd_hint(canvas, pal, x, y, "C", "Target");
                draw_kbd_hint(canvas, pal, x, y, "T", "Theme");
                draw_kbd_hint(canvas, pal, x, y, "Q", "Quit");
                break;
            case ActiveTab::Folders:
                draw_kbd_hint(canvas, pal, x, y, "A", "Add");
                draw_kbd_hint(canvas, pal, x, y, "D", "Del");
                draw_kbd_hint(canvas, pal, x, y, "Ent", "Sync");
                draw_kbd_hint(canvas, pal, x, y, "T", "Theme");
                break;
            case ActiveTab::Logs:
                draw_kbd_hint(canvas, pal, x, y, "j/k", "Scroll");
                draw_kbd_hint(canvas, pal, x, y, "R", "Retry");
                draw_kbd_hint(canvas, pal, x, y, "T", "Theme");
                draw_kbd_hint(canvas, pal, x, y, "Q", "Quit");
                break;
        }
    }

    void render_feedback_toast(const UiState& state) {
        if (state.modal != ModalKind::None || state.feedback_banner.empty()) {
            return;
        }
        uint32_t bg = state.feedback_is_error ? pal.status_error_bg : pal.status_online_bg;
        uint32_t fg = state.feedback_is_error ? pal.status_error : pal.status_online;
        canvas.draw_round_box(6, 129, 308, 19, 5, bg, fg, 1);
        canvas.draw_circle(15.0f, 138.5f, 2.5f, fg, false);
        std::string fit = canvas.truncate_right(FontRole::Mono, 9, state.feedback_banner, 282);
        canvas.draw_text(FontRole::Mono, 9, 23, 134, fit, fg);
    }

    void render(const UiState& state) {
        if (state.theme != theme) {
            theme = state.theme;
            pal = get_palette(theme);
        }
        canvas.clear(pal.background);
        render_top_bar(state);

        switch (state.tab) {
            case ActiveTab::Status:
                render_status_tab(state);
                break;
            case ActiveTab::Folders:
                render_folders_tab(state);
                break;
            case ActiveTab::Logs:
                render_logs_tab(state);
                break;
        }

        render_feedback_toast(state);
        render_modal(state);
        render_footer(state);

#if defined(CP0_HAS_LVGL)
        sync_lvgl_styles();
#endif
    }
};

LvglAppView::LvglAppView() : impl_(std::make_unique<Impl>()) {}
LvglAppView::~LvglAppView() = default;

void LvglAppView::apply_theme(ThemeMode mode) {
    impl_->theme = mode;
    impl_->pal = get_palette(mode);
#if defined(CP0_HAS_LVGL)
    impl_->sync_lvgl_styles();
#endif
}

void LvglAppView::render(const UiState& state) {
    impl_->render(state);
}

const uint32_t* LvglAppView::pixels() const {
    return impl_->canvas.pixels();
}

bool LvglAppView::save_ppm(const std::string& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    out << "P6\n" << kWidth << " " << kHeight << "\n255\n";
    const uint32_t* px = pixels();
    std::vector<uint8_t> rgb(static_cast<size_t>(kWidth * kHeight * 3));
    for (int i = 0; i < kWidth * kHeight; ++i) {
        uint32_t c = px[i];
        rgb[static_cast<size_t>(i * 3 + 0)] = static_cast<uint8_t>((c >> 16) & 0xffU);
        rgb[static_cast<size_t>(i * 3 + 1)] = static_cast<uint8_t>((c >> 8) & 0xffU);
        rgb[static_cast<size_t>(i * 3 + 2)] = static_cast<uint8_t>(c & 0xffU);
    }
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    return out.good();
}

bool LvglAppView::using_freetype() const {
    return impl_->canvas.using_freetype();
}

ThemeMode LvglAppView::active_theme() const {
    return impl_->theme;
}

} // namespace cp0
