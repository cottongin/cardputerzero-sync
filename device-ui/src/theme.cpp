#include "theme.hpp"

namespace cp0 {

ShadcnPalette get_palette(ThemeMode mode) {
    if (mode == ThemeMode::Light) {
        return ShadcnPalette{
            /* background */ 0xfafafa,
            /* card */ 0xffffff,
            /* muted */ 0xf4f4f5,
            /* border */ 0xe4e4e7,
            /* ring */ 0x71717a,
            /* foreground */ 0x09090b,
            /* muted_foreground */ 0x71717a,
            /* active_tab_bg */ 0xffffff,
            /* active_tab_fg */ 0x09090b,
            /* active_tab_border */ 0xe4e4e7,
            /* status_online */ 0x16a34a,
            /* status_online_bg */ 0xdcfce7,
            /* status_sleeping */ 0xd97706,
            /* status_sleeping_bg */ 0xfef3c7,
            /* status_error */ 0xdc2626,
            /* status_error_bg */ 0xfee2e2,
        };
    }

    return ShadcnPalette{
        /* background */ 0x09090b,
        /* card */ 0x09090b,
        /* muted */ 0x18181b,
        /* border */ 0x27272a,
        /* ring */ 0xd4d4d8,
        /* foreground */ 0xfafafa,
        /* muted_foreground */ 0xa1a1aa,
        /* active_tab_bg */ 0x27272a,
        /* active_tab_fg */ 0xfafafa,
        /* active_tab_border */ 0x27272a,
        /* status_online */ 0x4ade80,
        /* status_online_bg */ 0x11261a,
        /* status_sleeping */ 0xfbbf24,
        /* status_sleeping_bg */ 0x271d0b,
        /* status_error */ 0xf87171,
        /* status_error_bg */ 0x2b1214,
    };
}

ThemeMode toggle_theme(ThemeMode mode) {
    return mode == ThemeMode::Dark ? ThemeMode::Light : ThemeMode::Dark;
}

std::string theme_to_string(ThemeMode mode) {
    return mode == ThemeMode::Light ? "light" : "dark";
}

ThemeMode theme_from_string(const std::string& theme) {
    return theme == "light" ? ThemeMode::Light : ThemeMode::Dark;
}

} // namespace cp0
