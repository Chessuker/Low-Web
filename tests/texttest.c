// texttest.c — renders Thai and Latin text at several sizes through lw_text.
#include "lowweb.h"
static unsigned fb[900 * 600];
LW_EXPORT(lw_start) void lw_start(void) {
    for (int i = 0; i < 900 * 600; i++) fb[i] = 0xFF2A1E0F;
    static const int sizes[] = {12, 13, 14, 16, 20, 28};
    int y = 10;
    for (int k = 0; k < 6; k++) {
        const char *s = "ข้อความจากเซิร์ฟเวอร์ ปู่ ญี่ปุ่น Hello";
        int x = 10;
        x += lw_text(fb, 900, 600, x, y, LW_STR(s), sizes[k], 0, 0xFFFFFFFF) + 20;
        lw_text(fb, 900, 600, x, y, LW_STR(s), sizes[k], LW_TEXT_BOLD, 0xFFF8BD38);
        y += sizes[k] * 2;
    }
    lw_present(fb, 900, 600);
}
