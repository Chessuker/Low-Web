// hello.c — the smallest useful Low-web page. Copy it to start a new site.
//
//   clang --target=wasm32 -O2 -mcpu=mvp -mbulk-memory -mnontrapping-fptoint -msign-ext
//         -mmutable-globals -nostdlib -fno-builtin -Wl,--no-entry -Isdk -o index.wasm sdk/hello.c
#include "lowweb.h"

#define W 640
#define H 360
static unsigned fb[W * H];  // pixels as 0xAABBGGRR (R,G,B,A bytes in memory)
static int clicks, dirty = 1;

LW_EXPORT(lw_start) void lw_start(void) { lw_set_title(LW_STR("Hello")); }

LW_EXPORT(lw_frame) void lw_frame(double ms) {
    (void)ms;
    if (!dirty) return;  // redraw only when something changed
    dirty = 0;
    for (int i = 0; i < W * H; i++) fb[i] = 0xFF3B2A1E;
    lw_text(fb, W, H, 40, 40, LW_STR("สวัสดี Low-web!"), 40, LW_TEXT_BOLD, 0xFFFFFFFF);
    char msg[] = "clicks: 000";
    msg[8] = (char)('0' + clicks / 100 % 10);
    msg[9] = (char)('0' + clicks / 10 % 10);
    msg[10] = (char)('0' + clicks % 10);
    lw_text(fb, W, H, 40, 110, msg, 11, 22, 0, 0xFFF8BD38);
    lw_present(fb, W, H);
}

LW_EXPORT(lw_pointer) int lw_pointer(int kind, float x, float y, int button) {
    (void)x; (void)y; (void)button;
    if (kind == LW_DOWN) { clicks++; dirty = 1; }
    return LW_CURSOR_HAND;
}
