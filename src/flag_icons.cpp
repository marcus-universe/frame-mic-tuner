// 言語の旗を描く。flag_icons.h を参照。
#include "flag_icons.h"

#include <cstring>
#include <map>

namespace {

/** 言語ごとの旗のファイル名（circle-flags の名前）。 */
const char* flagName(Language language) {
    switch (language) {
        case Language::Ja: return "jp";
        case Language::En: return "en";
        case Language::Sc: return "cn";
        case Language::De: return "de";
        case Language::Fr: return "fr";
        case Language::Es: return "es";
        case Language::It: return "it";
    }
    return "";
}

/** メモリの中の PNG を読む位置。 */
struct PngReader {
    const unsigned char* data;
    std::size_t size;
    std::size_t pos;
};

cairo_status_t readPng(void* closure, unsigned char* out, unsigned int length) {
    PngReader* r = static_cast<PngReader*>(closure);
    if (r->pos + length > r->size) return CAIRO_STATUS_READ_ERROR;
    std::memcpy(out, r->data + r->pos, length);
    r->pos += length;
    return CAIRO_STATUS_SUCCESS;
}

/** 言語の旗の画像（初めて使うときに PNG を開いて、終わるまで持っておく）。@return 無ければ nullptr */
cairo_surface_t* flagSurface(Language language) {
    static std::map<Language, cairo_surface_t*> cache;
    const auto found = cache.find(language);
    if (found != cache.end()) return found->second;
    cairo_surface_t* surface = nullptr;
    const char* name = flagName(language);
    for (std::size_t i = 0; i < kFlagPngCount; ++i) {
        if (std::strcmp(kFlagPngs[i].name, name) != 0) continue;
        PngReader reader {kFlagPngs[i].data, kFlagPngs[i].size, 0};
        surface = cairo_image_surface_create_from_png_stream(readPng, &reader);
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS || cairo_image_surface_get_width(surface) <= 0) {
            cairo_surface_destroy(surface);
            surface = nullptr;
        }
        break;
    }
    cache[language] = surface;
    return surface;
}

}  // namespace

void drawLanguageFlag(cairo_t* cr, Language language, double x, double y, double size) {
    cairo_surface_t* flag = flagSurface(language);
    if (flag == nullptr) return;
    const double scale = size / cairo_image_surface_get_width(flag);
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, flag, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
}
