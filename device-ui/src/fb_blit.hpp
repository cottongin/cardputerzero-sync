#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace cp0 {

struct FbChannel {
    uint32_t offset = 0;
    uint32_t length = 0;
};

struct FbBlitSpec {
    uint32_t xres = 0;
    uint32_t yres = 0;
    uint32_t xoffset = 0;
    uint32_t yoffset = 0;
    uint32_t bits_per_pixel = 0;
    uint32_t line_length = 0;
    FbChannel red;
    FbChannel green;
    FbChannel blue;
};

inline uint32_t pack_fb_channel(uint32_t val8, const FbChannel& ch) {
    uint32_t scaled = (ch.length >= 8)
                          ? (val8 << (ch.length - 8))
                          : (val8 >> (8 - ch.length));
    uint32_t mask = (ch.length >= 32) ? 0xffffffffU : ((1U << ch.length) - 1U);
    return (scaled & mask) << ch.offset;
}

inline bool blit_framebuffer(
    uint8_t* fb_mem,
    size_t map_size,
    const FbBlitSpec& spec,
    const uint32_t* pixels,
    int src_w,
    int src_h
) {
    if (fb_mem == nullptr || pixels == nullptr || map_size == 0 || src_w <= 0 || src_h <= 0) {
        return false;
    }
    if (spec.bits_per_pixel != 16 && spec.bits_per_pixel != 24 && spec.bits_per_pixel != 32) {
        return false;
    }
    auto valid_channel = [&](const FbChannel& ch) {
        return ch.length > 0 && ch.length <= 16 &&
               ch.offset < spec.bits_per_pixel &&
               ch.offset + ch.length <= spec.bits_per_pixel;
    };
    if (!valid_channel(spec.red) || !valid_channel(spec.green) || !valid_channel(spec.blue)) {
        return false;
    }

    size_t copy_w = std::min(static_cast<size_t>(spec.xres), static_cast<size_t>(src_w));
    size_t copy_h = std::min(static_cast<size_t>(spec.yres), static_cast<size_t>(src_h));
    if (copy_w == 0 || copy_h == 0) {
        return false;
    }

    size_t bytes_per_pixel = static_cast<size_t>(spec.bits_per_pixel) / 8U;
    size_t stride = static_cast<size_t>(spec.line_length);
    if (copy_w > std::numeric_limits<size_t>::max() / bytes_per_pixel) {
        return false;
    }
    size_t row_bytes = copy_w * bytes_per_pixel;
    if (stride < row_bytes) {
        return false;
    }
    if (static_cast<size_t>(spec.xoffset) > std::numeric_limits<size_t>::max() / bytes_per_pixel) {
        return false;
    }
    size_t x_byte_offset = static_cast<size_t>(spec.xoffset) * bytes_per_pixel;
    if (x_byte_offset > stride - row_bytes) {
        return false;
    }

    bool fast_argb32 =
        (spec.bits_per_pixel == 32 &&
         spec.red.offset == 16 && spec.red.length == 8 &&
         spec.green.offset == 8 && spec.green.length == 8 &&
         spec.blue.offset == 0 && spec.blue.length == 8);

    for (size_t y = 0; y < copy_h; ++y) {
        size_t y_row = static_cast<size_t>(spec.yoffset) + y;
        if (y_row < static_cast<size_t>(spec.yoffset)) {
            return false;
        }
        if (y_row > std::numeric_limits<size_t>::max() / stride) {
            return false;
        }
        size_t row_base = y_row * stride;
        if (row_base > map_size || x_byte_offset > map_size - row_base) {
            return false;
        }
        size_t row_offset = row_base + x_byte_offset;
        if (row_bytes > map_size - row_offset) {
            return false;
        }

        uint8_t* row = fb_mem + row_offset;
        const uint32_t* src = pixels + y * static_cast<size_t>(src_w);

        if (fast_argb32) {
            std::memcpy(row, src, row_bytes);
        } else if (spec.bits_per_pixel == 32) {
            for (size_t x = 0; x < copy_w; ++x) {
                uint32_t c = src[x];
                uint32_t packed =
                    pack_fb_channel((c >> 16) & 0xffU, spec.red) |
                    pack_fb_channel((c >> 8) & 0xffU, spec.green) |
                    pack_fb_channel(c & 0xffU, spec.blue);
                std::memcpy(row + x * 4U, &packed, 4U);
            }
        } else if (spec.bits_per_pixel == 24) {
            for (size_t x = 0; x < copy_w; ++x) {
                uint32_t c = src[x];
                uint32_t packed =
                    pack_fb_channel((c >> 16) & 0xffU, spec.red) |
                    pack_fb_channel((c >> 8) & 0xffU, spec.green) |
                    pack_fb_channel(c & 0xffU, spec.blue);
                row[x * 3U + 0U] = static_cast<uint8_t>(packed & 0xffU);
                row[x * 3U + 1U] = static_cast<uint8_t>((packed >> 8) & 0xffU);
                row[x * 3U + 2U] = static_cast<uint8_t>((packed >> 16) & 0xffU);
            }
        } else {
            for (size_t x = 0; x < copy_w; ++x) {
                uint32_t c = src[x];
                uint16_t packed = static_cast<uint16_t>(
                    pack_fb_channel((c >> 16) & 0xffU, spec.red) |
                    pack_fb_channel((c >> 8) & 0xffU, spec.green) |
                    pack_fb_channel(c & 0xffU, spec.blue)
                );
                std::memcpy(row + x * 2U, &packed, 2U);
            }
        }
    }
    return true;
}

} // namespace cp0
