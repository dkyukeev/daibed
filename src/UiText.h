#pragma once

#include "raylib.h"

void DrawTextUtf8(const char* text, int posX, int posY, int fontSize, Color color);
int MeasureTextUtf8(const char* text, int fontSize);
