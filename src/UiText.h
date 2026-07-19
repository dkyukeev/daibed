#pragma once

#include "raylib.h"

void DrawTextUtf8(const char* text, int posX, int posY, int fontSize, Color color);
int MeasureTextUtf8(const char* text, int fontSize);
// Rasterizes every quantized pixel-font atlas up front (call once right after
// InitWindow). Without this the first text of a new size builds a ~600-glyph
// atlas mid-frame — a visible hitch.
void PreloadUiFonts();
