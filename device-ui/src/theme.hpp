#pragma once

#include <cstdint>
#include <string>

namespace cp0 {

/// Supported UI color themes matching daemon config values ("dark" | "light").
enum class ThemeMode {
    Dark,
    Light,
};

/// Exact shadcn/ui Zinc color tokens (24-bit RGB hex values).
struct ShadcnPalette {
    uint32_t background;
    uint32_t card;
    uint32_t muted;
    uint32_t border;
    uint32_t ring;
    uint32_t foreground;
    uint32_t muted_foreground;
    uint32_t active_tab_bg;
    uint32_t active_tab_fg;
    uint32_t active_tab_border;
    uint32_t status_online;
    uint32_t status_online_bg;
    uint32_t status_sleeping;
    uint32_t status_sleeping_bg;
    uint32_t status_error;
    uint32_t status_error_bg;
};

/// Return the Zinc Dark or Zinc Light palette tokens from Spec Section 5.4.
ShadcnPalette get_palette(ThemeMode mode);

/// Toggle between ThemeMode::Dark and ThemeMode::Light.
ThemeMode toggle_theme(ThemeMode mode);

/// Convert ThemeMode to its persisted config/IPC string ("dark" or "light").
std::string theme_to_string(ThemeMode mode);

/// Parse a theme string ("light" -> Light, anything else -> Dark).
ThemeMode theme_from_string(const std::string& theme);

} // namespace cp0
