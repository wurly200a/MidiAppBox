// ---- color_probe.hpp ----
#pragma once
#include <stdint.h>
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

// 8bit(R,G,B) -> 16bit RGB565 (from higher to lower bits)
// swap_bytes=false: convert 0xRRRGGGBB (MSB...LSB of 565) directly to two bytes
// swap_bytes=true : send those two bytes as [LSB, MSB]
static inline void put_rgb565(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, bool swap_bytes) {
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    if (swap_bytes) {
        dst[0] = v & 0xFF;       // LSB
        dst[1] = (v >> 8) & 0xFF;// MSB
    } else {
        dst[0] = (v >> 8) & 0xFF;// MSB
        dst[1] = v & 0xFF;       // LSB
    }
}

// Toggle RGB/BGR byte order
static inline void put_pixel(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b,
                             bool bgr_order, bool swap_bytes) {
    if (bgr_order) put_rgb565(dst, b, g, r, swap_bytes);
    else           put_rgb565(dst, r, g, b, swap_bytes);
}

static inline void fill_solid_rect(esp_lcd_panel_handle_t panel,
                                   int x0, int y0, int w, int h,
                                   uint8_t r, uint8_t g, uint8_t b,
                                   bool bgr_order, bool swap_bytes) {
    const int px = w * h;
    static uint8_t* buf = nullptr;
    static size_t   buf_bytes = 0;
    size_t need = px * 2;
    if (need > buf_bytes) {
        free(buf);
        buf = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_DMA);
        buf_bytes = need;
    }
    for (int i = 0; i < px; ++i) {
        put_pixel(&buf[i*2], r, g, b, bgr_order, swap_bytes);
    }
    esp_lcd_panel_draw_bitmap(panel, x0, y0, x0 + w, y0 + h, buf);
}

static inline void draw_9color_grid(esp_lcd_panel_handle_t panel,
                                    int x, int y, int cell_w, int cell_h,
                                    bool bgr_order, bool swap_bytes) {
    // 3x3: R,G,B / W,Gray,Yellow / Cyan,Magenta,Black
    const uint32_t cols[9] = {
        0xFF0000, 0x00FF00, 0x0000FF,
        0xFFFFFF, 0x808080, 0xFFFF00,
        0x00FFFF, 0xFF00FF, 0x000000
    };
    int idx = 0;
    for (int gy = 0; gy < 3; ++gy) {
        for (int gx = 0; gx < 3; ++gx) {
            uint32_t c = cols[idx++];
            uint8_t r = (c >> 16) & 0xFF;
            uint8_t g = (c >> 8)  & 0xFF;
            uint8_t b = (c >> 0)  & 0xFF;
            fill_solid_rect(panel,
                            x + gx*cell_w, y + gy*cell_h, cell_w, cell_h,
                            r, g, b, bgr_order, swap_bytes);
        }
    }
}

// Split the screen into 4 quadrants and show all combinations of RGB/BGR x byte-swap
// Top-left: RGB/NoSwap  Top-right: RGB/Swap
// Bottom-left: BGR/NoSwap  Bottom-right: BGR/Swap
static inline void probe_color_quadrants(esp_lcd_panel_handle_t panel, int H, int V) {
    int qw = H / 2;
    int qh = V / 2;
    int cw = qw / 3;
    int ch = qh / 3;

    // Clear background to black
    fill_solid_rect(panel, 0, 0, H, V, 0, 0, 0, false, false);

    // Q1: RGB / NoSwap
    draw_9color_grid(panel, 0, 0, cw, ch, /*bgr=*/false, /*swap=*/false);
    // Q2: RGB / Swap
    draw_9color_grid(panel, qw, 0, cw, ch, /*bgr=*/false, /*swap=*/true);
    // Q3: BGR / NoSwap
    draw_9color_grid(panel, 0, qh, cw, ch, /*bgr=*/true,  /*swap=*/false);
    // Q4: BGR / Swap
    draw_9color_grid(panel, qw, qh, cw, ch, /*bgr=*/true,  /*swap=*/true);
}
