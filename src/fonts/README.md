# Reading fonts

Montserrat SemiBold (SIL Open Font License 1.1, [OFL.txt](OFL.txt); from https://github.com/JulietaUla/Montserrat) at sizes LVGL doesn't ship, for the big numbers in the value and state widgets. ASCII and `°`, 4 bits per pixel. Made with LVGL's converter:

```
npx lv_font_conv@1.5.3 --font Montserrat-SemiBold.ttf -r 0x20-0x7E,0xB0 --size 96 --bpp 4 --format lvgl --lv-include lvgl.h -o reading_96.c
```

(and the same for 64, 48 and 36).
