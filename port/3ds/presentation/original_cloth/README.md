# Original fallback cloth

A quiet navy mathematical weave, created without disc assets or image inputs. Original project code and generated artwork under MIT. The existing RGB565 framebuffer format/layout and opacity handling are unchanged. Only the backend-off fallback texture is replaced; in-game UI and the pending live-map panel keep their separate resource paths.

Regenerate with `python3 generate.py --output ../../src/bottom_ui_texture_3ds.h` from this directory. The caller's GPU translation unit must be rebuilt after updating the header; normal build ownership remains with Claude.
