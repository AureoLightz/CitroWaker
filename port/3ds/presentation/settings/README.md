# CitroWaker settings presentation

The panel uses original code-drawn navy/sea-blue frames and rasterized Noto Sans
text. No Nintendo artwork, game font, icon or music is used by this panel.
`make_font.py` rasterizes Noto Sans Regular at13px into settings_font_3ds.h;
FONT-LICENSE.txt contains the font's SIL Open Font License, from
https://github.com/notofonts/latin-greek-cyrillic/blob/main/OFL.txt. The generated mask
is embedded, so no fonts, image decoder or runtime file reads are required.

START+SELECT opens the modal panel. D-pad selects/changes, A cycles, L/R changes
tabs; touch selects a tab or cycles a row. B cancels and Y saves. There is no
write until Save. Unknown performance.ini settings/comments are preserved.
Renderer/worker/texture/layout changes take effect next launch; HUD layout and
the optional bottom-frame counter update live. No thread is restarted mid-frame.

The game clock is frozen during the modal, pending render work is synchronized,
NDSP output is paused/cleared, and pacing deadlines are rebased on return.
The one-shot N3DS_SETTINGS_TEST diagnostic captures the actual framebuffer and
closes after one second without saving. It is absent unless explicitly enabled.
The FPS counter is off by default and follows the fallback panel's fade state;
it does not overlay a GPU-owned live map. Normal counter repaint is limited to4Hz.
