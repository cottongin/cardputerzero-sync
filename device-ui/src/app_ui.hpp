#pragma once

#include "theme.hpp"
#include "ui_controller.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace cp0 {

/// 320x170 shadcn/ui Zinc Bento Card & Segmented Tabs view renderer.
/// Supports LVGL v9 when CP0_HAS_LVGL is defined, and a built-in zero-external-LVGL
/// FreeType2 + anti-aliased RGBA framebuffer renderer otherwise.
class LvglAppView {
public:
    static constexpr int kWidth = 320;
    static constexpr int kHeight = 170;

    LvglAppView();
    ~LvglAppView();

    LvglAppView(const LvglAppView&) = delete;
    LvglAppView& operator=(const LvglAppView&) = delete;

    /// Swap active Zinc Dark / Zinc Light palette tokens without recreating view objects.
    void apply_theme(ThemeMode mode);

    /// Re-render the 320x170 view from the current UiState.
    void render(const UiState& state);

    /// Return 320*170 ARGB8888 (0xAARRGGBB) framebuffer pixels.
    const uint32_t* pixels() const;

    /// Write current 320x170 framebuffer as a binary PPM (P6) image.
    bool save_ppm(const std::string& path) const;

    /// True if at least one FreeType font (.ttf/.ttc/.otf) was loaded.
    bool using_freetype() const;

    /// Return currently applied theme mode.
    ThemeMode active_theme() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cp0
