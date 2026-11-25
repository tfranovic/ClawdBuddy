// Workaround for an ESP-IDF RGB panel bug (fixed upstream in "fix(rgb_lcd):
// resync bounce buffer index when restarting DMA", after v5.5.6; the
// precompiled Arduino libs here are 5.5.4 and don't have it).
//
// The driver sends the picture through two bounce buffers and refills
// whichever one the DMA has just finished. Which one that is comes from a
// running count of DMA end-of-buffer interrupts (`bb_eof_count`, odd or
// even). Every frame the driver restarts the DMA at buffer 0, but with
// CONFIG_LCD_RGB_RESTART_IN_VSYNC (set in the Arduino libs) it does not put
// the count back to 0. Interrupts are held off during flash writes, so
// sooner or later an odd number of those interrupts is lost, and from then
// until a reboot every refill lands in the buffer the panel is reading:
// the picture sits one buffer too high, wrapping at the bottom, and the
// first pixels of each buffer show what it held before (thin echoes at the
// left edge).
//
// The fix is the upstream one: count from 0 again at every vsync. The count
// is private to the driver, so it is found from the outside: the vsync
// callback registered here is stored three words after it
// (bounce_pos_px, bb_eof_count, expect_eof_count, on_color_trans_done,
// on_frame_buf_complete, on_vsync, on_bounce_empty, user_ctx; the same
// order in every 5.x release). The words around it are checked before the
// address is trusted; if they don't fit, nothing is touched.
//
// esp_lcd_new_rgb_panel is wrapped at link time (-Wl,--wrap in
// platformio.ini) because Arduino_GFX keeps the panel handle private.
#include "board.h"
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_rgb.h>

extern "C" esp_err_t __real_esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t* cfg,
                                                  esp_lcd_panel_handle_t* ret);

static volatile size_t* DRAM_ATTR eof_count = nullptr;
static char ctx_tag;   // its address marks our user_ctx in the driver's struct

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*) {
    if (eof_count) *eof_count = 0;
    return false;
}

static volatile size_t* find_eof_count(esp_lcd_panel_handle_t panel, size_t expect_eofs) {
    // The handle is the driver struct's first member, so it is the block
    // the driver allocated.
    const size_t words = heap_caps_get_allocated_size(panel) / sizeof(uintptr_t);
    uintptr_t* w = (uintptr_t*)panel;
    for (size_t i = 5; i + 2 < words; i++) {
        if (w[i] != (uintptr_t)&on_vsync) continue;
        const bool fits = w[i + 1] == 0                          // on_bounce_empty
                       && w[i + 2] == (uintptr_t)&ctx_tag        // user_ctx
                       && w[i - 1] == 0 && w[i - 2] == 0         // the other callbacks
                       && w[i - 3] == expect_eofs;               // expect_eof_count
        return fits ? (volatile size_t*)&w[i - 4] : nullptr;
    }
    return nullptr;
}

extern "C" esp_err_t __wrap_esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t* cfg,
                                                  esp_lcd_panel_handle_t* ret) {
    esp_err_t err = __real_esp_lcd_new_rgb_panel(cfg, ret);
    if (err != ESP_OK || !cfg->bounce_buffer_size_px) return err;
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_vsync = on_vsync;
    if (esp_lcd_rgb_panel_register_event_callbacks(*ret, &cbs, &ctx_tag) != ESP_OK) return err;
    const size_t expect = (size_t)cfg->timings.h_res * cfg->timings.v_res / cfg->bounce_buffer_size_px;
    eof_count = find_eof_count(*ret, expect);
    return err;
}

bool rgb_resync_armed(void) {
    return eof_count != nullptr;
}
