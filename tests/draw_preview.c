/* Host preview of the drawing library: writes a PPM test card. */
#include <stdio.h>
#include "draw.h"

static uint32_t px[640 * 360];

int main(int argc, char **argv) {
    surface_t s = { px, 640, 360, 640 };
    menu_t m = { { "Open", "Edit", "Properties" }, 3 };
    draw_fill(&s, 0, 0, 640, 360, 0x11161f);
    draw_shadow(&s, 40, 48, 320, 240, 10, 24, 0x000000, 150);
    draw_rrect(&s, 40, 40, 320, 34, 10, RR_TL | RR_TR, 0x1c2430, 255);
    draw_rrect(&s, 40, 74, 320, 206, 10, RR_BL | RR_BR, COL_BG, 255);
    draw_rrect_outline(&s, 40, 40, 320, 240, 10, 0xffffff, 28);
    draw_text_f(&s, 160, 49, "Explorer", COL_TEXT, FONT_BOLD);
    draw_circle(&s, 338, 57, 7, COL_DANGER, 255);
    draw_circle(&s, 314, 57, 7, COL_WARN, 255);
    ui_button(&s, 60, 90, 80, 28, "Back", 0, 0);
    ui_button(&s, 150, 90, 80, 28, "Save", 1, 0);
    draw_icon(&s, 70, 140, ICON_FOLDER);
    draw_icon(&s, 140, 140, ICON_FILE);
    draw_icon(&s, 210, 140, ICON_TERMINAL);
    draw_text(&s, 60, 200, "Xarı bülbül · Şuşa — anti-aliased text", COL_TEXT);
    draw_text_clip(&s, 60, 222, "a very long file name that will not fit here.txt", COL_MUTED, 200);
    for (const char *p = "shusha:/home$ ls"; *p; p++) draw_char(&s, 60 + (int)(p - "shusha:/home$ ls") * 8, 246, *p, COL_ACCENT_HI);
    draw_rrect(&s, 400, 270, 200, 64, 16, RR_ALL, 0x0a0e14, 190);
    draw_icon(&s, 412, 278, ICON_APP);
    draw_icon(&s, 472, 278, ICON_TERMINAL);
    menu_draw(&s, 420, 60, &m, 1);
    draw_cursor(&s, 470, 90);
    FILE *f = fopen(argc > 1 ? argv[1] : "card.ppm", "wb");
    fprintf(f, "P6 640 360 255\n");
    for (int i = 0; i < 640 * 360; i++) fputc(px[i] >> 16, f), fputc(px[i] >> 8 & 255, f), fputc(px[i] & 255, f);
    fclose(f);
    return 0;
}
