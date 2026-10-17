// 言語のメニューに出す旗。assets/flags/*.png（circle-flags を rsvg-convert で 64×64 にしたもの）を
// CMake が flag_icons_data.cpp に焼き込む。実行時にファイルやネットワークは使わない。
#pragma once

#include "i18n.h"

#include <cairo.h>

#include <cstddef>

/** 焼き込んだ PNG 1 枚（name は assets/flags のファイル名から .png を除いたもの）。 */
struct FlagPng {
    const char* name;
    const unsigned char* data;
    std::size_t size;
};

extern const FlagPng kFlagPngs[];
extern const std::size_t kFlagPngCount;

/**
 * 言語の旗を正方形に縮めて描く。旗が無ければ何も描かない。
 * @param cr cairo
 * @param language 言語
 * @param x 左
 * @param y 上
 * @param size 一辺（px）
 */
void drawLanguageFlag(cairo_t* cr, Language language, double x, double y, double size);
