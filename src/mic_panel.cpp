// マイクのパネルの実装。配置は見本（1200×788）のとおり。部品は frame-ui、色は theme.h（frame-ui と同じ）だけを使う。
#include "mic_panel.h"

#include "draw.h"
#include "i18n.h"
#include "outputs.h"
#include "theme.h"

#include "frame_apps_cairo.h"  // vendor/frame-apps/cpp

#include <cairo.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>

namespace {

using frame_ui::Rect;

constexpr int kWidth = 1200;
constexpr int kHeight = 788;

// ---- 見出し（状態ラベルとミュートのボタンは幅を決めてあるので、ミュートしても動かない） ----
constexpr Rect kChipRect {140, 28, 168, 44};
constexpr Rect kMuteRect {324, 22, 220, 56};
constexpr Rect kAppsRect {964, 22, 208, 56};
// ---- マイクの設定 ----
constexpr Rect kOutputSelect {28, 92, 380, 60};   ///< 「音の出口」の欄
constexpr Rect kMicSelect {420, 92, 184, 60};     ///< 「マイク」の欄
constexpr Rect kSettingsCard {28, 168, 576, 492};
constexpr Rect kTabsRect {50, 188, 532, 56};
constexpr double kInnerX = 50;                    ///< 左のカードの中身の左端
constexpr double kInnerRight = 582;               ///< 左のカードの中身の右端
constexpr double kInnerW = kInnerRight - kInnerX;
constexpr Rect kPipelineRect {50, 568, 532, 74};
constexpr Rect kVoiceCard {620, 92, 552, 568};
// ミックスのタブだけ列を広げる（28〜1172、間 16）。音声確認は 381、ミキサーは 747。かんたん・細かく調整は上の幅。
constexpr Rect kMixOutputSelect {28, 92, 547, 60};
constexpr Rect kMixMicSelect {591, 92, 184, 60};
constexpr Rect kMixSettingsCard {28, 168, 747, 492};
constexpr Rect kMixTabsRect {50, 188, 703, 56};
constexpr Rect kMixVoiceCard {791, 92, 381, 568};
constexpr double kMixInnerX = 50;
constexpr double kMixInnerRight = 753;
constexpr double kMixInnerW = kMixInnerRight - kMixInnerX;
// ---- アプリと更新 ----
constexpr Rect kAppCard {28, 92, 560, 568};
constexpr Rect kUpdateBox {50, 244, 516, 112};
constexpr Rect kRelatedCard {604, 92, 568, 568};
constexpr Rect kAppsModal {56, 56, 1088, 676};
constexpr Rect kAppsList {84, 148, 1032, 392};
constexpr Rect kConfirmCard {260, 150, 680, 476};
constexpr double kAppRowH = 84;    ///< 一覧のアプリの行の高さ
constexpr double kAppRowGap = 14;  ///< 一覧のアプリの行の間
// ---- 一番下の行（言語はアイコンとドロップダウン。部品の行は y 676〜732、注意書きはその下） ----
constexpr Rect kFooterRect {28, 674, 1144, 88};
// ---- 重ねた画面 ----
constexpr Rect kOutputModal {56, 56, 1088, 676};
constexpr Rect kOutputList {84, 148, 1032, 484};
constexpr Rect kMicModal {160, 140, 880, 440};
constexpr Rect kMicList {188, 240, 824, 312};
constexpr double kRowH = 76;        ///< 一覧の行の高さ
constexpr double kRowGap = 8;       ///< 一覧の行の間
constexpr double kRowButtonW = 196; ///< 行の右のボタン・札の幅

/**
 * 色を frame-ui の色にする（Canvas::text に渡す）。
 * @param c 色
 * @return frame-ui の色
 */
frame_ui::Rgb rgb(Color c) {
    return {c.r, c.g, c.b};
}

/**
 * 色を設定する。
 * @param cr cairo
 * @param c 色
 * @param alpha 不透明度
 */
void setColor(cairo_t* cr, Color c, double alpha = 1.0) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha);
}

/**
 * 角丸の四角を塗る。
 * @param cr cairo
 * @param r 矩形
 * @param radius 角の半径
 * @param c 色
 */
void fillRounded(cairo_t* cr, Rect r, double radius, Color c) {
    setColor(cr, c);
    frame_ui::roundedRect(cr, r, radius);
    cairo_fill(cr);
}

/**
 * 内側に収まる角丸の枠線を引く。
 * @param cr cairo
 * @param r 矩形
 * @param radius 角の半径
 * @param c 色
 * @param width 線の太さ
 * @param dashed 点線にするか
 */
void strokeRounded(cairo_t* cr, Rect r, double radius, Color c, double width, bool dashed = false) {
    setColor(cr, c);
    cairo_set_line_width(cr, width);
    if (dashed) {
        const double dash[] = {6, 4};
        cairo_set_dash(cr, dash, 2, 0);
    }
    frame_ui::roundedRect(cr, {r.x + width / 2, r.y + width / 2, r.w - width, r.h - width},
                          std::max(0.0, radius - width / 2));
    cairo_stroke(cr);
    cairo_set_dash(cr, nullptr, 0, 0);
}

/**
 * 塗りの丸。
 * @param cr cairo
 * @param cx 真ん中の x
 * @param cy 真ん中の y
 * @param r 半径
 * @param c 色
 */
void fillCircle(cairo_t* cr, double cx, double cy, double r, Color c) {
    setColor(cr, c);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_fill(cr);
}

/**
 * 描いたものを薄くする（使えない部品・前に使った出口）。alpha が 1 ならそのまま描く。
 * @param cr cairo
 * @param alpha 不透明度
 * @param draw 描く処理
 */
void withAlpha(cairo_t* cr, double alpha, const std::function<void()>& draw) {
    if (alpha >= 1.0) {
        draw();
        return;
    }
    cairo_push_group(cr);
    draw();
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, alpha);
}

/**
 * 2 つの矩形の重なり（一覧の見えている枠で、行のボタンの当たり判定を切る）。
 * @param a 矩形
 * @param b 矩形
 * @return 重なり（無ければ空）
 */
Rect intersect(Rect a, Rect b) {
    const double x0 = std::max(a.x, b.x);
    const double y0 = std::max(a.y, b.y);
    const double x1 = std::min(a.right(), b.right());
    const double y1 = std::min(a.bottom(), b.bottom());
    if (x1 - x0 < 8 || y1 - y0 < 8) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

// ---- 絵（見本の SVG と同じ線。viewBox の座標で描く） ----

/**
 * 絵を描く準備: (x, y) に一辺 size で置き、viewBox の 1 を size / box にする。線の太さは viewBox の単位。
 * @param cr cairo
 * @param x 左
 * @param y 上
 * @param size 一辺（px）
 * @param box viewBox の一辺
 * @param c 色
 * @param strokeWidth 線の太さ（viewBox の単位）
 */
void beginIcon(cairo_t* cr, double x, double y, double size, double box, Color c, double strokeWidth) {
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, size / box, size / box);
    setColor(cr, c);
    cairo_set_line_width(cr, strokeWidth);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_new_path(cr);
}

// assets/heroicons-language-16-solid.svg の d（実行時に取りに行かない。evenodd で塗る）。
// 実行時に取りに行かない。マイクは fontisto、切るときは famicons の mic-off。右は f7 のスピーカーと speaker-slash。
constexpr const char* kMicIconPath =
    "M8.4 16.8a4.805 4.805 0 0 0 4.8-4.8V4.8a4.8 4.8 0 1 0-9.6 0V12a4.805 4.805 0 0 0 4.8 4.8 "
    "M16.8 12V9.6a1.2 1.2 0 1 0-2.4 0V12a6 6 0 1 1-12 0V9.6a1.2 1.2 0 1 0-2.4 0V12a8.406 8.406 0 0 0 7.154 8.298l.046.006V21.6H3.6a1.2 1.2 0 1 0 0 2.4h9.6a1.2 1.2 0 1 0 0-2.4H9.6v-1.296c4.09-.609 7.193-4.093 7.2-8.303z";
constexpr const char* kMicOffIconPath =
    "M400 240v-31.55c0-8.61-6.62-16-15.23-16.43A16 16 0 0 0 368 208v32a111.7 111.7 0 0 1-2.68 24.38a2 2 0 0 0 .53 1.84l22.59 22.59a2 2 0 0 0 3.29-.72A143.3 143.3 0 0 0 400 240"
    "M256 352a112.36 112.36 0 0 1-112-112v-31.55c0-8.61-6.62-16-15.23-16.43A16 16 0 0 0 112 208v32c0 74 56.1 135.12 128 143.11V432h-47.55c-8.61 0-16 6.62-16.43 15.23A16 16 0 0 0 192 464h127.55c8.61 0 16-6.62 16.43-15.23A16 16 0 0 0 320 432h-48v-48.89a143.1 143.1 0 0 0 52-16.22a4 4 0 0 0 .91-6.35l-18.4-18.39a3 3 0 0 0-3.41-.58A111 111 0 0 1 256 352"
    "m1.14-304a79.66 79.66 0 0 0-68.47 36.57a4 4 0 0 0 .54 5L332.59 233a2 2 0 0 0 3.41-1.42V128.91C336 85 301 48.6 257.14 48"
    "M179.41 215a2 2 0 0 0-3.41 1.42V239a80.9 80.9 0 0 0 23.45 56.9a78.55 78.55 0 0 0 77.8 21.19a2 2 0 0 0 .86-3.35Z";
constexpr const char* kSpeakerIconPath =
    "M26 49.451c1.581 0 2.721-1.163 2.721-2.721V9.637c0-1.558-1.14-2.86-2.767-2.86c-1.14 0-1.907.511-3.14 1.674l-10.256 9.698a.9.9 0 0 1-.604.209H5.046C1.768 18.358 0 20.148 0 23.637v9.023c0 3.489 1.767 5.28 5.047 5.28h6.907c.232 0 .441.07.604.209l10.256 9.79c1.116 1.047 2.046 1.512 3.186 1.512m22.489-4.744c.906.604 2.023.372 2.674-.558c3.07-4.28 4.837-9.977 4.837-15.93c0-5.977-1.744-11.675-4.837-15.954c-.674-.907-1.768-1.14-2.674-.535c-.884.605-1.024 1.744-.326 2.744c2.535 3.721 4.093 8.605 4.093 13.744c0 5.14-1.512 10.07-4.117 13.745c-.65 1-.534 2.14.35 2.744m-9.28-6.28c.791.559 1.93.373 2.605-.534c1.814-2.442 2.907-6.024 2.907-9.675c0-3.65-1.116-7.209-2.907-9.697c-.674-.907-1.79-1.093-2.605-.535c-1.023.674-1.14 1.86-.395 2.883c1.349 1.814 2.163 4.582 2.163 7.35s-.86 5.534-2.186 7.371c-.698 1-.582 2.14.418 2.838";
constexpr const char* kSpeakerOffIconPath =
    "M39.18 33.402V8.207c0-1.57-1.149-2.883-2.79-2.883c-1.148 0-1.921.516-3.163 1.688l-9.61 9.093c-.14.141-.281.235-.445.282l-.938.07"
    "Zm7.593 16.57c.727.704 1.875.704 2.555 0c.703-.726.727-1.85 0-2.554L9.203 7.293a1.84 1.84 0 0 0-2.578 0c-.68.68-.68 1.875 0 2.555"
    "Zm-10.336-1.64c1.383 0 2.344-.82 2.649-2.203l-27.68-27.633c-.75.89-1.172 2.18-1.172 3.82v9.094c0 3.516 1.781 5.32 5.086 5.32h6.961c.235 0 .446.07.586.211l10.36 9.868c1.125 1.054 2.062 1.523 3.21 1.523";
constexpr const char* kAddIconPath =
    "M3 6.75A3.75 3.75 0 0 1 6.75 3h14.5A3.75 3.75 0 0 1 25 6.75v14.5A3.75 3.75 0 0 1 21.25 25H6.75A3.75 3.75 0 0 1 3 21.25zm11 .75a.75.75 0 0 0-.75.75v5h-5a.75.75 0 0 0 0 1.5h5v5a.75.75 0 0 0 1.5 0v-5h5a.75.75 0 0 0 0-1.5h-5v-5A.75.75 0 0 0 14 7.5";

constexpr const char* kLanguageIconPath =
    "M11 5a.75.75 0 0 1 .688.452l3.25 7.5a.75.75 0 1 1-1.376.596L12.89 12H9.109l-.67 1.548a.75.75 0 1 1-1.377-.596l3.25-7.5A.75.75 0 0 1 11 5m-1.24 5.5h2.48L11 7.636zM5 1a.75.75 0 0 1 .75.75v1.261a25 25 0 0 1 2.598.211a.75.75 0 1 1-.2 1.487q-.33-.045-.662-.08A13 13 0 0 1 5.92 8.058q.356.456.752.873a.75.75 0 0 1-1.086 1.035A13 13 0 0 1 5 9.307a13 13 0 0 1-2.841 2.546a.75.75 0 0 1-.827-1.251A11.6 11.6 0 0 0 4.08 8.057a13 13 0 0 1-.554-.938a.75.75 0 1 1 1.323-.707l.15.271c.388-.68.708-1.405.952-2.164a24 24 0 0 0-4.1.19a.75.75 0 0 1-.2-1.487q1.28-.171 2.598-.211V1.75A.75.75 0 0 1 5 1";

bool readSvgNumber(const char*& p, double& out) {
    while (*p == ' ' || *p == ',') ++p;
    if (*p == '\0') return false;
    char* end = nullptr;
    out = std::strtod(p, &end);
    if (end == p) return false;
    p = end;
    return true;
}

/**
 * SVG の円弧を 3 次ベジェにする（viewBox の単位。現在位置を更新する）。
 */
void svgArc(cairo_t* cr, double& cx, double& cy, double rx, double ry, double phiDeg, bool large, bool sweep, double x2, double y2) {
    if (rx == 0.0 || ry == 0.0 || (cx == x2 && cy == y2)) {
        cairo_line_to(cr, x2, y2);
        cx = x2;
        cy = y2;
        return;
    }
    const double phi = phiDeg * M_PI / 180.0;
    const double sinPhi = std::sin(phi);
    const double cosPhi = std::cos(phi);
    const double dx = (cx - x2) / 2.0;
    const double dy = (cy - y2) / 2.0;
    const double x1p = cosPhi * dx + sinPhi * dy;
    const double y1p = -sinPhi * dx + cosPhi * dy;
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        const double scale = std::sqrt(lambda);
        rx *= scale;
        ry *= scale;
    }
    const double rx2 = rx * rx;
    const double ry2 = ry * ry;
    const double num = rx2 * ry2 - rx2 * y1p * y1p - ry2 * x1p * x1p;
    const double den = rx2 * y1p * y1p + ry2 * x1p * x1p;
    double coef = den == 0.0 ? 0.0 : std::sqrt(std::max(0.0, num / den));
    if (large == sweep) coef = -coef;
    const double cxp = coef * rx * y1p / ry;
    const double cyp = coef * -ry * x1p / rx;
    const double centerX = cosPhi * cxp - sinPhi * cyp + (cx + x2) / 2.0;
    const double centerY = sinPhi * cxp + cosPhi * cyp + (cy + y2) / 2.0;
    const auto angleBetween = [](double ux, double uy, double vx, double vy) {
        const double dot = ux * vx + uy * vy;
        const double len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
        double ang = len == 0.0 ? 0.0 : std::acos(std::clamp(dot / len, -1.0, 1.0));
        if (ux * vy - uy * vx < 0.0) ang = -ang;
        return ang;
    };
    const double theta1 = angleBetween(1.0, 0.0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    double dtheta = angleBetween((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && dtheta > 0.0) dtheta -= 2.0 * M_PI;
    if (sweep && dtheta < 0.0) dtheta += 2.0 * M_PI;
    const int segments = std::max(1, static_cast<int>(std::ceil(std::fabs(dtheta) / (M_PI / 2.0))));
    const double delta = dtheta / segments;
    const double t = std::tan(delta / 4.0) * 4.0 / 3.0;
    double theta = theta1;
    const auto mapPoint = [&](double ux, double uy, double& ox, double& oy) {
        ox = centerX + rx * ux * cosPhi - ry * uy * sinPhi;
        oy = centerY + rx * ux * sinPhi + ry * uy * cosPhi;
    };
    for (int i = 0; i < segments; ++i) {
        const double cos1 = std::cos(theta);
        const double sin1 = std::sin(theta);
        theta += delta;
        const double cos2 = std::cos(theta);
        const double sin2 = std::sin(theta);
        double p1x = 0.0, p1y = 0.0, p2x = 0.0, p2y = 0.0, ex = 0.0, ey = 0.0;
        mapPoint(cos1 - t * sin1, sin1 + t * cos1, p1x, p1y);
        mapPoint(cos2 + t * sin2, sin2 - t * cos2, p2x, p2y);
        mapPoint(cos2, sin2, ex, ey);
        cairo_curve_to(cr, p1x, p1y, p2x, p2y, ex, ey);
    }
    cx = x2;
    cy = y2;
}

/**
 * SVG の path（M L H V C Q A Z と小文字）を cairo の道筋にする。
 */
void appendSvgPath(cairo_t* cr, const char* d) {
    const char* p = d;
    double cx = 0.0;
    double cy = 0.0;
    double sx = 0.0;
    double sy = 0.0;
    char command = 0;
    while (*p != '\0') {
        while (*p == ' ' || *p == ',') ++p;
        if (*p == '\0') break;
        if (std::isalpha(static_cast<unsigned char>(*p))) {
            command = *p++;
        } else if (command == 'M') {
            command = 'L';
        } else if (command == 'm') {
            command = 'l';
        } else if (command == 0) {
            break;
        }
        switch (command) {
            case 'M':
            case 'm': {
                double x = 0.0;
                double y = 0.0;
                if (!readSvgNumber(p, x) || !readSvgNumber(p, y)) return;
                if (command == 'm') {
                    x += cx;
                    y += cy;
                }
                cairo_move_to(cr, x, y);
                cx = sx = x;
                cy = sy = y;
                break;
            }
            case 'L':
            case 'l': {
                double x = 0.0;
                double y = 0.0;
                if (!readSvgNumber(p, x) || !readSvgNumber(p, y)) return;
                if (command == 'l') {
                    x += cx;
                    y += cy;
                }
                cairo_line_to(cr, x, y);
                cx = x;
                cy = y;
                break;
            }
            case 'H':
            case 'h': {
                double x = 0.0;
                if (!readSvgNumber(p, x)) return;
                if (command == 'h') x += cx;
                cairo_line_to(cr, x, cy);
                cx = x;
                break;
            }
            case 'V':
            case 'v': {
                double y = 0.0;
                if (!readSvgNumber(p, y)) return;
                if (command == 'v') y += cy;
                cairo_line_to(cr, cx, y);
                cy = y;
                break;
            }
            case 'C':
            case 'c': {
                double x1 = 0.0, y1 = 0.0, x2 = 0.0, y2 = 0.0, x = 0.0, y = 0.0;
                if (!readSvgNumber(p, x1) || !readSvgNumber(p, y1) || !readSvgNumber(p, x2) || !readSvgNumber(p, y2) ||
                    !readSvgNumber(p, x) || !readSvgNumber(p, y)) {
                    return;
                }
                if (command == 'c') {
                    x1 += cx;
                    y1 += cy;
                    x2 += cx;
                    y2 += cy;
                    x += cx;
                    y += cy;
                }
                cairo_curve_to(cr, x1, y1, x2, y2, x, y);
                cx = x;
                cy = y;
                break;
            }
            case 'Q':
            case 'q': {
                double x1 = 0.0, y1 = 0.0, x = 0.0, y = 0.0;
                if (!readSvgNumber(p, x1) || !readSvgNumber(p, y1) || !readSvgNumber(p, x) || !readSvgNumber(p, y)) return;
                if (command == 'q') {
                    x1 += cx;
                    y1 += cy;
                    x += cx;
                    y += cy;
                }
                const double c1x = cx + (2.0 / 3.0) * (x1 - cx);
                const double c1y = cy + (2.0 / 3.0) * (y1 - cy);
                const double c2x = x + (2.0 / 3.0) * (x1 - x);
                const double c2y = y + (2.0 / 3.0) * (y1 - y);
                cairo_curve_to(cr, c1x, c1y, c2x, c2y, x, y);
                cx = x;
                cy = y;
                break;
            }
            case 'A':
            case 'a': {
                double rx = 0.0, ry = 0.0, angle = 0.0, large = 0.0, sweep = 0.0, x = 0.0, y = 0.0;
                if (!readSvgNumber(p, rx) || !readSvgNumber(p, ry) || !readSvgNumber(p, angle) || !readSvgNumber(p, large) ||
                    !readSvgNumber(p, sweep) || !readSvgNumber(p, x) || !readSvgNumber(p, y)) {
                    return;
                }
                if (command == 'a') {
                    x += cx;
                    y += cy;
                }
                svgArc(cr, cx, cy, rx, ry, angle, large != 0.0, sweep != 0.0, x, y);
                break;
            }
            case 'Z':
            case 'z':
                cairo_close_path(cr);
                cx = sx;
                cy = sy;
                break;
            default: return;
        }
    }
}

/**
 * 言語の絵（heroicons language-16-solid。viewBox 16）。
 */
void drawLanguageIcon(cairo_t* cr, double x, double y, double size, Color c) {
    beginIcon(cr, x, y, size, 16, c, 1);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    appendSvgPath(cr, kLanguageIconPath);
    cairo_fill(cr);
    cairo_restore(cr);
}

/**
 * 塗りつぶしの絵。viewBox が正方形でなくても、size の中に収めて中央に置く。
 */
void drawFilledIcon(cairo_t* cr, double x, double y, double size, double viewW, double viewH, const char* d, Color c) {
    cairo_save(cr);
    const double scale = size / std::max(viewW, viewH);
    cairo_translate(cr, x + (size - viewW * scale) / 2.0, y + (size - viewH * scale) / 2.0);
    cairo_scale(cr, scale, scale);
    setColor(cr, c);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_new_path(cr);
    appendSvgPath(cr, d);
    cairo_fill(cr);
    cairo_restore(cr);
}

/** famicons mic-off の斜線（viewBox 512、線の太さ 32）。塗りとは別に描く。 */
void drawMicOffSlash(cairo_t* cr, double x, double y, double size, Color c) {
    cairo_save(cr);
    const double scale = size / 512.0;
    cairo_translate(cr, x, y);
    cairo_scale(cr, scale, scale);
    setColor(cr, c);
    cairo_set_line_width(cr, 32);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, 432, 400);
    cairo_line_to(cr, 96, 64);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/** ピークの色。下が緑、途中が黄、0 dBFS が赤。 */
Color levelColor(double fraction) {
    const Color green = hexColor(0x3dbe6a);
    const Color yellow = hexColor(0xe6c15a);
    const auto blend = [](Color a, Color b, double t) {
        return Color {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    };
    const double f = std::clamp(fraction, 0.0, 1.0);
    if (f < 0.62) return blend(green, yellow, f / 0.62);
    return blend(yellow, kDanger, (f - 0.62) / 0.38);
}

/**
 * スピーカーの絵（箱・コーンと音の波 2 本。viewBox 40）。
 * @param cr cairo
 * @param x 左
 * @param y 上
 * @param size 一辺（px）
 * @param c 色
 * @param strokeWidth 線の太さ（viewBox の単位）
 */
void drawSpeakerIcon(cairo_t* cr, double x, double y, double size, Color c, double strokeWidth = 3.4) {
    beginIcon(cr, x, y, size, 40, c, strokeWidth);
    cairo_move_to(cr, 6, 15);
    cairo_line_to(cr, 13, 15);
    cairo_line_to(cr, 22, 8);
    cairo_line_to(cr, 22, 32);
    cairo_line_to(cr, 13, 25);
    cairo_line_to(cr, 6, 25);
    cairo_close_path(cr);
    cairo_stroke(cr);
    cairo_move_to(cr, 28, 14);
    cairo_curve_to(cr, 30, 16, 31, 18, 31, 20);
    cairo_curve_to(cr, 31, 22, 30, 24, 28, 26);
    cairo_stroke(cr);
    cairo_move_to(cr, 32, 9);
    cairo_curve_to(cr, 35.5, 12, 37, 16, 37, 20);
    cairo_curve_to(cr, 37, 24, 35.5, 28, 32, 31);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/**
 * イヤホンの絵（左右のイヤーピースと、合わさるケーブル。viewBox 40）。
 * @param cr cairo
 * @param x 左
 * @param y 上
 * @param size 一辺（px）
 * @param c 色
 * @param strokeWidth 線の太さ（viewBox の単位）
 */
void drawEarphonesIcon(cairo_t* cr, double x, double y, double size, Color c, double strokeWidth = 3.4) {
    beginIcon(cr, x, y, size, 40, c, strokeWidth);
    cairo_new_sub_path(cr);
    cairo_arc(cr, 11, 12, 6, 0, 2 * M_PI);
    cairo_new_sub_path(cr);
    cairo_arc(cr, 29, 12, 6, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, 11, 18);
    cairo_line_to(cr, 11, 24);
    cairo_curve_to(cr, 11, 28, 15, 30, 20, 30);
    cairo_curve_to(cr, 25, 30, 29, 28, 29, 24);
    cairo_line_to(cr, 29, 18);
    cairo_move_to(cr, 20, 30);
    cairo_line_to(cr, 20, 36);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/**
 * 出口の種類の絵（スピーカーかイヤホン）。
 * @param cr cairo
 * @param speaker Frame のスピーカーなら true
 * @param x 左
 * @param y 上
 * @param size 一辺（px）
 * @param c 色
 * @param strokeWidth 線の太さ（viewBox の単位）
 */
void drawOutputIcon(cairo_t* cr, bool speaker, double x, double y, double size, Color c, double strokeWidth = 3.4) {
    if (speaker) {
        drawSpeakerIcon(cr, x, y, size, c, strokeWidth);
    } else {
        drawEarphonesIcon(cr, x, y, size, c, strokeWidth);
    }
}

/**
 * マイクの絵（viewBox 24）。muted なら斜線を入れる（ミュート中の印。色だけで伝えない）。
 * @param cr cairo
 * @param x 左
 * @param y 上
 * @param size 一辺（px）
 * @param c 色
 * @param muted 斜線を入れるか
 */
void drawMicIcon(cairo_t* cr, double x, double y, double size, Color c, bool muted) {
    beginIcon(cr, x, y, size, 24, c, muted ? 2.2 : 2.0);
    const double top = muted ? 3 : 2;
    frame_ui::roundedRect(cr, {9, top, 6, 11}, 3);
    cairo_stroke(cr);
    cairo_new_sub_path(cr);
    cairo_arc_negative(cr, 12, top + 8, 7, M_PI, 0);
    cairo_stroke(cr);
    cairo_move_to(cr, 12, top + 15);
    cairo_line_to(cr, 12, muted ? 21 : 22);
    cairo_stroke(cr);
    if (muted) {
        cairo_move_to(cr, 4, 4);
        cairo_line_to(cr, 20, 20);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

/**
 * 4 つの四角の絵（「アプリと更新」。viewBox 20）。
 * @param cr cairo
 * @param x 左
 * @param y 上
 * @param size 一辺（px）
 * @param c 色
 */
void drawGridIcon(cairo_t* cr, double x, double y, double size, Color c) {
    beginIcon(cr, x, y, size, 20, c, 1);
    for (const double gx : {2.0, 11.0}) {
        for (const double gy : {2.0, 11.0}) frame_ui::roundedRect(cr, {gx, gy, 7, 7}, 2);
    }
    cairo_fill(cr);
    cairo_restore(cr);
}

/**
 * 線の折れ線（山かっこ・✕・入れ替えの矢印など）を描く。
 * @param cr cairo
 * @param x 左
 * @param y 上
 * @param size 一辺（px）
 * @param box viewBox の一辺
 * @param c 色
 * @param strokeWidth 線の太さ（viewBox の単位）
 * @param points 点（x, y の並び）。NAN の組で線を切る
 */
void drawPolyline(cairo_t* cr, double x, double y, double size, double box, Color c, double strokeWidth,
                  std::initializer_list<double> points) {
    beginIcon(cr, x, y, size, box, c, strokeWidth);
    bool start = true;
    for (auto it = points.begin(); it != points.end() && it + 1 != points.end(); it += 2) {
        if (std::isnan(*it)) {
            start = true;
            continue;
        }
        if (start) {
            cairo_move_to(cr, *it, *(it + 1));
        } else {
            cairo_line_to(cr, *it, *(it + 1));
        }
        start = false;
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

/**
 * ▶（再生）を描く。
 * @param cr cairo
 * @param cx 真ん中の x
 * @param cy 真ん中の y
 * @param s 大きさ（px）
 * @param c 色
 */
void drawPlayIcon(cairo_t* cr, double cx, double cy, double s, Color c) {
    setColor(cr, c);
    cairo_move_to(cr, cx - s * 0.32, cy - s * 0.42);
    cairo_line_to(cr, cx + s * 0.44, cy);
    cairo_line_to(cr, cx - s * 0.32, cy + s * 0.42);
    cairo_close_path(cr);
    cairo_fill(cr);
}

/**
 * ■（停止）を描く。
 * @param cr cairo
 * @param cx 真ん中の x
 * @param cy 真ん中の y
 * @param s 一辺（px）
 * @param c 色
 */
void drawStopIcon(cairo_t* cr, double cx, double cy, double s, Color c) {
    fillRounded(cr, {cx - s / 2, cy - s / 2, s, s}, s * 0.15, c);
}

// ---- 文字 ----

/**
 * 文言の %s を 1 つ埋める（printf の書式。表の文言は自分の表のものだけ）。
 * @param format 書式
 * @param arg 入れる文字列
 * @return 文字列
 */
std::string format1(const char* format, const std::string& arg) {
    char text[640];
    std::snprintf(text, sizeof(text), format, arg.c_str());
    return text;
}

/**
 * 文言の %1$s・%2$s を埋める（言語で順番が違ってよい）。
 * @param format 書式
 * @param a 1 つ目
 * @param b 2 つ目
 * @return 文字列
 */
std::string format2(const char* format, const std::string& a, const std::string& b) {
    char text[640];
    std::snprintf(text, sizeof(text), format, a.c_str(), b.c_str());
    return text;
}

/**
 * 文言の %d を埋める。
 * @param format 書式
 * @param n 数
 * @return 文字列
 */
std::string formatInt(const char* format, int n) {
    char text[160];
    std::snprintf(text, sizeof(text), format, n);
    return text;
}

/**
 * 行の頭に来てはいけない記号か（禁則。前の行に残す）。
 * @param token 1 文字ぶん
 * @return そうなら true
 */
bool closesLine(const std::string& token) {
    static const char* const kClosing[] = {"、", "。", "，", "．", "）", "」", "』", "】", "・", "…", "：", "；", "！", "？",
                                          ",", ".", ")", ":", ";", "!", "?"};
    for (const char* c : kClosing) {
        if (token.rfind(c, 0) == 0) return true;
    }
    return false;
}

/**
 * 幅に収まるよう行に分ける（英語は語の切れ目、日本語・中国語は文字ごと。句読点は行頭に来ないようにする）。
 * 入りきらない分は最後の行の末尾を「…」にする。
 * @param ui 描く先（文字を測る）
 * @param text 文字列
 * @param size 大きさ
 * @param bold 太字か
 * @param width 幅
 * @param maxLines 行の上限
 * @return 行
 */
std::vector<std::string> wrapText(const frame_ui::Canvas& ui, const std::string& text, double size, bool bold,
                                  double width, int maxLines) {
    // 区切り: 英数字の続き（後ろの空白ごと）か、それ以外の 1 文字
    std::vector<std::string> tokens;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t end = i + 1;
        if (c < 0x80 && c != ' ') {
            while (end < text.size() && static_cast<unsigned char>(text[end]) < 0x80 && text[end] != ' ') ++end;
        } else if (c >= 0x80) {
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
        }
        while (end < text.size() && text[end] == ' ') ++end;  // 後ろの空白はその語に付ける
        tokens.push_back(text.substr(i, end - i));
        i = end;
    }
    const auto trimRight = [](std::string s) {
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    };
    std::vector<std::string> lines;
    std::string line;
    for (const std::string& token : tokens) {
        if (line.empty()) {
            line = token;
        } else if (ui.measure(trimRight(line + token), size, bold) <= width || closesLine(token)) {
            line += token;
        } else {
            lines.push_back(trimRight(line));
            line = token;
        }
    }
    if (!line.empty()) lines.push_back(trimRight(line));
    if (static_cast<int>(lines.size()) > maxLines) {
        std::string rest;
        for (size_t i = maxLines - 1; i < lines.size(); ++i) rest += lines[i] + (i + 1 < lines.size() ? " " : "");
        lines.resize(maxLines);
        lines.back() = ui.ellipsize(rest, size, width, bold);
    }
    for (std::string& l : lines) l = ui.ellipsize(l, size, width, bold);
    return lines;
}

/**
 * 行に分けて描く。
 * @param ui 描く先
 * @param x 左
 * @param firstBaseline 1 行目のベースライン
 * @param lineH 行の高さ
 * @param lines 行
 * @param size 大きさ
 * @param c 色
 * @param bold 太字か
 */
void drawLines(const frame_ui::Canvas& ui, double x, double firstBaseline, double lineH,
               const std::vector<std::string>& lines, double size, Color c, bool bold = false) {
    for (size_t i = 0; i < lines.size(); ++i) ui.text(x, firstBaseline + lineH * i, lines[i], size, rgb(c), bold);
}

/**
 * 録った時刻を「00:41」の形にする。
 * @param t 時刻
 * @return 文字列
 */
std::string clockText(std::time_t t) {
    std::tm tm {};
    localtime_r(&t, &tm);
    char text[16];
    std::strftime(text, sizeof(text), "%H:%M", &tm);
    return text;
}

/**
 * 「2026-10-08」を「10/8」にする。
 * @param date YYYY-MM-DD
 * @return M/D（読めなければ空）
 */
std::string monthDay(const std::string& date) {
    int year = 0;
    int month = 0;
    int day = 0;
    if (std::sscanf(date.c_str(), "%d-%d-%d", &year, &month, &day) != 3 || month < 1 || day < 1) return "";
    return std::to_string(month) + "/" + std::to_string(day);
}

/**
 * 設定のまとめ（「エコー除去オン・ノイズ除去オン」）。
 * @param echo エコー除去
 * @param ns ノイズ除去
 * @param t 言語の表
 * @return 文字列
 */
std::string summaryText(bool echo, bool ns, const UiText& t) {
    return std::string(echo ? t.sumEchoOn : t.sumEchoOff) + (ns ? t.sumNsOn : "");
}

/** 音の出口の一覧の 1 行（つながっている出口と、覚えているがつながっていない出口）。 */
struct OutputEntry {
    std::string key;
    std::string name;        ///< 表示名
    bool connected = false;
    bool active = false;     ///< 今の出口
    OutputProfile profile;   ///< 覚えている設定（無ければ初めての設定）
};

/**
 * 音の出口の一覧を作る（つながっているもの → 前に使ったもの（最後に使った日の新しい順））。
 * @param config 設定（出口ごとの設定）
 * @param devices 今の一覧
 * @param activeKey 今の出口
 * @param t 言語の表
 * @return 一覧
 */
std::vector<OutputEntry> outputEntries(const Config& config, const AudioDevices& devices, const std::string& activeKey,
                                       const UiText& t) {
    std::vector<OutputEntry> entries;
    const auto profileOf = [&](const std::string& key, const std::string& name) {
        const auto it = config.outputs.find(key);
        return it != config.outputs.end() ? it->second : firstProfile(key, name);
    };
    for (const AudioEndpoint& e : devices.outputs) {
        entries.push_back({e.key, outputDisplayName(e.key, e.name, t), true, e.key == activeKey, profileOf(e.key, e.name)});
    }
    if (!activeKey.empty() && devices.findOutput(activeKey) == nullptr) {
        // 一覧をまだ読めていないとき: 今の出口だけは、つながっている出口として出す
        const OutputProfile profile = profileOf(activeKey, "");
        entries.insert(entries.begin(), {activeKey, outputDisplayName(activeKey, profile.name, t), true, true, profile});
    }
    std::vector<OutputEntry> past;
    for (const auto& entry : config.outputs) {
        const bool listed = std::any_of(entries.begin(), entries.end(), [&](const OutputEntry& e) { return e.key == entry.first; });
        if (listed) continue;
        past.push_back({entry.first, outputDisplayName(entry.first, entry.second.name, t), false, false, entry.second});
    }
    std::stable_sort(past.begin(), past.end(),
                     [](const OutputEntry& a, const OutputEntry& b) { return a.profile.lastUsed > b.profile.lastUsed; });
    entries.insert(entries.end(), past.begin(), past.end());
    return entries;
}

std::string foldKey(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string trimEdge(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.pop_back();
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    return text.substr(i);
}

std::string fileBase(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool regularFile(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/** データディレクトリ（アイコンと .desktop）。Flatpak の書き出しも含む。 */
std::vector<std::string> dataRoots() {
    std::vector<std::string> roots;
    const char* home = std::getenv("HOME");
    const std::string local = (home != nullptr && home[0] != '\0') ? std::string(home) + "/.local/share" : std::string();
    const char* xdgHome = std::getenv("XDG_DATA_HOME");
    if (xdgHome != nullptr && xdgHome[0] != '\0') roots.push_back(xdgHome);
    else if (!local.empty()) roots.push_back(local);
    if (!local.empty()) roots.push_back(local + "/flatpak/exports/share");
    roots.push_back("/var/lib/flatpak/exports/share");
    const char* xdg = std::getenv("XDG_DATA_DIRS");
    const std::string dirs = (xdg != nullptr && xdg[0] != '\0') ? xdg : "/usr/local/share:/usr/share";
    size_t pos = 0;
    while (pos <= dirs.size()) {
        const size_t colon = dirs.find(':', pos);
        const std::string part = dirs.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
        if (!part.empty()) roots.push_back(part);
        if (colon == std::string::npos) break;
        pos = colon + 1;
    }
    return roots;
}

std::string appstreamPng(const std::string& name);

std::string resolveIconPng(const std::string& icon) {
    if (icon.empty() || icon.find("..") != std::string::npos) return {};
    if (icon[0] == '/') {
        if (icon.size() >= 4 && icon.compare(icon.size() - 4, 4, ".png") == 0 && regularFile(icon)) return icon;
        if (regularFile(icon + ".png")) return icon + ".png";
        return {};
    }
    std::string name = icon;
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".png") == 0) name.resize(name.size() - 4);
    if (name.empty() || name.find('/') != std::string::npos) return {};
    const char* sizes[] = {"128x128", "64x64", "48x48", "256x256", "512x512"};
    for (const std::string& root : dataRoots()) {
        for (const char* size : sizes) {
            const std::string path = root + "/icons/hicolor/" + size + "/apps/" + name + ".png";
            if (regularFile(path)) return path;
        }
        const std::string pixmap = root + "/pixmaps/" + name + ".png";
        if (regularFile(pixmap)) return pixmap;
    }
    // Flatpak の書き出しは SVG だけのことがある。PNG は appstream の Cache にある。
    const std::string cached = appstreamPng(name);
    if (!cached.empty()) return cached;
    return {};
}

/** appstream の PNG を一度集め、Icon= の名前で引く。 */
std::string appstreamPng(const std::string& name) {
    static std::map<std::string, std::string> index;
    static bool built = false;
    if (!built) {
        built = true;
        std::vector<std::string> roots = {"/var/lib/flatpak/appstream"};
        const char* home = std::getenv("HOME");
        if (home != nullptr && home[0] != '\0') roots.push_back(std::string(home) + "/.local/share/flatpak/appstream");
        const char* sizes[] = {"128x128", "64x64", "48x48"};
        const auto takeDir = [&](const std::string& dir) {
            DIR* icons = ::opendir(dir.c_str());
            if (icons == nullptr) return;
            while (const dirent* ent = ::readdir(icons)) {
                const std::string file = ent->d_name;
                if (file.size() < 5 || file.compare(file.size() - 4, 4, ".png") != 0) continue;
                const std::string key = file.substr(0, file.size() - 4);
                if (index.find(key) == index.end()) index.emplace(key, dir + "/" + file);
            }
            ::closedir(icons);
        };
        for (const std::string& root : roots) {
            DIR* remotes = ::opendir(root.c_str());
            if (remotes == nullptr) continue;
            while (const dirent* remote = ::readdir(remotes)) {
                if (remote->d_name[0] == '.') continue;
                DIR* arches = ::opendir((root + "/" + remote->d_name).c_str());
                if (arches == nullptr) continue;
                while (const dirent* arch = ::readdir(arches)) {
                    if (arch->d_name[0] == '.') continue;
                    DIR* hashes = ::opendir((root + "/" + remote->d_name + "/" + arch->d_name).c_str());
                    if (hashes == nullptr) continue;
                    while (const dirent* hash = ::readdir(hashes)) {
                        if (hash->d_name[0] == '.') continue;
                        const std::string icons = root + "/" + remote->d_name + "/" + arch->d_name + "/" + hash->d_name + "/icons";
                        for (const char* size : sizes) takeDir(icons + "/" + size);
                    }
                    ::closedir(hashes);
                }
                ::closedir(arches);
            }
            ::closedir(remotes);
        }
    }
    const auto it = index.find(name);
    return it == index.end() ? std::string() : it->second;
}

/**
 * 一度だけ .desktop を読み、名前・ファイル名・実行ファイルから Icon= を覚える。
 * 描き直しのたびにディスクを見ない。
 */
const std::map<std::string, std::string>& desktopIcons() {
    static std::map<std::string, std::string> index;
    static bool built = false;
    if (built) return index;
    built = true;
    const auto remember = [&](const std::string& key, const std::string& icon) {
        if (key.empty() || icon.empty()) return;
        const std::string fold = foldKey(key);
        if (index.find(fold) == index.end()) index.emplace(fold, icon);
    };
    for (const std::string& root : dataRoots()) {
        DIR* dir = ::opendir((root + "/applications").c_str());
        if (dir == nullptr) continue;
        while (const dirent* ent = ::readdir(dir)) {
            const std::string file = ent->d_name;
            if (file.size() < 9 || file.compare(file.size() - 8, 8, ".desktop") != 0) continue;
            std::ifstream in(root + "/applications/" + file);
            if (!in) continue;
            std::string name;
            std::string execLine;
            std::string icon;
            std::string line;
            bool seenGroup = false;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty() && line[0] == '[') {
                    if (seenGroup) break;
                    seenGroup = true;
                    continue;
                }
                if (line.rfind("Name=", 0) == 0) name = trimEdge(line.substr(5));
                else if (line.rfind("Icon=", 0) == 0) icon = trimEdge(line.substr(5));
                else if (line.rfind("Exec=", 0) == 0 && execLine.empty()) execLine = trimEdge(line.substr(5));
            }
            std::string execBase = execLine;
            if (!execBase.empty() && execBase[0] == '"') {
                const size_t end = execBase.find('"', 1);
                execBase = execBase.substr(1, end == std::string::npos ? std::string::npos : end - 1);
            } else {
                const size_t space = execBase.find(' ');
                if (space != std::string::npos) execBase.resize(space);
            }
            execBase = fileBase(execBase);
            remember(file.substr(0, file.size() - 8), icon);
            remember(name, icon);
            if (execBase != "flatpak" && execBase != "env" && execBase != "bwrap") remember(execBase, icon);
            // 一覧の名前はプロセス名（Zen）。Flatpak の Id の最後（app.zen_browser.zen）と合わせる。
            if (execLine.find("flatpak") != std::string::npos) {
                std::string token;
                const auto take = [&]() {
                    if (token.size() > 2 && token[0] != '-' && token.find('.') != std::string::npos &&
                        token.find('/') == std::string::npos && token.find('%') == std::string::npos) {
                        remember(token, icon);
                        const size_t dot = token.rfind('.');
                        if (dot != std::string::npos) remember(token.substr(dot + 1), icon);
                    }
                    token.clear();
                };
                for (char c : execLine) {
                    if (c == ' ' || c == '\t') take();
                    else token.push_back(c);
                }
                take();
            }
        }
        ::closedir(dir);
    }
    return index;
}

/**
 * 実行中のアプリの PNG（.desktop の Icon=）。無ければ nullptr。結果は名前ごとに覚えておく。
 */
cairo_surface_t* runningAppIcon(const std::string& name) {
    static std::map<std::string, cairo_surface_t*> cache;
    const auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    cairo_surface_t* surface = nullptr;
    const auto& index = desktopIcons();
    const auto icon = index.find(foldKey(name));
    if (icon != index.end()) {
        const std::string path = resolveIconPng(icon->second);
        if (!path.empty()) {
            cairo_surface_t* loaded = cairo_image_surface_create_from_png(path.c_str());
            if (loaded != nullptr && cairo_surface_status(loaded) == CAIRO_STATUS_SUCCESS &&
                cairo_image_surface_get_width(loaded) > 0) {
                surface = loaded;
            } else if (loaded != nullptr) {
                cairo_surface_destroy(loaded);
            }
        }
    }
    cache.emplace(name, surface);
    return surface;
}

void paintAppIcon(cairo_t* cr, cairo_surface_t* icon, double x, double y, double size) {
    const double w = cairo_image_surface_get_width(icon);
    const double h = cairo_image_surface_get_height(icon);
    if (w < 1 || h < 1) return;
    cairo_save(cr);
    frame_ui::roundedRect(cr, {x, y, size, size}, 8);
    cairo_clip(cr);
    cairo_translate(cr, x, y);
    cairo_scale(cr, size / w, size / h);
    cairo_set_source_surface(cr, icon, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

}  // namespace

std::string errorText(MicError error, const UiText& text) {
    switch (error) {
        case MicError::None: break;
        case MicError::ReadSettings: return text.errReadSettings;
        case MicError::NotInstalled: return text.errNotInstalled;
        case MicError::ReadLinks: return text.errReadLinks;
        case MicError::WriteSettings: return text.errWriteSettings;
        case MicError::WriteAutostart: return text.errWriteAutostart;
        case MicError::WriteNsParams: return text.errWriteNsParams;
        case MicError::WriteEcho: return text.errWriteEcho;
        case MicError::WriteNs: return text.errWriteNs;
        case MicError::WriteMute: return text.errWriteMute;
        case MicError::WriteMuteOn: return text.errWriteMuteOn;
        case MicError::WriteOutput: return text.errWriteOutput;
        case MicError::WriteInput: return text.errWriteInput;
        case MicError::WriteMix: return text.errWriteMix;
    }
    return "";
}

frame_ui::Lang uiLang(Language language) {
    // 更新の帯は frame-ui（日本語・英語・簡体字中国語）。それ以外は英語に戻す。
    if (language == Language::Ja) return frame_ui::Lang::Ja;
    if (language == Language::Sc) return frame_ui::Lang::Sc;
    return frame_ui::Lang::En;
}

std::string outputDisplayName(const std::string& key, const std::string& name, const UiText& t) {
    if (isBuiltinSpeaker(key)) return t.speakerName;
    return name.empty() ? key : name;
}

namespace {

/**
 * サムネイル（マイクの絵と「Mic」）を描く（renderThumbnail と、アプリと更新の画面のアイコンで共通）。
 * @param cr cairo（一辺 size の画像）
 * @param fonts フォント
 * @param size 一辺
 */
void drawThumbnail(cairo_t* cr, const FontSet& fonts, int size) {
    const Pen pen {cr, &fonts};
    const double s = size / 256.0;

    pen.color(kBg);
    pen.roundedRect(8 * s, 8 * s, 240 * s, 240 * s, 48 * s);
    cairo_fill(cr);
    // 内側の縁（明るい 1px）
    cairo_set_source_rgba(cr, 1, 1, 1, 0.08);
    cairo_set_line_width(cr, 2 * s);
    pen.roundedRect(9 * s, 9 * s, 238 * s, 238 * s, 47 * s);
    cairo_stroke(cr);

    // マイクの頭（縦長のカプセル、イメージカラー）と、その光彩
    for (int i = 3; i >= 1; --i) {
        pen.color(kAccent, 0.07);
        pen.roundedRect((100 - i * 5) * s, (30 - i * 5) * s, (56 + i * 10) * s, (96 + i * 10) * s, (28 + i * 5) * s);
        cairo_fill(cr);
    }
    pen.color(kAccent);
    pen.roundedRect(100 * s, 30 * s, 56 * s, 96 * s, 28 * s);
    cairo_fill(cr);
    // 頭の網目（横線 3 本）
    pen.color(kBg, 0.55);
    cairo_set_line_width(cr, 5 * s);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 3; ++i) {
        const double y = (62 + i * 16) * s;
        cairo_move_to(cr, 114 * s, y);
        cairo_line_to(cr, 142 * s, y);
    }
    cairo_stroke(cr);
    // 受け（U 字）・柄・台
    pen.color(kText);
    cairo_set_line_width(cr, 10 * s);
    cairo_arc(cr, 128 * s, 94 * s, 48 * s, 0, M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, 128 * s, 142 * s);
    cairo_line_to(cr, 128 * s, 170 * s);
    cairo_move_to(cr, 100 * s, 172 * s);
    cairo_line_to(cr, 156 * s, 172 * s);
    cairo_stroke(cr);

    const double w = pen.measure("Mic", 56 * s, true);
    pen.text((size - w) / 2, 230 * s, "Mic", 56 * s, kText, true);
}

}  // namespace

MicPanel::MicPanel(const FontSet& fonts) : fonts_(fonts) {
    surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kWidth, kHeight);
    cr_ = cairo_create(surface_);
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_set_font_options(cr_, options);
    cairo_font_options_destroy(options);
    // アプリと更新の画面のアイコン（サムネイルと同じ絵を 1 回だけ描いておく）
    appIcon_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 256, 256);
    cairo_t* iconCr = cairo_create(appIcon_);
    drawThumbnail(iconCr, fonts_, 256);
    cairo_destroy(iconCr);
}

MicPanel::~MicPanel() {
    cairo_surface_destroy(appIcon_);
    cairo_destroy(cr_);
    cairo_surface_destroy(surface_);
}

void MicPanel::addButton(const PanelHit& hit, Rect rect, bool inList) {
    if (rect.w <= 0 || rect.h <= 0) return;
    buttons_.push_back({hit, rect, inList});
}

int MicPanel::look(Rect rect) const {
    // 重ねた画面を出している間は、後ろを描くあいだ pointer_.inside を外しておく（render）ので、後ろは乗っている見た目にならない
    if (!pointer_.inside || listDragging_ || !rect.contains(pointer_.x, pointer_.y)) return 0;
    return pointer_.down ? 2 : 1;
}

PanelHit MicPanel::hitTest(double x, double y, bool* inList) const {
    for (auto it = buttons_.rbegin(); it != buttons_.rend(); ++it) {
        if (!it->rect.contains(x, y)) continue;
        if (inList != nullptr) *inList = it->inList;
        return it->hit;
    }
    if (inList != nullptr) *inList = false;
    return {};
}

double MicPanel::trackValue(const Track& track, double x, double y) {
    const double span = track.vertical ? (track.y1 - track.y0) : (track.x1 - track.x0);
    const double pos = track.vertical ? (track.y1 - y) : (x - track.x0);
    const double fraction = std::clamp(pos / std::max(1.0, span), 0.0, 1.0);
    double value = track.min + (track.max - track.min) * fraction;
    // 縦のミックスバーだけ、100% の前後 4 ポイントで吸い付く
    if (track.vertical && std::fabs(value - 100.0) <= 4.0) value = 100.0;
    if (track.action == PanelAction::NsVadSlider) return clampNsVad(value);
    if (track.action == PanelAction::NsGraceSlider) return clampNsGrace(value);
    return clampMixLinear(value) * 100.0;
}

void MicPanel::setMixPeaks(std::vector<std::pair<std::string, float>> peaks) {
    mixPeaks_ = std::move(peaks);
}

float MicPanel::mixPeak(const std::string& key) const {
    for (const auto& item : mixPeaks_) {
        if (item.first == key) return item.second;
    }
    return 0.0f;
}

void MicPanel::applyLocal(const PanelHit& hit) {
    switch (hit.action) {
        case PanelAction::ShowApps:
            view_ = PanelView::Apps;
            overlay_ = PanelOverlay::None;
            break;
        case PanelAction::ShowSettings: view_ = PanelView::Settings; break;
        case PanelAction::OpenOutputPicker:
            overlay_ = PanelOverlay::OutputPicker;
            listScroll_ = 0;
            break;
        case PanelAction::OpenMicPicker:
            overlay_ = PanelOverlay::MicPicker;
            listScroll_ = 0;
            break;
        case PanelAction::CloseOverlay: overlay_ = PanelOverlay::None; break;
        case PanelAction::OutputView:
            viewKey_ = hit.key;
            overlay_ = PanelOverlay::None;
            break;
        case PanelAction::OutputUse: viewKey_ = hit.key; break;  // 一覧は開いたまま（「いま使用中」に変わるのを見せる）
        case PanelAction::OutputForget:
            if (viewKey_ == hit.key) viewKey_.clear();
            break;
        case PanelAction::MicUse: overlay_ = PanelOverlay::None; break;
        case PanelAction::AppsOpenList:
            overlay_ = PanelOverlay::AppsList;
            listScroll_ = 0;
            launchResult_ = frame_apps::LaunchResult::Started;
            break;
        case PanelAction::AppInstall:
        case PanelAction::AppsOpenMenu:
            // まず確認（実行するコマンドを見せる）。開くのは確認の「Konsole で開く」
            confirmBack_ = overlay_ == PanelOverlay::AppsList ? PanelOverlay::AppsList : PanelOverlay::None;
            confirmKey_ = hit.action == PanelAction::AppInstall ? hit.key : std::string();
            overlay_ = PanelOverlay::AppsConfirm;
            launchResult_ = frame_apps::LaunchResult::Started;
            break;
        case PanelAction::AppsConfirmCancel:
            overlay_ = confirmBack_;
            launchResult_ = frame_apps::LaunchResult::Started;
            break;
        case PanelAction::MixAdd:
            overlay_ = PanelOverlay::MixSuggest;
            listScroll_ = 0;
            nameError_.clear();
            break;
        default: break;
    }
}

void MicPanel::setLaunchResult(frame_apps::LaunchResult result) {
    launchResult_ = result;
    if (result == frame_apps::LaunchResult::Started && overlay_ == PanelOverlay::AppsConfirm) overlay_ = confirmBack_;
}

void MicPanel::openConfirmForPreview(const std::string& key, bool fromList) {
    view_ = PanelView::Apps;
    confirmKey_ = key;
    confirmBack_ = fromList ? PanelOverlay::AppsList : PanelOverlay::None;
    overlay_ = PanelOverlay::AppsConfirm;
}

bool MicPanel::pointerMove(double x, double y) {
    pointer_.x = x;
    pointer_.y = y;
    pointer_.inside = true;
    if (dragAction_ != PanelAction::None) {
        // ドラッグ中は、溝から外れても軸の位置で値を決める（横バーは x、縦バーは y）
        for (const Track& track : tracks_) {
            if (track.action != dragAction_ || track.index != dragIndex_) continue;
            const double value = trackValue(track, x, y);
            if (value == dragValue_) return false;
            dragValue_ = value;
            return true;
        }
        return false;
    }
    if (listTracking_) {
        // 一覧を押したまま 8px 以上動かしたら、押した行は取り消してスクロールにする
        const double moved = listHorizontal_ ? (x - listStartX_) : (y - listStartY_);
        if (!listDragging_ && std::fabs(moved) > 8) {
            listDragging_ = true;
            listPress_ = {};
        }
        if (!listDragging_) return false;
        const double origin = listHorizontal_ ? listStartX_ : listStartY_;
        const double pointer = listHorizontal_ ? x : y;
        const double next = std::clamp(listStartScroll_ + (origin - pointer), 0.0, listMaxScroll_);
        if (next == listScroll_) return false;
        listScroll_ = next;
        return true;
    }
    const PanelHit now = hitTest(x, y);
    if (now == hover_) return false;
    hover_ = now;
    return true;
}

PanelHit MicPanel::pointerDown(double x, double y, double now) {
    pointer_ = {x, y, true, true};
    bool inList = false;
    PanelHit hit = hitTest(x, y, &inList);
    hover_ = hit;
    // 言語の一覧: 選ぶか、もう一度押すか、隣を押すと閉じる。隣を押したときはそのボタンは動かさない
    if (languageOpen_ || hit.action == PanelAction::LanguageMenu) {
        quitArmed_ = false;
        updateArmed_ = false;
    }
    if (languageOpen_) {
        const bool pick = hit.action == PanelAction::LanguagePick;
        languageOpen_ = false;
        if (!pick) return {};
        applyLocal(hit);
        return hit;
    }
    if (hit.action == PanelAction::LanguageMenu) {
        languageOpen_ = true;
        return {};
    }
    if (inList || (overlay_ != PanelOverlay::None && listRect_.contains(x, y) && hit.action == PanelAction::None)) {
        // 一覧の中: 離したときに返す（そのまま動かせばスクロール）
        listTracking_ = true;
        listDragging_ = false;
        listPress_ = hit;
        listStartX_ = x;
        listStartY_ = y;
        listStartScroll_ = listScroll_;
        return {};
    }
    const bool slider = hit.action == PanelAction::NsVadSlider || hit.action == PanelAction::NsGraceSlider ||
                        hit.action == PanelAction::MixMicSlider || hit.action == PanelAction::MixToMicSlider ||
                        hit.action == PanelAction::MixToPhonesSlider;
    if (slider) {
        // バーは押したところへ飛び、そのままドラッグ
        for (const Track& track : tracks_) {
            if (track.action != hit.action || track.index != hit.index) continue;
            dragAction_ = track.action;
            dragIndex_ = hit.index;
            dragValue_ = trackValue(track, x, y);
        }
    }
    if (hit.action == PanelAction::MixDelete) {
        if (deleteArmed_ == hit.key && now <= deleteArmedUntil_) {
            deleteArmed_.clear();
        } else {
            deleteArmed_ = hit.key;
            deleteArmedUntil_ = now + kQuitConfirmSec;
            return {};
        }
    } else if (hit.action != PanelAction::None) {
        deleteArmed_.clear();
    }
    if (hit.action == PanelAction::Quit) {
        // 誤って押しても終わらないよう、1 回目は確認の表示にするだけ
        updateArmed_ = false;
        if (quitArmed_ && now <= quitArmedUntil_) return hit;
        quitArmed_ = true;
        quitArmedUntil_ = now + kQuitConfirmSec;
        return {};
    }
    quitArmed_ = false;  // 別のボタンを押したら確認は取り消す
    if (hit.action == PanelAction::UpdateInstall) {
        // 1 回目は確認の表示（やめる・更新する）にするだけ
        updateArmed_ = true;
        updateArmedUntil_ = now + kQuitConfirmSec;
        return {};
    }
    const bool confirmed = hit.action == PanelAction::UpdateConfirmYes && updateArmed_;
    updateArmed_ = false;
    if (hit.action == PanelAction::UpdateConfirmNo) return {};
    if (hit.action == PanelAction::UpdateConfirmYes && !confirmed) return {};
    applyLocal(hit);
    return hit;
}

PanelHit MicPanel::pointerUp() {
    pointer_.down = false;
    dragAction_ = PanelAction::None;
    PanelHit result;
    if (listTracking_) {
        if (!listDragging_ && listPress_.action != PanelAction::None && hitTest(pointer_.x, pointer_.y) == listPress_) {
            result = listPress_;
        }
        if (result.action == PanelAction::MixDelete) {
            if (deleteArmed_ == result.key && now_ <= deleteArmedUntil_) {
                deleteArmed_.clear();
            } else {
                deleteArmed_ = result.key;
                deleteArmedUntil_ = now_ + kQuitConfirmSec;
                result = {};
            }
        } else if (result.action != PanelAction::None) {
            deleteArmed_.clear();
        }
        listTracking_ = false;
        listDragging_ = false;
        listPress_ = {};
        applyLocal(result);
    }
    return result;
}

bool MicPanel::pointerLeave() {
    const bool changed = pointer_.inside || hover_.action != PanelAction::None || listTracking_;
    pointer_.inside = false;
    pointer_.down = false;
    hover_ = {};
    dragAction_ = PanelAction::None;
    listTracking_ = false;
    listDragging_ = false;
    listPress_ = {};
    return changed;
}

bool MicPanel::scroll(double dy) {
    if (dy == 0.0 || listMaxScroll_ <= 0 || overlay_ == PanelOverlay::AppsConfirm) return false;
    const double next = std::clamp(listScroll_ - dy * kScrollPxPerUnit, 0.0, listMaxScroll_);
    if (next == listScroll_) return false;
    listScroll_ = next;
    return true;
}

bool MicPanel::tick(double now) {
    const bool holdExpired = holdUntil_ >= 0 && now_ <= holdUntil_ && now > holdUntil_;
    bool mixHoldExpired = false;
    for (const HeldMixVolume& item : heldMixVolumes_) {
        if (item.until >= 0 && now_ <= item.until && now > item.until) mixHoldExpired = true;
    }
    bool muteHoldExpired = false;
    for (const HeldMixMute& item : heldMixMutes_) {
        if (item.until >= 0 && now_ <= item.until && now > item.until) muteHoldExpired = true;
    }
    now_ = now;
    bool changed = holdExpired;  // 書いた値の表示をやめて、読んだ値に戻す
    if (quitArmed_ && now > quitArmedUntil_) {
        quitArmed_ = false;
        changed = true;
    }
    if (updateArmed_ && now > updateArmedUntil_) {
        updateArmed_ = false;
        changed = true;
    }
    if (!deleteArmed_.empty() && now > deleteArmedUntil_) {
        deleteArmed_.clear();
        changed = true;
    }
    if (!nameError_.empty() && now > nameErrorUntil_) {
        nameError_.clear();
        changed = true;
    }
    if (mixHoldExpired || muteHoldExpired) changed = true;
    return changed;
}

void MicPanel::resetView() {
    view_ = PanelView::Settings;
    overlay_ = PanelOverlay::None;
    viewKey_.clear();
    listScroll_ = 0;
    listTracking_ = false;
    listDragging_ = false;
    listPress_ = {};
    updateArmed_ = false;
    quitArmed_ = false;
    launchResult_ = frame_apps::LaunchResult::Started;
}

void MicPanel::holdPreset(PanelAction action, double until) {
    presetHold_ = action;
    presetHoldUntil_ = until;
}

bool MicPanel::mixDrag(std::string& node, double& percent) const {
    if (dragAction_ != PanelAction::MixMicSlider && dragAction_ != PanelAction::MixToMicSlider &&
        dragAction_ != PanelAction::MixToPhonesSlider) {
        return false;
    }
    for (const Track& track : tracks_) {
        if (track.action == dragAction_ && track.index == dragIndex_) {
            node = track.node;
            percent = dragValue_;
            return !node.empty();
        }
    }
    return false;
}

void MicPanel::holdMixVolume(const std::string& node, double percent, double until) {
    for (HeldMixVolume& item : heldMixVolumes_) {
        if (item.node != node) continue;
        item.percent = percent;
        item.until = until;
        return;
    }
    heldMixVolumes_.push_back({node, percent, until});
}

bool MicPanel::heldMixVolume(const std::string& node, double& percent) const {
    for (const HeldMixVolume& item : heldMixVolumes_) {
        if (item.node != node || now_ > item.until) continue;
        percent = item.percent;
        return true;
    }
    return false;
}

void MicPanel::clearMixVolumeHold(const std::string& node) {
    heldMixVolumes_.erase(std::remove_if(heldMixVolumes_.begin(), heldMixVolumes_.end(),
                                         [&](const HeldMixVolume& item) { return item.node == node; }),
                          heldMixVolumes_.end());
}

void MicPanel::holdMixMute(const std::string& node, bool muted, double until) {
    for (HeldMixMute& item : heldMixMutes_) {
        if (item.node != node) continue;
        item.muted = muted;
        item.until = until;
        return;
    }
    heldMixMutes_.push_back({node, muted, until});
}

bool MicPanel::heldMixMute(const std::string& node, bool& muted) const {
    for (const HeldMixMute& item : heldMixMutes_) {
        if (item.node != node || now_ > item.until) continue;
        muted = item.muted;
        return true;
    }
    return false;
}

void MicPanel::clearMixMuteHold(const std::string& node) {
    heldMixMutes_.erase(std::remove_if(heldMixMutes_.begin(), heldMixMutes_.end(),
                                       [&](const HeldMixMute& item) { return item.node == node; }),
                        heldMixMutes_.end());
}

void MicPanel::wantMixVolume(const std::string& node, double percent) {
    for (auto& item : wantedMix_) {
        if (item.first != node) continue;
        item.second = percent;
        return;
    }
    wantedMix_.push_back({node, percent});
}

void MicPanel::clearWantedMix(const std::string& node) {
    wantedMix_.erase(std::remove_if(wantedMix_.begin(), wantedMix_.end(),
                                    [&](const std::pair<std::string, double>& item) { return item.first == node; }),
                     wantedMix_.end());
}

std::vector<std::pair<std::string, double>> MicPanel::wantedMixVolumes() const {
    return wantedMix_;
}

bool MicPanel::wantedMixPercent(const std::string& node, double& percent) const {
    for (const auto& item : wantedMix_) {
        if (item.first != node) continue;
        percent = item.second;
        return true;
    }
    return false;
}

void MicPanel::setNameError(const std::string& text, double until) {
    nameError_ = text;
    nameErrorUntil_ = until;
}

void MicPanel::holdNsValues(double vad, double grace, double until) {
    holdVad_ = clampNsVad(vad);
    holdGrace_ = clampNsGrace(grace);
    holdUntil_ = until;
}

void MicPanel::displayedNsValues(double& vad, double& grace) const {
    const bool held = now_ <= holdUntil_;
    vad = held ? holdVad_ : baseVad_;
    grace = held ? holdGrace_ : baseGrace_;
    if (dragAction_ == PanelAction::NsVadSlider) vad = dragValue_;
    if (dragAction_ == PanelAction::NsGraceSlider) grace = dragValue_;
}

bool MicPanel::endDrag() {
    if (dragAction_ == PanelAction::None) return false;
    dragAction_ = PanelAction::None;
    pointer_.down = false;
    return true;
}

void MicPanel::setDragForPreview(PanelAction action, double value) {
    dragAction_ = action;
    dragValue_ = action == PanelAction::NsVadSlider ? clampNsVad(value) : clampNsGrace(value);
}

void MicPanel::armQuitForPreview() {
    quitArmed_ = true;
    quitArmedUntil_ = 1e300;
}

void MicPanel::armUpdateForPreview() {
    updateArmed_ = true;
    updateArmedUntil_ = 1e300;
}

void MicPanel::setPointerForPreview(double x, double y, bool down) {
    pointer_ = {x, y, true, down};
    hover_ = hitTest(x, y);
}

void MicPanel::showForPreview(PanelView view, PanelOverlay overlay) {
    view_ = view;
    overlay_ = overlay;
}

// ============================================================================
// 見出し
// ============================================================================

void MicPanel::drawHeader(const frame_ui::Canvas& ui, const UiText& t, const PanelModel& model) {
    cairo_t* cr = ui.cr;
    const MicState& state = model.state;
    const double titleSize = ui.fit(t.title, frame_ui::kTitleSize, 22, kChipRect.x - frame_ui::kPanelPadX - 12, true);
    ui.text(frame_ui::kPanelPadX, frame_ui::centerBaseline(24, frame_ui::kHeaderH, titleSize), t.title, titleSize,
            rgb(kText), true);

    // 状態ラベル（幅 168 で固定。ミュート中は赤い地にマイクに斜線の絵、使用中は緑の ●、未使用・読み込み中は灰の ○）
    const bool muted = state.muteKnown && state.muted;
    std::string label;
    Color fill = kIdleFill;
    Color fg = kIdleText;
    int mark = 0;  // 0 = ○、1 = ●、2 = マイクに斜線、3 = なし
    if (muted) {
        label = t.micMuted;
        fill = kMuteFill;
        fg = kMuteText;
        mark = 2;
    } else if (!state.loaded) {
        label = t.loading;
        mark = 3;
    } else if (!state.linksKnown) {
        label = t.chipUnknown;
    } else if (state.inUse) {
        label = t.micInUse;
        fill = kSuccessTint;
        fg = kSuccess;
        mark = 1;
    } else {
        label = t.micIdle;
    }
    fillRounded(cr, kChipRect, kChipRect.h / 2, fill);
    const double markW = mark == 2 ? 20 : (mark == 3 ? 0 : 12);
    const double gap = markW > 0 ? 8 : 0;
    const double size = ui.fit(label, frame_ui::kControlSize, 13, kChipRect.w - 32 - markW - gap, true);
    const double textW = ui.measure(label, size, true);
    const double left = kChipRect.x + (kChipRect.w - markW - gap - textW) / 2;
    const double cy = kChipRect.y + kChipRect.h / 2;
    if (mark == 1) {
        fillCircle(cr, left + 6, cy, 6, fg);
    } else if (mark == 0) {
        setColor(cr, fg);
        cairo_set_line_width(cr, 2);
        cairo_new_sub_path(cr);
        cairo_arc(cr, left + 6, cy, 5, 0, 2 * M_PI);
        cairo_stroke(cr);
    } else if (mark == 2) {
        drawMicIcon(cr, left, cy - 10, 20, fg, true);
    }
    ui.text(left + markW + gap, frame_ui::centerBaseline(kChipRect.y, kChipRect.h, size), label, size, rgb(fg), true);

    // ミュートする / ミュートを解除（幅 220 で固定。読めていない間は押せない）
    const bool canMute = state.muteKnown;
    const Rect muteHit = frame_ui::drawButton(ui, kMuteRect, "", frame_ui::ButtonKind::Normal, canMute);
    withAlpha(cr, canMute ? 1.0 : frame_ui::kDisabledAlpha, [&] {
        const std::string text = muted ? t.unmute : t.mute;
        const double iconW = muted ? 0 : 20;
        const double iconGap = muted ? 0 : 8;
        const double textSize = ui.fit(text, frame_ui::kControlSize, 13, kMuteRect.w - 28 - iconW - iconGap, true);
        const double w = iconW + iconGap + ui.measure(text, textSize, true);
        const double x = kMuteRect.x + (kMuteRect.w - w) / 2;
        if (!muted) drawMicIcon(cr, x, kMuteRect.y + kMuteRect.h / 2 - 10, 20, kText, true);
        ui.text(x + iconW + iconGap, frame_ui::centerBaseline(kMuteRect.y, kMuteRect.h, textSize), text, textSize,
                rgb(kText), true);
    });
    addButton({PanelAction::MuteToggle}, muteHit);

    // 右: 「アプリと更新」（新しい版があるときはピンクの点）/ アプリと更新の画面では「← マイクの設定へ」
    const bool apps = view_ == PanelView::Apps;
    const Rect appsHit = frame_ui::drawButton(ui, kAppsRect, "", frame_ui::ButtonKind::Normal);
    {
        using frame_updater::UpdateState;
        // ピンクの点: 新しい版がある（入れ終わり・失敗も）か、このアプリで使うアプリが入っていない
        const UpdateState s = model.update.state;
        bool relatedMissing = false;
        for (const frame_apps::Entry& e : model.apps) relatedMissing |= e.related && e.state == frame_apps::AppState::Missing;
        const bool attention = !apps && (s == UpdateState::Available || s == UpdateState::Installed ||
                                         s == UpdateState::InstallFailed || relatedMissing);
        const std::string text = apps ? t.backToSettings : t.appsButton;
        const double iconW = apps ? 18 : 20;
        const double dotW = attention ? 10 + 8 : 0;
        const double textSize = ui.fit(text, frame_ui::kControlSize, 13, kAppsRect.w - 32 - iconW - 8 - dotW, true);
        const double w = iconW + 8 + ui.measure(text, textSize, true) + dotW;
        const double x = kAppsRect.x + (kAppsRect.w - w) / 2;
        const double acy = kAppsRect.y + kAppsRect.h / 2;
        if (apps) {
            drawPolyline(cr, x, acy - 9, 18, 18, kText, 2.4, {11, 3, 5, 9, 11, 15});
        } else {
            drawGridIcon(cr, x, acy - 10, 20, kText);
        }
        const double textEnd = x + iconW + 8 + ui.text(x + iconW + 8, frame_ui::centerBaseline(kAppsRect.y, kAppsRect.h, textSize),
                                                       text, textSize, rgb(kText), true);
        if (attention) fillCircle(cr, textEnd + 8 + 5, acy, 5, kAccent);
    }
    addButton({apps ? PanelAction::ShowSettings : PanelAction::ShowApps}, appsHit);
}

// ============================================================================
// マイクの設定
// ============================================================================

void MicPanel::drawSelectors(const frame_ui::Canvas& ui, const UiText& t, const Config& config, const PanelModel& model, bool wide) {
    cairo_t* cr = ui.cr;
    const AudioDevices& devices = model.state.devices;
    const Rect outputBox = wide ? kMixOutputSelect : kOutputSelect;
    const Rect micBox = wide ? kMixMicSelect : kMicSelect;
    /**
     * 欄の地・枠・絵の箱・▾ を描き、文字を置ける幅を返す。
     */
    const auto selectBase = [&](Rect r, PanelAction action) {
        const int l = look(r);
        fillRounded(cr, r, 18, l == 2 ? kControlDown : (l == 1 ? kControlHover : kCard));
        strokeRounded(cr, r, 18, kBorder, 2);
        fillRounded(cr, {r.x + 10, r.y + 10, 40, 40}, 11, kIconBox);
        drawPolyline(cr, r.right() - 14 - 18, r.y + r.h / 2 - 9, 18, 18, kText, 2.4, {4, 7, 9, 12, 14, 7});
        addButton({action}, r);
    };
    /**
     * 欄の 2 行（小さな見出しと名前）を描く。
     */
    const auto selectText = [&](Rect r, double right, const std::string& label, const std::string& name) {
        const double x = r.x + 62;
        const double w = right - x;
        ui.text(x, r.y + 22, ui.ellipsize(label, 12, w), 12, rgb(kTextMuted));
        const double size = ui.fit(name, 18, 15, w, true);
        ui.text(x, r.y + 45, ui.ellipsize(name, size, w, true), size, rgb(kText), true);
    };

    // 音の出口（表示している出口。何個つながっても 1 つの欄）
    const std::string activeKey = model.activeOutputKey.empty() ? devices.defaultOutputKey : model.activeOutputKey;
    const std::vector<OutputEntry> entries = outputEntries(config, devices, activeKey, t);
    selectBase(outputBox, PanelAction::OpenOutputPicker);
    drawOutputIcon(cr, isBuiltinSpeaker(viewedKey_), outputBox.x + 10 + 9, outputBox.y + 10 + 9, 22, kText);
    const std::string count = entries.empty() ? std::string() : formatInt(t.countFormat, static_cast<int>(entries.size()));
    const double countRight = outputBox.right() - 14 - 18 - 12;
    const double countW = count.empty() ? 0 : ui.text(countRight, outputBox.y + 36, count, 14, rgb(kTextMuted), false, true);
    std::string viewedName = t.loading;
    for (const OutputEntry& e : entries) {
        if (e.key == viewedKey_) viewedName = e.name;
    }
    selectText(outputBox, countRight - countW - 12, t.outputLabel, viewedName);

    // 使うマイク（短い名前）
    selectBase(micBox, PanelAction::OpenMicPicker);
    drawMicIcon(cr, micBox.x + 10 + 9, micBox.y + 10 + 9, 22, kText, false);
    std::string micName = "—";
    if (!devices.defaultInputKey.empty()) {
        const AudioEndpoint* input = devices.findInput(devices.defaultInputKey);
        micName = isBuiltinMic(devices.defaultInputKey) ? t.builtinMicShort
                                                         : shortDeviceName(input != nullptr ? input->name : devices.defaultInputKey);
    }
    selectText(micBox, micBox.right() - 14 - 18 - 8, t.micLabel, micName);
}

void MicPanel::drawSettings(const frame_ui::Canvas& ui, const UiText& t, const Config& config, const PanelModel& model) {
    const MicState& state = model.state;
    const AudioDevices& devices = state.devices;

    // 設定を表示する出口: 選んだ出口（まだあれば）、無ければ今の出口
    const std::string activeKey = model.activeOutputKey.empty() ? devices.defaultOutputKey : model.activeOutputKey;
    const bool viewKnown = !viewKey_.empty() && (devices.findOutput(viewKey_) != nullptr || config.outputs.count(viewKey_) != 0);
    viewedKey_ = viewKnown ? viewKey_ : activeKey;
    viewingActive_ = viewedKey_ == activeKey;
    const AudioEndpoint* endpoint = devices.findOutput(viewedKey_);
    const auto saved = config.outputs.find(viewedKey_);
    const OutputProfile profile =
        saved != config.outputs.end() ? saved->second : firstProfile(viewedKey_, endpoint != nullptr ? endpoint->name : "");
    const std::string outputName =
        viewedKey_.empty() ? std::string(t.loading)
                           : outputDisplayName(viewedKey_, endpoint != nullptr ? endpoint->name : profile.name, t);
    // 今の出口は今かかっている値、ほかの出口は覚えている値を出す
    bool echo = profile.echo;
    bool ns = profile.ns;
    bool known = true;
    bool nsParamsKnown = true;
    baseVad_ = profile.nsVad;
    baseGrace_ = profile.nsGrace;
    if (viewingActive_) {
        known = state.echoKnown && state.nsKnown;
        echo = state.echo;
        ns = state.ns;
        const NsParams& p = state.nsParams;
        nsParamsKnown = p.nodeKnown && p.vadKnown && p.graceKnown;
        if (nsParamsKnown) {
            baseVad_ = p.vad;
            baseGrace_ = p.grace;
        }
    }

    const int tabIndex = config.tab == PanelTab::Quick ? 0 : config.tab == PanelTab::Fine ? 1 : 2;
    const bool external = !devices.defaultInputKey.empty() && !isBuiltinMic(devices.defaultInputKey);
    if (config.tab == PanelTab::Mix) {
        if (!mixWide_) {
            listScroll_ = 0;
            mixWide_ = true;
        }
        drawSelectors(ui, t, config, model, true);
        frame_ui::drawCard(ui, kMixSettingsCard);
        const std::vector<Rect> tabs =
            frame_ui::drawSegmented(ui, kMixTabsRect, {t.tabQuick, t.fineTune, t.mixTab}, tabIndex, true);
        addButton({PanelAction::TabQuick}, tabs[0]);
        addButton({PanelAction::TabFine}, tabs[1]);
        addButton({PanelAction::TabMix}, tabs[2]);
        drawMix(ui, t, config, model);
        drawVoice(ui, t, model.voice, kMixVoiceCard);
        return;
    }
    if (mixWide_) {
        listScroll_ = 0;
        mixWide_ = false;
    }
    drawSelectors(ui, t, config, model, false);
    frame_ui::drawCard(ui, kSettingsCard);
    const std::vector<Rect> tabs =
        frame_ui::drawSegmented(ui, kTabsRect, {t.tabQuick, t.fineTune, t.mixTab}, tabIndex, true);
    addButton({PanelAction::TabQuick}, tabs[0]);
    addButton({PanelAction::TabFine}, tabs[1]);
    addButton({PanelAction::TabMix}, tabs[2]);
    if (external) {
        drawExternal(ui, t, model, echo, ns, outputName);
    } else if (config.tab == PanelTab::Quick) {
        drawQuick(ui, t, config, model, echo, ns, known, outputName);
    } else {
        drawFine(ui, t, model, echo, ns, known, nsParamsKnown);
    }
    drawPipeline(ui, t, state, external);
    drawVoice(ui, t, model.voice, kVoiceCard);
}

void MicPanel::drawMix(const frame_ui::Canvas& ui, const UiText& t, const Config& config, const PanelModel& model) {
    cairo_t* cr = ui.cr;
    const MixGraph& mix = model.state.mix;
    const bool routing = mix.settingKnown && mix.enabled;
    std::string status = routing ? (mix.vrlinkPresent ? t.mixToPcOn : t.mixToPcWaiting) : t.mixToPcOff;
    if (config.mixEnabled && !mix.settingKnown) status = t.mixNeedRestart;
    // タブと同じ幅と高さ。間は 16px。状態の一文はその下。
    const double switchY = kMixTabsRect.bottom() + 16;
    const double switchH = kMixTabsRect.h;
    const std::vector<Rect> power =
        frame_ui::drawSegmented(ui, {kMixTabsRect.x, switchY, kMixTabsRect.w, switchH}, {t.on, t.off},
                                routing || config.mixEnabled ? 0 : 1, true);
    addButton({PanelAction::MixOn}, power[0]);
    addButton({PanelAction::MixOff}, power[1]);
    const double statusY = switchY + switchH + 8;
    ui.text(kMixInnerX, frame_ui::centerBaseline(statusY, 20, 14), ui.ellipsize(status, 14, kMixInnerW), 14,
            rgb(routing && mix.routed ? kSuccess : kTextSoft), true);

    /**
     * 数字を cx の中央に置く。
     */
    const auto centerText = [&](double cx, double baseline, const std::string& text, double size, Color color, bool bold) {
        const double w = ui.measure(text, size, bold);
        ui.text(cx - w / 2.0, baseline, text, size, rgb(color), bold);
    };
    /**
     * 縦のカプセル（下 0%・上 150%、100% に刻み）。隣の細いバーはフェーダー後のピーク。
     * 当たりはカプセルだけ。一覧の中には入れないので、ドラッグは音量で、スクロールしない。
     */
    const auto fader = [&](double cx, double top, double capH, const std::string& node, const MixNodeVolume& live, double fallback,
                           PanelAction action, int index, const std::string& meterKey, const char* icon, double iconW, double iconH,
                           const Rect* clip, PanelAction muteAction) {
        const bool draggingThis = dragAction_ == action && dragIndex_ == index;
        double percent = fallback;
        double wanted = 0.0;
        if (wantedMixPercent(node, wanted)) percent = wanted;
        if (live.known) percent = std::clamp(live.linear, 0.0, kMixVolumeMax) * 100.0;
        // 動かした直後の読み直しはまだ古い音量。一致するまでスライダーを戻さない。
        double heldPercent = 0.0;
        if (heldMixVolume(node, heldPercent)) {
            if (live.known && std::abs(percent - heldPercent) < 1.5) clearMixVolumeHold(node);
            else percent = heldPercent;
        }
        if (draggingThis) percent = dragValue_;

        const double capW = 32;
        const Rect capsule {cx - capW / 2.0, top, capW, capH};
        const double radius = capW / 2.0;
        const double fraction = std::clamp(percent / 150.0, 0.0, 1.0);
        cairo_save(cr);
        frame_ui::roundedRect(cr, capsule, radius);
        cairo_clip(cr);
        fillRounded(cr, capsule, 0, kBg);
        const double fillTop = capsule.bottom() - capsule.h * fraction;
        if (capsule.bottom() - fillTop > 0.5) {
            fillRounded(cr, {capsule.x, fillTop, capsule.w, capsule.bottom() - fillTop}, 0, kAccent);
        }
        cairo_restore(cr);
        strokeRounded(cr, capsule, radius, kBorder, 1.5);
        const double notchY = capsule.bottom() - capsule.h * (100.0 / 150.0);
        setColor(cr, kText);
        cairo_set_line_width(cr, 2);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, capsule.x + 5, notchY);
        cairo_line_to(cr, capsule.right() - 5, notchY);
        cairo_stroke(cr);

        const Rect meter {capsule.right() + 4, capsule.y, 6, capsule.h};
        // 押した直後の読み直しはまだミュート前。一致するまでアイコンを戻さない。
        bool muted = live.muted;
        bool heldMuted = false;
        if (heldMixMute(node, heldMuted)) {
            if (live.known && live.muted == heldMuted) clearMixMuteHold(node);
            else muted = heldMuted;
        }
        const float post = muted ? 0.0f : mixPeak(meterKey) * static_cast<float>(percent / 100.0);
        const double db = post <= 1e-6f ? -120.0 : 20.0 * std::log10(static_cast<double>(post));
        const double level = std::clamp((db + 48.0) / 48.0, 0.0, 1.0);
        fillRounded(cr, meter, 3, kBg);
        if (level > 0.004) {
            cairo_save(cr);
            frame_ui::roundedRect(cr, meter, 3);
            cairo_clip(cr);
            const double litTop = meter.bottom() - meter.h * level;
            cairo_pattern_t* grad = cairo_pattern_create_linear(0, meter.bottom(), 0, meter.y);
            const Color green = hexColor(0x3dbe6a);
            const Color yellow = hexColor(0xe6c15a);
            cairo_pattern_add_color_stop_rgb(grad, 0.0, green.r, green.g, green.b);
            cairo_pattern_add_color_stop_rgb(grad, 0.62, yellow.r, yellow.g, yellow.b);
            cairo_pattern_add_color_stop_rgb(grad, 1.0, kDanger.r, kDanger.g, kDanger.b);
            cairo_set_source(cr, grad);
            cairo_rectangle(cr, meter.x, litTop, meter.w, meter.bottom() - litTop);
            cairo_fill(cr);
            cairo_pattern_destroy(grad);
            cairo_restore(cr);
        }

        tracks_.push_back({action, capsule.x, capsule.right(), 0, 150, capsule.y + capsule.h / 2.0, index, node, true, capsule.y,
                           capsule.bottom()});
        Rect hit {capsule.x - 4, capsule.y, meter.right() - (capsule.x - 4), capsule.h};
        if (clip != nullptr) hit = intersect(hit, *clip);
        if (hit.w > 4 && hit.h > 4) addButton({action, index, node}, hit, false);

        char text[16];
        std::snprintf(text, sizeof(text), "%.0f%%", percent);
        centerText(cx, capsule.bottom() + 13, text, 13, std::fabs(percent - 100.0) < 0.5 ? kAccent : kText, true);
        // アイコンはミュートの切り替え。音量はそのまま、絵だけ Off になる。
        if (icon != nullptr) {
            const double iconSize = 20;
            const double iconX = cx - iconSize / 2.0;
            const double iconY = capsule.bottom() + 20;
            if (muted && muteAction == PanelAction::MixToMicMute) {
                drawFilledIcon(cr, iconX, iconY, iconSize, 512, 512, kMicOffIconPath, kText);
                drawMicOffSlash(cr, iconX, iconY, iconSize, kText);
            } else if (muted && muteAction == PanelAction::MixToPhonesMute) {
                drawFilledIcon(cr, iconX, iconY, iconSize, 56, 56, kSpeakerOffIconPath, kText);
            } else {
                drawFilledIcon(cr, iconX, iconY, iconSize, iconW, iconH, icon, kText);
            }
            if (muteAction != PanelAction::None) {
                Rect iconHit {iconX - 6, iconY - 4, iconSize + 12, iconSize + 8};
                if (clip != nullptr) iconHit = intersect(iconHit, *clip);
                if (iconHit.w > 4 && iconHit.h > 4) addButton({muteAction, index, node}, iconHit, false);
            }
        }
        if (db > -47.5) {
            char dbText[16];
            std::snprintf(dbText, sizeof(dbText), "%.0f", db > -0.5 ? 0.0 : db);
            centerText(cx, capsule.bottom() + 52, dbText, 11, levelColor(level), true);
        }
    };

    // マイクの音量は既定のマイク。左に固定し、チャンネル列だけ横にスクロールする。
    // 説明は列の下（カードの底）。名前のエラーもそこ。
    constexpr double kMicW = 120;
    constexpr double kChanW = 144;
    constexpr double kAddW = 72;
    constexpr double kGap = 10;
    const std::string hint = !nameError_.empty() && now_ <= nameErrorUntil_ ? nameError_ : std::string(t.mixHint);
    const Color hintColor = !nameError_.empty() && now_ <= nameErrorUntil_ ? kDangerText : kTextMuted;
    const double hintBaseline = kMixSettingsCard.bottom() - frame_ui::kCardPadX - 4 - 15;
    const double stripTop = statusY + 28;
    const double stripBottom = hintBaseline - 28;
    const double stripH = std::max(180.0, stripBottom - stripTop);
    const Rect micCard {kMixInnerX, stripTop, kMicW, stripH};
    const Rect list {micCard.right() + kGap, stripTop, kMixInnerRight - (micCard.right() + kGap), stripH};
    // 再生中のアプリ名は出さない。その分だけフェーダーを名前のすぐ下まで伸ばす。
    const double capTop = 40;
    const double capH = std::max(80.0, stripH - capTop - 64);

    fillRounded(cr, micCard, 16, kInset);
    strokeRounded(cr, micCard, 16, kBorder, 2);
    ui.text(micCard.x + 10, micCard.y + 28, ui.ellipsize(t.mixMic, 14, micCard.w - 20, true), 14, rgb(kText), true);
    MixNodeVolume micVol;
    micVol.known = model.state.sourceVolumeKnown;
    micVol.linear = model.state.sourceVolume;
    micVol.nodeName = kDefaultSource;
    fader(micCard.x + 52, micCard.y + capTop, capH, kDefaultSource, micVol, 100.0, PanelAction::MixMicSlider, 0, kDefaultSource,
          kMicIconPath, 17, 24, nullptr, PanelAction::None);

    const bool room = static_cast<int>(config.mixChannels.size()) < kMixChannelMax;
    const double contentW = static_cast<double>(config.mixChannels.size()) * (kChanW + kGap) + kAddW;
    beginList(list, contentW, true);
    addButton({PanelAction::None}, list, true);
    cairo_save(cr);
    cairo_rectangle(cr, list.x, list.y, list.w, list.h);
    cairo_clip(cr);
    double x = list.x - listScroll_;
    for (size_t i = 0; i < config.mixChannels.size(); ++i) {
        const MixChannel& channel = config.mixChannels[i];
        const Rect row {x, list.y, kChanW, stripH};
        if (row.right() >= list.x && row.x <= list.right()) {
            const MixChannelLive* live = nullptr;
            for (const MixChannelLive& item : mix.channels) {
                if (item.id == channel.id) live = &item;
            }
            fillRounded(cr, row, 16, kInset);
            strokeRounded(cr, row, 16, kAccent, 2);
            const bool armed = deleteArmed_ == channel.id && now_ <= deleteArmedUntil_;
            const Rect del = armed ? Rect {row.x + 8, row.y + 8, row.w - 16, 30} : Rect {row.right() - 8 - 28, row.y + 8, 28, 28};
            const Rect delHit = intersect(del, list);
            if (delHit.w > 4 && delHit.h > 4) addButton({PanelAction::MixDelete, static_cast<int>(i), channel.id}, delHit, true);
            const int delLook = look(del);
            fillRounded(cr, del, 10, armed ? kAccentTint : (delLook == 2 ? kControlDown : (delLook == 1 ? kControlHover : kIconBox)));
            if (armed) {
                const double size = ui.fit(t.mixDeleteConfirm, 13, 10, del.w - 12, true);
                ui.text(del.x + 8, frame_ui::centerBaseline(del.y, del.h, size), ui.ellipsize(t.mixDeleteConfirm, size, del.w - 12, true),
                        size, rgb(kAccent), true);
            } else {
                setColor(cr, kText);
                cairo_set_line_width(cr, 2);
                cairo_move_to(cr, del.x + 9, del.y + 9);
                cairo_line_to(cr, del.right() - 9, del.bottom() - 9);
                cairo_move_to(cr, del.right() - 9, del.y + 9);
                cairo_line_to(cr, del.x + 9, del.bottom() - 9);
                cairo_stroke(cr);
                const Rect nameHit {row.x + 10, row.y + 8, del.x - row.x - 16, 28};
                const Rect nameButton = intersect(nameHit, list);
                if (nameButton.w > 4) addButton({PanelAction::MixRename, static_cast<int>(i), channel.id}, nameButton, true);
                const double nameSize = ui.fit(channel.name, 15, 12, nameHit.w, true);
                ui.text(nameHit.x, frame_ui::centerBaseline(nameHit.y, nameHit.h, nameSize),
                        ui.ellipsize(channel.name, nameSize, nameHit.w, true), nameSize, rgb(kAccent), true);
            }
            MixNodeVolume toMic;
            MixNodeVolume toPhones;
            if (live != nullptr) {
                toMic = live->toMic;
                toPhones = live->toPhones;
            }
            const double col = (kChanW - 16) / 2.0;
            fader(row.x + 8 + col / 2.0 - 6, row.y + capTop, capH, channelToMicNode(channel.id), toMic, 50.0, PanelAction::MixToMicSlider,
                  static_cast<int>(i), channelSinkNode(channel.id), kMicIconPath, 17, 24, &list, PanelAction::MixToMicMute);
            // 右はヘッドホンではなく Frame のスピーカーへ。
            fader(row.x + 8 + col + col / 2.0 - 6, row.y + capTop, capH, channelToPhonesNode(channel.id), toPhones, 100.0,
                  PanelAction::MixToPhonesSlider, static_cast<int>(i), channelSinkNode(channel.id), kSpeakerIconPath, 56, 56, &list,
                  PanelAction::MixToPhonesMute);
        }
        x += kChanW + kGap;
    }
    // 足すは四角のプラス。列の縦の真ん中。8 個あるときは押せない。
    const Rect add {x, list.y + (stripH - kAddW) / 2.0, kAddW, kAddW};
    if (add.right() >= list.x && add.x <= list.right()) {
        const int addLook = room ? look(add) : 0;
        fillRounded(cr, add, 16, addLook == 2 ? kControlDown : (addLook == 1 ? kControlHover : kInset));
        strokeRounded(cr, add, 16, room ? kAccent : kBorder, 2);
        const double icon = 40;
        drawFilledIcon(cr, add.x + (add.w - icon) / 2.0, add.y + (add.h - icon) / 2.0, icon, 28, 28, kAddIconPath,
                       room ? kAccent : kTextMuted);
        if (room) {
            const Rect addHit = intersect(add, list);
            if (addHit.w > 4 && addHit.h > 4) addButton({PanelAction::MixAdd}, addHit, true);
        }
    }
    cairo_restore(cr);
    endList(ui, contentW, list.right() - 8);
    drawLines(ui, kMixInnerX, hintBaseline, 15, wrapText(ui, hint, 13, false, kMixInnerW, 2), 13, hintColor);
}

void MicPanel::drawMixSuggest(const frame_ui::Canvas& ui, const UiText& t, const Config& config, const PanelModel& model) {
    cairo_t* cr = ui.cr;
    const Rect modal {48, 36, 1104, 710};
    const double y0 = drawModal(ui, modal, t.mixSuggestTitle, t.mixSuggestSub);
    const double mid = modal.x + modal.w / 2.0;
    setColor(cr, kDivider);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, mid, y0);
    cairo_line_to(cr, mid, modal.bottom() - 20);
    cairo_stroke(cr);

    const std::vector<std::string> suggestions = {t.suggestMusic, t.suggestBoard, t.suggestBrowser, t.suggestGame,
                                                  t.suggestVideo};
    const double rowH = 52;
    const double gap = 6;
    const Rect left {modal.x + 24, y0 + 36, mid - modal.x - 40, modal.bottom() - (y0 + 36) - 20};
    ui.text(left.x, y0 + 22, ui.ellipsize(t.mixAdd, 16, left.w, true), 16, rgb(kText), true);
    double y = left.y;
    for (size_t i = 0; i < suggestions.size() + 1; ++i) {
        if (y + rowH > left.bottom()) break;
        const bool custom = i == suggestions.size();
        const std::string name = custom ? std::string(t.mixCustom) : suggestions[i];
        std::string normalized;
        const bool taken = !custom && validateChannelName(name, config.mixChannels, "", normalized) == MixNameError::Duplicate;
        const Rect row {left.x, y, left.w, rowH};
        const int l = taken ? 0 : look(row);
        fillRounded(cr, row, 14, l == 2 ? kControlDown : (l == 1 ? kControlHover : kInset));
        ui.text(row.x + 16, y + 34, ui.ellipsize(name, 18, row.w - 32, true), 18, rgb(taken ? kTextMuted : kText), true);
        if (!taken) addButton({custom ? PanelAction::MixCustom : PanelAction::MixSuggest, 0, custom ? std::string() : name}, row);
        y += rowH + gap;
    }

    std::vector<std::string> apps;
    const auto addApp = [&](const std::string& name) {
        if (name.empty()) return;
        for (const std::string& have : apps) {
            if (mixAppMatches(name, have, have)) return;
        }
        apps.push_back(name);
    };
    for (const MixPlayback& play : model.state.mix.playbacks) addApp(play.app);
    for (const std::string& running : model.state.mix.runningApps) addApp(running);
    const Rect right {mid + 16, y0 + 36, modal.right() - 24 - (mid + 16), left.h};
    const Rect refresh {right.right() - 128, y0, 120, 34};
    ui.text(right.x, y0 + 22, ui.ellipsize(t.mixRunning, 16, right.w - 136, true), 16, rgb(kText), true);
    addButton({PanelAction::MixRefreshApps}, refresh);
    frame_ui::drawButton(ui, refresh, t.mixRefresh, frame_ui::ButtonKind::Normal);
    const double contentH = apps.empty() ? 36 : apps.size() * (rowH + gap) - gap;
    beginList(right, contentH);
    cairo_save(cr);
    cairo_rectangle(cr, right.x, right.y, right.w, right.h);
    cairo_clip(cr);
    if (apps.empty()) ui.text(right.x + 8, right.y + 28, ui.ellipsize(t.mixRunningEmpty, 16, right.w - 16), 16, rgb(kTextMuted));
    y = right.y - listScroll_;
    for (const std::string& app : apps) {
        if (y + rowH >= right.y && y <= right.bottom()) {
            const Rect row {right.x, y, right.w - 8, rowH};
            const int l = look(row);
            fillRounded(cr, row, 14, l == 2 ? kControlDown : (l == 1 ? kControlHover : kInset));
            // 名前の左に 32px のアイコン。行全体が選択の当たり判定のまま。
            const double iconSize = 32;
            const double iconX = row.x + 10;
            const double iconY = y + (rowH - iconSize) / 2.0;
            cairo_surface_t* icon = runningAppIcon(app);
            if (icon != nullptr) paintAppIcon(cr, icon, iconX, iconY, iconSize);
            else if (!frame_apps::drawIcon(cr, app, iconX, iconY, iconSize)) {
                fillRounded(cr, {iconX, iconY, iconSize, iconSize}, 8, kIconBox);
                std::string glyph = "?";
                if (!app.empty()) {
                    const unsigned char lead = static_cast<unsigned char>(app[0]);
                    size_t len = 1;
                    if ((lead & 0xE0) == 0xC0) len = 2;
                    else if ((lead & 0xF0) == 0xE0) len = 3;
                    else if ((lead & 0xF8) == 0xF0) len = 4;
                    if (len > app.size()) len = 1;
                    glyph = app.substr(0, len);
                    if (len == 1 && glyph[0] >= 'a' && glyph[0] <= 'z') glyph[0] = static_cast<char>(glyph[0] - 'a' + 'A');
                }
                const double glyphSize = ui.fit(glyph, 16, 12, iconSize - 6, true);
                ui.text(iconX + (iconSize - ui.measure(glyph, glyphSize, true)) / 2.0,
                        frame_ui::centerBaseline(iconY, iconSize, glyphSize), glyph, glyphSize, rgb(kAccent), true);
            }
            const double textX = iconX + iconSize + 10;
            ui.text(textX, y + 34, ui.ellipsize(app, 18, row.right() - textX - 12, true), 18, rgb(kText), true);
            addButton({PanelAction::MixUseApp, 0, app}, intersect(row, right), true);
        }
        y += rowH + gap;
    }
    cairo_restore(cr);
    endList(ui, contentH, right.right() - 4);
}

void MicPanel::drawQuick(const frame_ui::Canvas& ui, const UiText& t, const Config& /*config*/, const PanelModel& model,
                         bool echo, bool ns, bool known, const std::string& outputName) {
    cairo_t* cr = ui.cr;
    const std::string heading = format1(t.listeningFormat, outputName);
    ui.text(kInnerX, frame_ui::centerBaseline(262, 26, 18), ui.ellipsize(heading, 18, kInnerW, true), 18, rgb(kText), true);

    // プリセット（ユーザーと決めたもの）: イヤホン = エコー除去オフ・ノイズ除去オフ、スピーカー = エコー除去オン・ノイズ除去オフ。
    // 一致はこの 2 つだけで見る（バーの値はノイズ除去がオフなら効かない）
    bool matches[2] = {known && !echo && !ns, known && echo && !ns};
    // プリセットを書き込んでいる間は、押したカードを選択中の見た目で保つ（途中の値で選択が外れない）
    if (viewingActive_ && presetHold_ != PanelAction::None && now_ <= presetHoldUntil_) {
        matches[0] = presetHold_ == PanelAction::Earphone;
        matches[1] = presetHold_ == PanelAction::Speaker;
    }
    for (int i = 0; i < 2; ++i) {
        const Rect r {kInnerX, 296.0 + i * 106, kInnerW, 96};
        const bool selected = matches[i];
        const int l = look(r);
        const Color fill = selected ? (l == 2 ? kAccentPressed : (l == 1 ? kAccentHover : kAccent))
                                    : (l == 2 ? kControlDown : (l == 1 ? kControlHover : kControl));
        fillRounded(cr, r, 16, fill);
        strokeRounded(cr, r, 16, selected ? fill : (l == 2 ? kAccent : kBorder), 2);
        const Color fg = selected ? kOnAccent : kText;
        const double cy = r.y + r.h / 2;
        drawOutputIcon(cr, i == 1, r.x + 20, cy - 22, 44, fg, 3);

        // 右に「押すと何になるか」の札 2 つ（縦に並べる）
        const std::string tags[2] = {i == 0 ? t.chipEchoOff : t.chipEchoOn, t.chipNsOff};
        double tagW = 0;
        for (const std::string& tag : tags) tagW = std::max(tagW, ui.measure(tag, 13, true) + 22);
        const double tagX = r.right() - 16 - tagW;
        for (int c = 0; c < 2; ++c) {
            const Rect tag {tagX, cy - 27 + c * 30, tagW, 24};
            fillRounded(cr, tag, 12, selected ? kOnAccent : kBg);
            if (!selected) strokeRounded(cr, tag, 12, kBorder, 2);
            ui.text(tag.x + 11, frame_ui::centerBaseline(tag.y, tag.h, 13), tags[c], 13,
                    rgb(selected ? kAccentText : kText), true);
        }

        // 真ん中: 名前（選択中は前に ✓ の丸）と、何のためか
        double tx = r.x + 80;
        const double textRight = tagX - 14;
        if (selected) {
            fillCircle(cr, tx + 11, cy - 13, 11, kOnAccent);
            frame_ui::drawCheck(cr, tx + 11, cy - 13, 14, frame_ui::kAccent);
            tx += 30;
        }
        const std::string title = i == 0 ? t.earphoneCard : t.speakerCard;
        const double titleSize = ui.fit(title, 22, 16, textRight - tx, true);
        ui.text(tx, cy - 6, ui.ellipsize(title, titleSize, textRight - tx, true), titleSize, rgb(fg), true);
        const std::string effect = i == 0 ? t.earphoneHint : t.speakerHint;
        const double effectSize = ui.fit(effect, 15, 12, textRight - r.x - 80);
        ui.text(r.x + 80, cy + 23, ui.ellipsize(effect, effectSize, textRight - r.x - 80), effectSize,
                rgb(selected ? kOnAccentSoft : kTextMuted));
        addButton({i == 0 ? PanelAction::Earphone : PanelAction::Speaker}, r);
    }

    // カードの下: 自動で切り替えた直後は知らせ（元に戻す）、ふだんは説明
    if (model.switchNotice && model.switchKey == viewedKey_ && viewingActive_) {
        const Rect box {kInnerX, 502, kInnerW, 56};
        strokeRounded(cr, box, 14, kAccent, 2);
        drawPolyline(cr, box.x + 14, box.y + 18, 20, 24, kAccent, 2.2, {4, 8, 17, 8, 14, 5, NAN, NAN, 20, 16, 7, 16, 10, 19});
        const double buttonW = std::max(96.0, ui.measure(t.undo, frame_ui::kControlSize, true) + 32);
        const Rect undo {box.right() - 6 - buttonW, box.y + 6, buttonW, 44};
        addButton({PanelAction::UndoSwitch}, frame_ui::drawButton(ui, undo, t.undo));
        const double textX = box.x + 14 + 20 + 10;
        const std::vector<std::string> lines =
            wrapText(ui, format1(t.switchedFormat, outputName), 14, false, undo.x - 10 - textX, 2);
        drawLines(ui, textX, box.y + 28 - (static_cast<double>(lines.size()) - 1) * 9.5 + 5, 19, lines, 14, kText);
    } else {
        const std::string note = (matches[0] || matches[1] || !known) ? format1(t.quickNoteFormat, outputName) : t.quickCustom;
        drawLines(ui, kInnerX, 510 + 15, 20, wrapText(ui, note, 14, false, kInnerW, 2), 14, kTextMuted);
    }
}

void MicPanel::drawFine(const frame_ui::Canvas& ui, const UiText& t, const PanelModel& /*model*/, bool echo, bool ns,
                        bool known, bool nsParamsKnown) {
    cairo_t* cr = ui.cr;
    // エコー除去・ノイズ除去のオン / オフ（右に 2 行までの説明）
    for (int row = 0; row < 2; ++row) {
        const double top = 262 + row * 68;
        const bool isEcho = row == 0;
        const std::string title = isEcho ? t.rowEcho : t.rowNs;
        const double size = ui.fit(title, frame_ui::kLabelSize, 15, 196 - kInnerX - 8, true);
        ui.text(kInnerX, frame_ui::centerBaseline(top, 56, size), title, size, rgb(kText), true);
        const bool value = isEcho ? echo : ns;
        const std::vector<Rect> hits =
            frame_ui::drawSegmented(ui, {196, top, 210, 56}, {t.on, t.off}, known ? (value ? 0 : 1) : -1);
        addButton({isEcho ? PanelAction::EchoOn : PanelAction::NsOn}, hits[0]);
        addButton({isEcho ? PanelAction::EchoOff : PanelAction::NsOff}, hits[1]);
        const std::vector<std::string> lines = wrapText(ui, isEcho ? t.echoHint : t.nsHint, 14, false, kInnerRight - 420, 2);
        drawLines(ui, 420, top + 28 - (static_cast<double>(lines.size()) - 1) * 10 + 5, 20, lines, 14, kTextMuted);
    }

    // ノイズ除去の強さのバー 2 本。ノイズ除去がオフの間は効かないので、塗りとつまみを灰色にして説明を替える（押せるまま）
    double vad = 0.0;
    double grace = 0.0;
    displayedNsValues(vad, grace);
    const double trackX0 = kInnerX + 132 + 14 + 12;
    const double trackX1 = kInnerRight - 70 - 14 - 12;
    for (int row = 0; row < 2; ++row) {
        const bool isVad = row == 0;
        const double top = 404 + row * 54;
        const double cy = top + 24;
        const PanelAction action = isVad ? PanelAction::NsVadSlider : PanelAction::NsGraceSlider;
        const double min = isVad ? kNsVadMin : kNsGraceMin;
        const double max = isVad ? kNsVadMax : kNsGraceMax;
        const double value = isVad ? vad : grace;
        const std::string title = isVad ? t.nsVad : t.nsGrace;
        const double titleSize = ui.fit(title, 17, 13, 130, true);
        ui.text(kInnerX, top + 21, title, titleSize, rgb(kText), true);
        const std::string hint = (!ns && isVad) ? t.nsInactive : (isVad ? t.nsVadHint : t.nsGraceHint);
        const double hintSize = ui.fit(hint, 12, 10, 130);
        ui.text(kInnerX, top + 40, ui.ellipsize(hint, hintSize, 130), hintSize, rgb(kTextMuted));

        // 溝・今の値までの塗り・つまみ
        fillRounded(cr, {trackX0, cy - 3, trackX1 - trackX0, 6}, 3, kDivider);
        const double fraction = nsParamsKnown ? std::clamp((value - min) / (max - min), 0.0, 1.0) : 0.0;
        const double thumbX = trackX0 + (trackX1 - trackX0) * fraction;
        const Color fill = ns ? kAccent : kTextMuted;
        if (nsParamsKnown && thumbX > trackX0 + 1) fillRounded(cr, {trackX0, cy - 3, thumbX - trackX0, 6}, 3, fill);
        if (nsParamsKnown) {
            const bool dragged = dragAction_ == action;
            const int l = look({trackX0 - 12, top, trackX1 - trackX0 + 24, 48});
            const double r = dragged ? 14 : (l > 0 ? 13 : 12);
            if (dragged && ns) {
                setColor(cr, kAccent, 0.3);
                cairo_new_sub_path(cr);
                cairo_arc(cr, thumbX, cy, r + 6, 0, 2 * M_PI);
                cairo_fill(cr);
            }
            fillCircle(cr, thumbX, cy, r + 2, kCard);  // 塗りと見分けるための縁
            fillCircle(cr, thumbX, cy, r, ns ? kText : kTextMuted);
        }
        tracks_.push_back({action, trackX0, trackX1, min, max, cy, 0, {}, false, 0, 0});
        if (nsParamsKnown) addButton({action}, {trackX0 - 12, top, trackX1 - trackX0 + 24, 48});

        char text[32];
        if (!nsParamsKnown) {
            std::snprintf(text, sizeof(text), "--");
        } else if (isVad) {
            std::snprintf(text, sizeof(text), "%.0f%%", value);
        } else {
            std::snprintf(text, sizeof(text), "%.0fms", value);
        }
        ui.text(kInnerRight, frame_ui::centerBaseline(top, 48, 20), text, 20, rgb(ns ? kText : kTextMuted), true, true);
    }

    // 標準に戻す（SteamOS の既定 23% / 500ms）と、標準の値の説明
    const Rect reset {424, 514, 158, 44};
    const double noteSize = ui.fit(t.nsDefaultNote, 14, 11, reset.x - kInnerX - 12);
    ui.text(kInnerX, frame_ui::centerBaseline(reset.y, reset.h, noteSize), t.nsDefaultNote, noteSize, rgb(kTextMuted));
    addButton({PanelAction::NsReset}, frame_ui::drawButton(ui, reset, t.nsReset, frame_ui::ButtonKind::Normal, nsParamsKnown));
}

void MicPanel::drawExternal(const frame_ui::Canvas& ui, const UiText& t, const PanelModel& model, bool echo, bool ns,
                            const std::string& outputName) {
    const AudioDevices& devices = model.state.devices;
    const AudioEndpoint* input = devices.findInput(devices.defaultInputKey);
    const std::string micName = format1(t.micOfFormat, input != nullptr ? input->name : devices.defaultInputKey);
    const std::string now = format1(t.externalNowFormat, micName);
    const double size = ui.fit(now, 21, 16, kInnerW, true);
    ui.text(kInnerX, 268 + 23, ui.ellipsize(now, size, kInnerW, true), size, rgb(kText), true);
    drawLines(ui, kInnerX, 306 + 19, 26, wrapText(ui, t.externalExplain, 16, false, kInnerW, 3), 16, kTextSoft);
    drawLines(ui, kInnerX, 392 + 15, 21,
              wrapText(ui, format2(t.externalSavedFormat, outputName, summaryText(echo, ns, t)), 14, false, kInnerW, 2), 14,
              kTextMuted);
    // 内蔵マイクに戻す（内蔵マイクが一覧にあるときだけ押せる）
    bool builtinAvailable = false;
    for (const AudioEndpoint& e : devices.inputs) builtinAvailable |= isBuiltinMic(e.key);
    const Rect button {kInnerX, 452, std::max(300.0, ui.measure(t.useBuiltinMic, frame_ui::kControlSize, true) + 48), 56};
    addButton({PanelAction::UseBuiltinMic},
              frame_ui::drawButton(ui, button, t.useBuiltinMic, frame_ui::ButtonKind::Normal, builtinAvailable));
}

void MicPanel::drawPipeline(const frame_ui::Canvas& ui, const UiText& t, const MicState& state, bool external) {
    cairo_t* cr = ui.cr;
    const Rect box = kPipelineRect;
    fillRounded(cr, box, 14, kInset);
    const double labelBaseline = box.y + 8 + 13;
    const double labelW = ui.text(box.x + 14, labelBaseline, t.rowChain, 13, rgb(kTextMuted), true);
    std::string note;
    if (!state.loaded) {
        note = t.loading;
    } else if (!state.linksKnown) {
        note = t.chainUnknown;
    } else if (!state.inUse && !external) {
        note = t.chainIdle;
    }
    if (!note.empty()) {
        const double room = box.w - 28 - labelW - 16;
        const double size = ui.fit(note, 13, 11, room);
        ui.text(box.right() - 14, labelBaseline, ui.ellipsize(note, size, room), size, rgb(kTextMuted), false, true);
    }

    // 5 つの段: マイク ─ 音質補正 ─ エコー除去 ─ ノイズ除去 ─ アプリへ。通っている段だけピンク
    std::string micLabel = t.stageMic;
    if (external) {
        const AudioEndpoint* input = state.devices.findInput(state.devices.defaultInputKey);
        micLabel = shortDeviceName(input != nullptr ? input->name : state.devices.defaultInputKey);
    }
    bool active[5] = {external || state.linksKnown, false, false, false, external || state.linksKnown};
    if (!external) {
        for (const ChainStage& stage : state.chain) {
            if (stage.kind == ChainStage::Kind::Eq) active[1] = true;
            if (stage.kind == ChainStage::Kind::EchoCancel) active[2] = true;
            if (stage.kind == ChainStage::Kind::NoiseSuppression) active[3] = true;
        }
    }
    // ミックスがオンなら最後の段は「アプリへ」ではなく Mix。ピンクなのは、流れているとき。
    std::string outLabel = t.stageOut;
    if (state.mix.enabled) {
        outLabel = t.mixTab;
        active[4] = true;
    }
    const std::string labels[5] = {micLabel, t.stageEq, t.stageEcho, t.stageNs, outLabel};
    const double inner = box.w - 28;
    const double minWire = 6;
    double size = 15;
    double widths[5];
    double total = 0;
    for (; size >= 11; size -= 1) {
        total = 0;
        for (int i = 0; i < 5; ++i) {
            widths[i] = ui.measure(labels[i], size, active[i]) + 24;
            total += widths[i];
        }
        if (total + 4 * minWire <= inner) break;
    }
    const double wire = std::max(minWire, (inner - total) / 4);
    const double nodeH = 36;
    const double top = box.y + 8 + 19 + 6;
    const double cy = top + nodeH / 2;
    double x = box.x + 14;
    for (int i = 0; i < 5; ++i) {
        const Rect node {x, top, widths[i], nodeH};
        if (active[i]) {
            fillRounded(cr, node, nodeH / 2, kAccentTint);
            strokeRounded(cr, node, nodeH / 2, kAccent, 2);
        } else {
            strokeRounded(cr, node, nodeH / 2, kBorder, 2, true);
        }
        ui.text(node.x + 12, frame_ui::centerBaseline(node.y, node.h, size), labels[i], size,
                rgb(active[i] ? kText : kTextMuted), active[i]);
        x += widths[i];
        if (i == 4) break;
        // 段と段の線: 両側が通っていればピンクの実線、ほかは点線
        const bool on = active[i] && active[i + 1];
        setColor(cr, on ? kAccent : kBorder);
        cairo_set_line_width(cr, 3);
        if (!on) {
            const double dash[] = {6, 5};
            cairo_set_dash(cr, dash, 2, 0);
        }
        cairo_move_to(cr, x, cy);
        cairo_line_to(cr, x + wire, cy);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
        x += wire;
    }
}

void MicPanel::drawVoice(const frame_ui::Canvas& ui, const UiText& t, const VoiceView& voice, const Rect& card) {
    cairo_t* cr = ui.cr;
    frame_ui::drawCard(ui, card);
    const double x = card.x + frame_ui::kCardPadX;
    const double right = card.right() - frame_ui::kCardPadX;
    const Rect record {right - 150, 110, 150, 56};
    ui.text(x, frame_ui::centerBaseline(110, 32, frame_ui::kCardTitleSize), t.voiceTitle,
            ui.fit(t.voiceTitle, frame_ui::kCardTitleSize, 16, record.x - x - 12, true), rgb(kText), true);
    const double hintSize = ui.fit(t.voiceHint, 14, 11, record.x - x - 12);
    ui.text(x, 142 + 15, ui.ellipsize(t.voiceHint, hintSize, record.x - x - 12), hintSize, rgb(kTextMuted));

    // 録音のボタン: 待機中は赤い枠に「● 録音」、録音中は赤い塗りに「■ 停止」
    const Rect recordHit = frame_ui::drawButton(ui, record, "",
                                                voice.recording ? frame_ui::ButtonKind::DangerArmed : frame_ui::ButtonKind::Danger);
    {
        const std::string label = voice.recording ? t.stop : t.record;
        const double size = ui.fit(label, 21, 14, record.w - 60, true);
        const double w = 16 + 8 + ui.measure(label, size, true);
        const double bx = record.x + (record.w - w) / 2;
        const double cy = record.y + record.h / 2;
        if (voice.recording) {
            drawStopIcon(cr, bx + 8, cy, 14, kOnAccent);
        } else {
            fillCircle(cr, bx + 8, cy, 8, kDanger);
        }
        ui.text(bx + 24, frame_ui::centerBaseline(record.y, record.h, size), label, size,
                rgb(voice.recording ? kOnAccent : kText), true);
    }
    addButton({PanelAction::Record}, recordHit);

    // 録音のボタンの下の行: 録音中は「● 録音中」・経過と残り・ピークのメーター、失敗は赤、ほかは説明
    const double lineTop = 180;
    const double lineH = 28;
    const double lineBaseline = frame_ui::centerBaseline(lineTop, lineH, 14);
    if (voice.recording) {
        fillCircle(cr, x + 6, lineTop + lineH / 2, 6, kDanger);
        double cursor = x + 18 + ui.text(x + 18, lineBaseline, t.recording, 15, rgb(kDangerText), true);
        char elapsed[64];
        std::snprintf(elapsed, sizeof(elapsed), t.elapsedFormat, voice.recordSec,
                      std::max(0.0, kVoiceMaxSec - voice.recordSec));
        cursor += 12;
        cursor += ui.text(cursor, lineBaseline, elapsed, 14, rgb(kText));
        char level[32];
        std::snprintf(level, sizeof(level), "%.1f dBFS", std::max(-60.0f, voice.levelDb));
        const double levelW = ui.measure("-60.0 dBFS", 14);
        ui.text(right, lineBaseline, level, 14, rgb(kText), false, true);
        const double barX = cursor + 14;
        const double barW = right - levelW - 12 - barX;
        if (barW > 30) {
            const Rect bar {barX, lineTop + lineH / 2 - 7, barW, 14};
            fillRounded(cr, bar, 7, kBg);
            const double fraction = std::clamp((voice.levelDb + 60.0) / 60.0, 0.0, 1.0);
            if (fraction > 0) fillRounded(cr, {bar.x, bar.y, std::max(bar.h, bar.w * fraction), bar.h}, 7, kAccent);
            strokeRounded(cr, bar, 7, kBorder, 1.5);
        }
    } else if (voice.error != VoiceError::None) {
        const std::string message = voice.error == VoiceError::Record ? t.errRecord : t.errPlay;
        const double size = ui.fit(message, 15, 11, right - x, true);
        ui.text(x, lineBaseline, ui.ellipsize(message, size, right - x, true), size, rgb(kDangerText), true);
    } else {
        const double size = ui.fit(t.voiceIdle, 14, 11, right - x);
        ui.text(x, lineBaseline, ui.ellipsize(t.voiceIdle, size, right - x), size, rgb(kTextMuted));
    }

    // 履歴（新しい順に 5 件まで）
    fillRounded(cr, {x, 216, right - x, 1}, 0, kDivider);
    if (voice.clips.empty()) ui.text(x, 226 + 40, t.noClips, 16, rgb(kTextMuted));
    for (size_t i = 0; i < voice.clips.size() && i < kVoiceHistory; ++i) {
        const VoiceClip& clip = *voice.clips[i];
        const double top = 226 + i * 84.0;
        const double cy = top + 36;
        const bool playing = voice.playing && voice.playingId == clip.id;
        const int index = static_cast<int>(i);
        // ▶ / ■ のボタン（丸）
        const Rect play {x, cy - 26, 52, 52};
        const int l = look(play);
        if (playing) {
            fillCircle(cr, play.x + 26, cy, 26, l == 2 ? kAccentPressed : (l == 1 ? kAccentHover : kAccent));
            drawStopIcon(cr, play.x + 26, cy, 14, kOnAccent);
        } else {
            if (l > 0) fillCircle(cr, play.x + 26, cy, 26, l == 2 ? kControlDown : kControlHover);
            setColor(cr, kBorder);
            cairo_set_line_width(cr, 2);
            cairo_new_sub_path(cr);
            cairo_arc(cr, play.x + 26, cy, 25, 0, 2 * M_PI);
            cairo_stroke(cr);
            drawPlayIcon(cr, play.x + 27, cy, 16, kText);
        }
        addButton({PanelAction::Play, index}, play);

        // 時刻・長さ（1 行目）と、録ったときの出口と設定（2 行目。例:「AB13X・エコー除去オフ」）
        const double tx = play.right() + 14;
        const double textW = std::min(176.0, std::max(72.0, (right - tx) * 0.46));
        const double timeW = ui.text(tx, top + 31, clockText(clip.recordedAt), 18, rgb(kText), true);
        char length[32];
        std::snprintf(length, sizeof(length), "%.1f %s", clip.seconds(), t.seconds);
        ui.text(tx + timeW + 8, top + 31, length, 13, rgb(kTextMuted));
        std::string label;
        if (clip.externalMic) {
            label = format1(t.clipExternalFormat, shortDeviceName(clip.micName));
        } else if (!clip.echoKnown) {
            label = t.unknownSetting;
        } else {
            label = summaryText(clip.echo, clip.nsKnown && clip.ns, t);
            if (clip.nsKnown && clip.ns && clip.nsParamsKnown) {
                char numbers[32];
                std::snprintf(numbers, sizeof(numbers), " %.0f%%/%.0fms", clip.nsVad, clip.nsGrace);
                label += numbers;
            }
            if (clip.outputKnown) {
                const std::string output = clip.outputSpeaker ? std::string(t.speaker) : shortDeviceName(clip.outputName);
                if (!output.empty()) label = output + t.labelSeparator + label;
            }
        }
        const double labelSize = ui.fit(label, 13, 11, textW);
        ui.text(tx, top + 54, ui.ellipsize(label, labelSize, textW), labelSize, rgb(kTextSoft));

        // 波形（3px の棒を 2px おき）。再生中は再生済みの部分をピンクに
        const double waveX = tx + textW + 14;
        const double waveW = right - waveX;
        const int bars = std::max(1, static_cast<int>((waveW + 2) / 5));
        const double playedX = playing ? waveX + waveW * std::clamp(voice.playSec / std::max(0.01, clip.seconds()), 0.0, 1.0)
                                       : waveX;
        const int bins = static_cast<int>(clip.wave.size());
        for (int b = 0; b < bars && bins > 0; ++b) {
            float peak = 0.0f;
            for (int k = b * bins / bars; k < std::max(b * bins / bars + 1, (b + 1) * bins / bars) && k < bins; ++k) {
                peak = std::max(peak, clip.wave[k]);
            }
            const double bx = waveX + b * 5;
            const double amplitude = std::sqrt(std::clamp(static_cast<double>(peak), 0.0, 1.0));  // 小さい音も見えるように
            const double bh = std::max(6.0, amplitude * 44);
            fillRounded(cr, {bx, cy - bh / 2, 3, bh}, 1.5, playing && bx < playedX ? kAccent : kTextMuted);
        }
    }
}

// ============================================================================
// アプリと更新
// ============================================================================

void MicPanel::drawUpdateBox(const frame_ui::Canvas& ui, const UiText& t, frame_ui::Lang lang, const Config& config,
                             const frame_updater::UpdateStatus& update, Rect box) {
    using frame_updater::UpdateState;
    cairo_t* cr = ui.cr;
    const frame_ui::Strings& s = frame_ui::strings(lang);
    const auto fill = [](const char* format, const std::string& arg) { return format1(format, arg); };

    /** 箱の右に並べるボタン 1 つ。 */
    struct BoxButton {
        PanelAction action;
        std::string label;
        frame_ui::ButtonKind kind;
        bool enabled;
    };
    std::string line;
    std::string hint = config.updateCheck ? t.updateAutoHint : t.updateOffHint;
    Color lineColor = kTextSoft;
    bool lineBold = false;
    Color edge = kInset;
    bool hasEdge = false;
    std::vector<BoxButton> buttons;
    const BoxButton checkNow {PanelAction::UpdateCheckNow, s.updateCheckNow, frame_ui::ButtonKind::Normal, !update.checking};
    switch (update.state) {
        case UpdateState::Installing:
            line = fill(s.updateInstallingFormat, frame_ui::updateStepText(lang, update.step) + "…");
            hint = t.updateConfirmHint;  // 確認のときと同じ補足（終わったら起動し直す）
            break;
        case UpdateState::Installed:
            line = fill(t.updateInstalledFormat, update.version);
            lineColor = kText;
            lineBold = true;
            edge = kAccent;
            hasEdge = true;
            hint = t.updateInstalledHint;
            buttons.push_back({PanelAction::UpdateDismiss, s.updateDismiss, frame_ui::ButtonKind::Normal, true});
            break;
        case UpdateState::InstallFailed:
            line = std::string(s.updateInstallFailed) + " " + frame_ui::updateErrorText(lang, update.error);
            lineColor = kDangerText;
            lineBold = true;
            edge = kDanger;
            hasEdge = true;
            hint.clear();
            buttons.push_back({PanelAction::UpdateRetry, s.updateRetry, frame_ui::ButtonKind::Normal, true});
            buttons.push_back({PanelAction::UpdateDismiss, s.updateDismiss, frame_ui::ButtonKind::Normal, true});
            break;
        case UpdateState::Available:
            edge = kAccent;
            hasEdge = true;
            lineBold = true;
            if (update.installable && updateArmed_) {
                line = fill(s.updateConfirmFormat, update.latest);
                lineColor = kText;
                hint = t.updateConfirmHint;
                buttons.push_back({PanelAction::UpdateConfirmNo, s.updateConfirmNo, frame_ui::ButtonKind::Normal, true});
                buttons.push_back({PanelAction::UpdateConfirmYes, s.updateConfirmYes, frame_ui::ButtonKind::Primary, true});
            } else if (update.installable) {
                // 箱の幅に合わせて、ボタンは「更新する」だけ（新しい版はもう分かっているので「今すぐ確かめる」は出さない）
                line = fill(s.updateAvailableFormat, update.latest);
                lineColor = kAccentText;
                hint.clear();
                buttons.push_back({PanelAction::UpdateInstall, s.updateButton, frame_ui::ButtonKind::Primary, true});
            } else {
                line = fill(s.updateAvailableFormat, update.latest);
                lineColor = kAccentText;
                hint = s.updateManual;
                if (!update.url.empty()) hint += std::string("  ") + t.updateReleasePage + update.url;
                buttons.push_back(checkNow);
            }
            break;
        case UpdateState::UpToDate:
            line = update.checking ? s.updateChecking : fill(s.updateUpToDateFormat, update.current);
            buttons.push_back(checkNow);
            break;
        case UpdateState::CheckFailed:
            if (update.checking) {
                line = s.updateChecking;
            } else {
                line = std::string(s.updateCheckFailed) + " " + frame_ui::updateErrorText(lang, update.error);
                lineColor = kDangerText;
            }
            buttons.push_back(checkNow);
            break;
        case UpdateState::Unknown:
            line = update.checking ? s.updateChecking : fill(s.updateCurrentFormat, update.current);
            buttons.push_back(checkNow);
            break;
    }

    fillRounded(cr, box, 14, kInset);
    if (hasEdge) strokeRounded(cr, box, 14, edge, 2);
    // ボタンは右から（右の余白 18、間 10、縦は真ん中）
    double left = box.right() - 18;
    for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
        const double w = std::max(110.0, ui.measure(it->label, frame_ui::kControlSize, true) + 40);
        left -= w;
        const Rect hit = frame_ui::drawButton(ui, {left, box.y + (box.h - 56) / 2, w, 56}, it->label, it->kind, it->enabled);
        addButton({it->action}, hit);
        left -= 10;
    }
    const double textX = box.x + 20;
    const double room = (buttons.empty() ? box.right() - 20 : left - 6) - textX;
    // 文は 2 行まで（補足が無ければ 4 行まで）折り返し、補足は箱の高さに入るだけ（3 行まで）出す
    const double lineSize = ui.fit(line, frame_ui::kControlSize, 15, room, lineBold);
    const double rowH = lineSize + 7;
    const double hintH = 19;
    const std::vector<std::string> lineRows = wrapText(ui, line, lineSize, lineBold, room, hint.empty() ? 4 : 2);
    const int hintRoom = static_cast<int>((box.h - 16 - lineRows.size() * rowH - 6) / hintH);
    const std::vector<std::string> hintRows =
        hint.empty() || hintRoom < 1 ? std::vector<std::string>() : wrapText(ui, hint, 14, false, room, std::min(3, hintRoom));
    const double blockH = lineRows.size() * rowH + (hintRows.empty() ? 0 : 6 + hintRows.size() * hintH);
    double y = box.y + (box.h - blockH) / 2;
    for (const std::string& row : lineRows) {
        ui.text(textX, y + lineSize, row, lineSize, rgb(lineColor), lineBold);
        y += rowH;
    }
    if (!hintRows.empty()) drawLines(ui, textX, y + 6 + 14, hintH, hintRows, 14, kTextMuted);
}

void MicPanel::drawApps(const frame_ui::Canvas& ui, const UiText& t, const Config& config, const PanelModel& model) {
    cairo_t* cr = ui.cr;
    const frame_ui::Lang lang = uiLang(config.language);
    frame_ui::drawCard(ui, kAppCard);
    const double x = kAppCard.x + frame_ui::kCardPadX;
    const double right = kAppCard.right() - frame_ui::kCardPadX;
    ui.text(x, frame_ui::centerBaseline(110, 32, frame_ui::kCardTitleSize), t.thisApp, frame_ui::kCardTitleSize, rgb(kText),
            true);

    // アイコン（サムネイルと同じ絵）・名前・版・説明
    cairo_save(cr);
    cairo_translate(cr, x, 156);
    cairo_scale(cr, 64.0 / 256, 64.0 / 256);
    cairo_set_source_surface(cr, appIcon_, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
    const double nameW = ui.text(132, 160 + 24, "Frame Mic Tuner", 22, rgb(kText), true);
    ui.text(132 + nameW + 8, 160 + 24, model.update.current, 16, rgb(kTextMuted));
    const double descSize = ui.fit(t.thisAppDesc, 14, 11, right - 132);
    ui.text(132, 194 + 15, ui.ellipsize(t.thisAppDesc, descSize, right - 132), descSize, rgb(kTextMuted));

    drawUpdateBox(ui, t, lang, config, model.update, kUpdateBox);

    // 新しい版の確認（オン / オフ。オフならアプリの一覧も取りに行かない）
    const double labelSize = ui.fit(t.rowUpdateCheck, 18, 13, 216 - x - 10, true);
    ui.text(x, frame_ui::centerBaseline(380, 56, labelSize), t.rowUpdateCheck, labelSize, rgb(kText), true);
    const std::vector<Rect> toggle = frame_ui::drawSegmented(ui, {216, 380, 180, 56}, {t.on, t.off}, config.updateCheck ? 0 : 1);
    addButton({PanelAction::UpdateCheckOn}, toggle[0]);
    addButton({PanelAction::UpdateCheckOff}, toggle[1]);
    drawLines(ui, x, 446 + 15, 21, wrapText(ui, t.updateCheckNote, 14, false, right - x, 2), 14, kTextMuted);

    fillRounded(cr, {x, 508, right - x, 1}, 0, kDivider);
    ui.text(x, 526 + 15, t.helpTitle, 14, rgb(kTextMuted));
    ui.text(x, 548 + 15, "github.com/sasaken1102r/frame-mic-tuner", 14, rgb(kTextSoft));

    drawRelatedApps(ui, t, config, model);
}

void MicPanel::drawAppRow(const frame_ui::Canvas& ui, const UiText& t, Language language, const frame_apps::Entry& entry,
                          Rect row, bool inList) {
    using frame_apps::AppState;
    cairo_t* cr = ui.cr;
    fillRounded(cr, row, 16, kInset);
    const double cy = row.y + row.h / 2;
    // 左: アイコン（取ってきたもの → 同梱のもの。どちらも無ければ札の文字）
    const double tile = 52;
    const double tx = row.x + 16;
    if (!frame_apps::drawIcon(cr, entry, tx, cy - tile / 2, tile)) {
        fillRounded(cr, {tx, cy - tile / 2, tile, tile}, 13, kIconBox);
        const double monoSize = ui.fit(entry.app.mono, 16, 11, tile - 8, true);
        const double w = ui.measure(entry.app.mono, monoSize, true);
        ui.text(tx + (tile - w) / 2, frame_ui::centerBaseline(cy - tile / 2, tile, monoSize), entry.app.mono, monoSize,
                rgb(kAccent), true);
    }
    // 右: 「入れる」か状態の札
    double rightEdge = row.right() - 16;
    if (entry.state == AppState::Missing && !entry.busy) {
        const double w = std::max(110.0, ui.measure(t.appsInstall, frame_ui::kControlSize, true) + 40);
        const Rect button {rightEdge - w, cy - frame_ui::kControlH / 2, w, frame_ui::kControlH};
        const Rect hit = frame_ui::drawButton(ui, button, t.appsInstall, frame_ui::ButtonKind::Primary);
        addButton({PanelAction::AppInstall, 0, entry.app.key}, inList ? intersect(hit, listRect_) : hit, inList);
        rightEdge = button.x - 16;
    } else if (entry.state != AppState::Unknown || entry.busy) {
        const char* label = entry.busy ? t.appsChipBusy : (entry.state == AppState::Running ? t.appsChipRunning : t.appsChipInstalled);
        const bool ok = !entry.busy && entry.state == AppState::Running;
        const double size = entry.busy ? 15 : 16;
        const double dot = ok ? 12 + 8 : 0;
        const double w = 18 + dot + ui.measure(label, size, true) + 18;
        const Rect chip {rightEdge - w, cy - 22, w, 44};
        fillRounded(cr, chip, 22, ok ? kSuccessTint : kIdleFill);
        if (ok) fillCircle(cr, chip.x + 18 + 6, cy, 6, kSuccess);
        ui.text(chip.x + 18 + dot, frame_ui::centerBaseline(chip.y, chip.h, size), label, size,
                rgb(ok ? kSuccess : kIdleText), true);
        rightEdge = chip.x - 16;
    }
    // 真ん中: 名前（一覧では「このアプリで使う」の札も）と、説明か理由（簡体字中国語の説明は英語）
    const double textX = tx + tile + 16;
    const double textW = std::max(40.0, rightEdge - textX);
    const double nameSize = 19;
    std::string tag;
    double tagW = 0;
    if (inList && entry.related) {
        tag = t.appsRelatedTag;
        tagW = ui.measure(tag, 13, true) + 20;
    }
    const double nameW = ui.measure(entry.app.name, nameSize, true);
    const bool tagFits = !tag.empty() && nameW + 10 + tagW <= textW;
    const std::string name = ui.ellipsize(entry.app.name, nameSize, tagFits ? textW - 10 - tagW : textW, true);
    const double drawnW = ui.text(textX, cy - 4, name, nameSize, rgb(kText), true);
    if (tagFits) {
        const Rect tagRect {textX + drawnW + 10, cy - 4 - 17, tagW, 24};
        fillRounded(cr, tagRect, 12, kAccentTint);
        ui.text(tagRect.x + 10, frame_ui::centerBaseline(tagRect.y, tagRect.h, 13), tag, 13, rgb(kAccentText), true);
    }
    // frame-apps は日本語と英語だけ。ほかの言語は英語の説明。
    const frame_apps::Lang appsLang = language == Language::Ja ? frame_apps::Lang::Ja : frame_apps::Lang::En;
    std::string sub = entry.app.desc(appsLang);
    if (!inList && entry.related) sub = entry.app.name == "frame-aux-shortcuts" ? std::string(t.appsReasonAux) : entry.reason(appsLang);
    const double subSize = ui.fit(sub, 14, 11, textW);
    ui.text(textX, cy + 20, ui.ellipsize(sub, subSize, textW), subSize, rgb(kTextMuted));
}

void MicPanel::drawRelatedApps(const frame_ui::Canvas& ui, const UiText& t, const Config& config, const PanelModel& model) {
    cairo_t* cr = ui.cr;
    frame_ui::drawCard(ui, kRelatedCard);
    const double x = kRelatedCard.x + frame_ui::kCardPadX;
    const double right = kRelatedCard.right() - frame_ui::kCardPadX;
    const double w = right - x;
    ui.text(x, frame_ui::centerBaseline(110, 32, frame_ui::kCardTitleSize), t.appsRelatedTitle,
            ui.fit(t.appsRelatedTitle, frame_ui::kCardTitleSize, 16, w, true), rgb(kText), true);
    const double hintSize = ui.fit(t.appsRelatedHint, 14, 11, w);
    ui.text(x, 142 + 15, ui.ellipsize(t.appsRelatedHint, hintSize, w), hintSize, rgb(kTextMuted));

    // このアプリで使うものだけ（理由付き）
    double y = 178;
    for (const frame_apps::Entry& entry : model.apps) {
        if (!entry.related || y + 96 > 560) continue;
        drawAppRow(ui, t, config.language, entry, {x, y, w, 96}, false);
        y += 96 + 16;
    }

    // 「ささけんの Frame アプリ　N 個のうち M 個が入っていません ›」（押すと一覧）
    const Rect open {x, y, w, 88};
    const int l = look(open);
    fillRounded(cr, open, 16, l == 2 ? kControlDown : (l == 1 ? kControlHover : kControl));
    strokeRounded(cr, open, 16, kBorder, 2);
    double iconX = open.x + 18;
    int shown = 0;
    for (const frame_apps::Entry& entry : model.apps) {
        if (shown == 4) break;
        // 重ねたアイコン（下のボタンの色の縁で区切る）
        fillRounded(cr, {iconX - 2, open.y + 26 - 2, 40, 40}, 11, kControl);
        if (!frame_apps::drawIcon(cr, entry, iconX, open.y + 26, 36)) {
            fillRounded(cr, {iconX, open.y + 26, 36, 36}, 9, kIconBox);
        }
        iconX += 24;
        ++shown;
    }
    const double textX = open.x + 18 + (shown > 0 ? 24 * (shown - 1) + 36 : 0) + 16;
    int missing = 0;
    for (const frame_apps::Entry& entry : model.apps) missing += entry.state == frame_apps::AppState::Missing ? 1 : 0;
    char count[200];
    if (missing > 0) {
        std::snprintf(count, sizeof(count), t.appsAllCountFormat, static_cast<int>(model.apps.size()), missing);
    } else {
        std::snprintf(count, sizeof(count), t.appsAllInstalledFormat, static_cast<int>(model.apps.size()));
    }
    const double textW = open.right() - 18 - 20 - 12 - textX;
    const double titleSize = ui.fit(t.appsAllTitle, 19, 15, textW, true);
    ui.text(textX, open.y + 40, ui.ellipsize(t.appsAllTitle, titleSize, textW, true), titleSize, rgb(kText), true);
    ui.text(textX, open.y + 64, ui.ellipsize(count, 14, textW), 14, rgb(kTextMuted));
    drawPolyline(cr, open.right() - 18 - 20, open.y + open.h / 2 - 10, 20, 20, kText, 2.4, {7, 4, 13, 10, 7, 16});
    addButton({PanelAction::AppsOpenList}, open);

    drawLines(ui, x, open.bottom() + 18 + 15, 22, wrapText(ui, t.appsInstallNote, 14, false, w, 3), 14, kTextMuted);
}

void MicPanel::drawAppsList(const frame_ui::Canvas& ui, const UiText& t, const Config& config, const PanelModel& model) {
    cairo_t* cr = ui.cr;
    drawModal(ui, kAppsModal, t.appsAllTitle, t.appsListSub);
    const double contentH = model.apps.empty() ? 0 : model.apps.size() * (kAppRowH + kAppRowGap) - kAppRowGap;
    beginList(kAppsList, contentH);
    cairo_save(cr);
    cairo_rectangle(cr, kAppsList.x - 2, kAppsList.y, kAppsList.w + 4, kAppsList.h);
    cairo_clip(cr);
    double y = kAppsList.y - listScroll_;
    const double rowW = listMaxScroll_ > 0 ? kAppsList.w - 16 : kAppsList.w;
    for (const frame_apps::Entry& entry : model.apps) {
        if (y + kAppRowH >= kAppsList.y && y <= kAppsList.bottom()) {
            drawAppRow(ui, t, config.language, entry, {kAppsList.x, y, rowW, kAppRowH}, true);
        }
        y += kAppRowH + kAppRowGap;
    }
    cairo_restore(cr);
    endList(ui, contentH, kAppsList.right() - 6);

    // 下: 補足と「インストーラーを開く」（メニューの Konsole がまだ開いていれば押せない）
    fillRounded(cr, {kAppsList.x, 556, kAppsList.w, 1}, 0, kDivider);
    const Rect menu {884, 648, 232, 56};
    drawLines(ui, kAppsList.x, 574 + 14, 20, wrapText(ui, t.appsListNote, 13, false, 760, 3), 13, kTextMuted);
    const std::string label = model.menuBusy ? t.appsChipBusy : t.appsOpenInstaller;
    addButton({PanelAction::AppsOpenMenu}, frame_ui::drawButton(ui, menu, label, frame_ui::ButtonKind::Normal, !model.menuBusy));
}

void MicPanel::drawAppsConfirm(const frame_ui::Canvas& ui, const UiText& t, const Config& /*config*/,
                               const PanelModel& model) {
    using frame_apps::LaunchResult;
    cairo_t* cr = ui.cr;
    setColor(cr, kBackdrop, kBackdropAlpha);
    frame_ui::roundedRect(cr, {0, 0, static_cast<double>(kWidth), static_cast<double>(kHeight)}, 24);
    cairo_fill(cr);
    const Rect card = kConfirmCard;
    fillRounded(cr, card, 18, kCard);
    strokeRounded(cr, card, 18, kBorder, 2);
    const double x = card.x + 30;
    const double w = card.w - 60;

    // アプリ名とコマンドは、今の一覧（取ってきたものかもしれない）から呼び名で引く。Konsole に渡すのと同じ関数で作る
    const frame_apps::Entry* entry = nullptr;
    for (const frame_apps::Entry& e : model.apps) {
        if (!confirmKey_.empty() && e.app.key == confirmKey_) entry = &e;
    }
    const bool unknownKey = !confirmKey_.empty() && entry == nullptr;  // 一覧が変わって消えた: 開かせない
    double y = card.y + 28;
    double titleX = x;
    if (entry != nullptr) {
        frame_apps::drawIcon(cr, *entry, x, y, 44);
        titleX = x + 44 + 14;
    }
    const std::string title = entry != nullptr ? format1(t.appsConfirmTitleFormat, entry->app.name) : std::string(t.appsConfirmMenuTitle);
    const double titleSize = ui.fit(title, 24, 16, card.right() - 30 - titleX, true);
    ui.text(titleX, frame_ui::centerBaseline(y, 44, titleSize), ui.ellipsize(title, titleSize, card.right() - 30 - titleX, true),
            titleSize, rgb(kText), true);
    y += 44 + 22;
    ui.text(x, y + 18, ui.ellipsize(t.appsConfirmLead, 17, w), 17, rgb(kTextSoft));
    y += 25 + 10;
    // 実行するコマンド（等幅。入りきらなければ空白で折り返す）
    const frame_ui::Canvas mono {cr, fonts_.mono(), fonts_.mono(), frame_ui::Pointer {}};
    const std::string command = frame_apps::installerCommand(entry != nullptr ? entry->app.key : std::string());
    const std::vector<std::string> lines = wrapText(mono, command, 15, false, w - 32, 3);
    const Rect code {x, y, w, 28 + lines.size() * 22.0};
    fillRounded(cr, code, 12, kBg);
    strokeRounded(cr, code, 12, kDivider, 1);
    drawLines(mono, x + 16, y + 14 + 16, 22, lines, 15, kText);
    y = code.bottom() + 18;
    // 補足 3 つ（進み具合かメニュー・sudo を使わない・終わったら閉じる）
    for (const char* note : {entry != nullptr ? t.appsConfirmProgress : t.appsConfirmMenu, t.appsConfirmNoSudo, t.appsConfirmClose}) {
        const std::vector<std::string> rows = wrapText(ui, note, 14, false, w, 2);
        drawLines(ui, x, y + 16, 23, rows, 14, kTextMuted);
        y += rows.size() * 23.0;
    }
    // 開けなかった理由（赤）
    const char* error = unknownKey ? t.appsErrorFailed : nullptr;
    switch (launchResult_) {
        case LaunchResult::Started: break;
        case LaunchResult::Busy: error = t.appsErrorBusy; break;
        case LaunchResult::NoDisplay: error = t.appsErrorNoDisplay; break;
        case LaunchResult::NoKonsole: error = t.appsErrorNoKonsole; break;
        case LaunchResult::UnknownApp:
        case LaunchResult::Failed: error = t.appsErrorFailed; break;
    }
    if (error != nullptr) {
        const double size = ui.fit(error, 14, 11, w, true);
        ui.text(x, 544 - 14, ui.ellipsize(error, size, w, true), size, rgb(kDangerText), true);
    }
    // やめる・Konsole で開く（同じアプリ・メニューの Konsole がまだ開いていれば開けない）
    const bool busy = unknownKey || (entry == nullptr && model.menuBusy) || (entry != nullptr && entry->busy);
    addButton({PanelAction::AppsConfirmCancel}, frame_ui::drawButton(ui, {560, 544, 150, 56}, t.appsConfirmCancel));
    addButton({PanelAction::AppsConfirmLaunch, 0, confirmKey_},
              frame_ui::drawButton(ui, {724, 544, 186, 56}, t.appsConfirmLaunch, frame_ui::ButtonKind::Primary, !busy));
}

// ============================================================================
// 一番下の行
// ============================================================================

void MicPanel::drawFooter(const frame_ui::Canvas& ui, const UiText& t, const Config& config, const MicState& state) {
    cairo_t* cr = ui.cr;
    const Rect r = kFooterRect;
    const double controlY = r.y + (frame_ui::kFooterRowH - frame_ui::kControlH) / 2;
    const double icon = 22;
    // 言語の絵は足の文字色。ドロップダウンはその右。
    drawLanguageIcon(cr, r.x, controlY + (frame_ui::kControlH - icon) / 2, icon, kLabelSoft);
    const Rect drop {r.x + icon + 10, controlY, 220, frame_ui::kControlH};
    const int dropLook = look(drop);
    const Color dropFill = dropLook == 2 ? kControlDown : (dropLook == 1 || languageOpen_ ? kControlHover : kControl);
    fillRounded(cr, drop, 16, dropFill);
    strokeRounded(cr, drop, 16, languageOpen_ ? kAccent : kBorder, 2);
    const std::string current = languageEndonym(config.language);
    const double nameSize = ui.fit(current, 18, 13, drop.w - 46, true);
    ui.text(drop.x + 16, frame_ui::centerBaseline(drop.y, drop.h, nameSize), current, nameSize, rgb(kText), true);
    cairo_save(cr);
    setColor(cr, kTextMuted);
    cairo_set_line_width(cr, 2);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    const double chevronX = drop.right() - 26;
    const double chevronY = drop.y + drop.h / 2 - 3;
    if (languageOpen_) {
        cairo_move_to(cr, chevronX, chevronY + 5);
        cairo_line_to(cr, chevronX + 5, chevronY);
        cairo_line_to(cr, chevronX + 10, chevronY + 5);
    } else {
        cairo_move_to(cr, chevronX, chevronY);
        cairo_line_to(cr, chevronX + 5, chevronY + 5);
        cairo_line_to(cr, chevronX + 10, chevronY);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
    addButton({PanelAction::LanguageMenu}, drop);

    const bool autoEnabled = state.autostart == Autostart::Enabled || state.autostart == Autostart::Disabled;
    const std::string quitLabel = quitArmed_ ? t.footerQuitConfirm : t.footerQuit;
    const double quitW = quitArmed_ ? frame_ui::buttonWidth(ui, quitLabel, 160) : 160;
    const Rect quitRect {r.right() - quitW, controlY, quitW, frame_ui::kControlH};
    addButton({PanelAction::Quit},
              frame_ui::drawButton(ui, quitRect, quitLabel, quitArmed_ ? frame_ui::ButtonKind::DangerArmed : frame_ui::ButtonKind::Danger));
    const double autoW = 180;
    const double autoX = quitRect.x - 18 - autoW;
    const int autostart = state.autostart == Autostart::Enabled ? 0 : state.autostart == Autostart::Disabled ? 1 : -1;
    const std::vector<Rect> onOff =
        frame_ui::drawSegmented(ui, {autoX, controlY, autoW, frame_ui::kControlH}, {t.on, t.off}, autostart, autoEnabled);
    addButton({PanelAction::AutostartOn}, onOff[0]);
    addButton({PanelAction::AutostartOff}, onOff[1]);
    // 「SteamVR と一緒に起動」は言語の横ではなく、オン・オフのすぐ左。
    const double labelRight = autoX - 14;
    const double labelRoom = std::max(1.0, labelRight - (drop.right() + 18));
    const double labelSize = ui.fit(t.footerStart, frame_ui::kFooterLabelSize, 12, labelRoom);
    const std::string startLabel = ui.ellipsize(t.footerStart, labelSize, labelRoom);
    ui.text(labelRight - ui.measure(startLabel, labelSize), frame_ui::centerBaseline(r.y, frame_ui::kFooterRowH, labelSize),
            startLabel, labelSize, rgb(kLabelSoft));

    // 注意書き（1 行だけ）: 失敗（赤）があればそれ、無ければ自動起動が使えない理由、どちらも無ければ説明
    std::string note = t.footer;
    bool noteIsError = false;
    const MicError error = state.writeError != MicError::None ? state.writeError : state.readError;
    if (error != MicError::None) {
        note = errorText(error, t);
        noteIsError = true;
    } else if (state.loaded && !autoEnabled) {
        note = state.autostart == Autostart::Missing ? t.autostartMissing : t.autostartUnknown;
    }
    if (!note.empty()) {
        const double size = ui.fit(note, frame_ui::kHintSize, 12, r.w, noteIsError);
        ui.text(r.x, r.y + frame_ui::kFooterRowH + 8 + 15, ui.ellipsize(note, size, r.w, noteIsError), size,
                rgb(noteIsError ? kDangerText : kTextMuted), noteIsError);
    }
}

void MicPanel::drawLanguageMenu(const frame_ui::Canvas& ui, Language current) {
    Rect drop {};
    for (const auto& button : buttons_) {
        if (button.hit.action == PanelAction::LanguageMenu) drop = button.rect;
    }
    if (drop.w <= 0) return;
    cairo_t* cr = ui.cr;
    constexpr int count = static_cast<int>(sizeof(kLanguages) / sizeof(kLanguages[0]));
    const double rowH = 44;
    const double pad = 6;
    const double listH = pad * 2 + count * rowH;
    const Rect box {drop.x, drop.y - 8 - listH, drop.w, listH};
    fillRounded(cr, box, 16, kCard);
    strokeRounded(cr, box, 16, kBorder, 2);
    for (int i = 0; i < count; ++i) {
        const Language language = kLanguages[i];
        const Rect row {box.x + 4, box.y + pad + i * rowH, box.w - 8, rowH};
        const bool selected = language == current;
        const int hover = look(row);
        if (selected) {
            fillRounded(cr, row, 10, kAccentTint);
        } else if (hover > 0) {
            fillRounded(cr, row, 10, hover == 2 ? kControlDown : kControlHover);
        }
        const char* name = languageEndonym(language);
        const double size = ui.fit(name, 18, 13, row.w - 44, true);
        ui.text(row.x + 12, frame_ui::centerBaseline(row.y, row.h, size), name, size, rgb(selected ? kAccentText : kText), true);
        if (selected) frame_ui::drawCheck(cr, row.right() - 16, row.y + row.h / 2, 14, frame_ui::kAccent);
        addButton({PanelAction::LanguagePick, static_cast<int>(language), languageCode(language)}, row);
    }
}

// ============================================================================
// 重ねた画面
// ============================================================================

double MicPanel::drawModal(const frame_ui::Canvas& ui, Rect r, const std::string& title, const std::string& sub) {
    cairo_t* cr = ui.cr;
    setColor(cr, kBackdrop, kBackdropAlpha);
    frame_ui::roundedRect(cr, {0, 0, static_cast<double>(kWidth), static_cast<double>(kHeight)}, 24);
    cairo_fill(cr);
    fillRounded(cr, r, 18, kCard);
    strokeRounded(cr, r, 18, kBorder, 2);
    const Rect close {r.right() - 28 - 56, r.y + 20, 56, 56};
    const double textRight = close.x - 16;
    const double titleSize = ui.fit(title, 24, 16, textRight - (r.x + 28), true);
    ui.text(r.x + 28, r.y + 24 + 26, title, titleSize, rgb(kText), true);
    const double subSize = ui.fit(sub, 14, 11, textRight - (r.x + 28));
    ui.text(r.x + 28, r.y + 60 + 15, ui.ellipsize(sub, subSize, textRight - (r.x + 28)), subSize, rgb(kTextMuted));
    addButton({PanelAction::CloseOverlay}, frame_ui::drawButton(ui, close, ""));
    drawPolyline(cr, close.x + 19, close.y + 19, 18, 18, kText, 2.4, {4, 4, 14, 14, NAN, NAN, 14, 4, 4, 14});
    return r.y + 92;
}

void MicPanel::beginList(Rect box, double content, bool horizontal) {
    listRect_ = box;
    listHorizontal_ = horizontal;
    const double view = horizontal ? box.w : box.h;
    listMaxScroll_ = std::max(0.0, content - view);
    listScroll_ = std::clamp(listScroll_, 0.0, listMaxScroll_);
}

void MicPanel::endList(const frame_ui::Canvas& ui, double contentH, double barX) {
    if (listMaxScroll_ <= 0) return;
    cairo_t* cr = ui.cr;
    const Rect box = listRect_;
    if (listHorizontal_) {
        // 横スクロール: 端のぼかしと、列の下のバー
        if (listScroll_ > 0.5) {
            cairo_pattern_t* fade = cairo_pattern_create_linear(box.x, 0, box.x + 24, 0);
            cairo_pattern_add_color_stop_rgba(fade, 0, kCard.r, kCard.g, kCard.b, 1);
            cairo_pattern_add_color_stop_rgba(fade, 1, kCard.r, kCard.g, kCard.b, 0);
            cairo_set_source(cr, fade);
            cairo_rectangle(cr, box.x, box.y, 24, box.h);
            cairo_fill(cr);
            cairo_pattern_destroy(fade);
        }
        if (listScroll_ < listMaxScroll_ - 0.5) {
            cairo_pattern_t* fade = cairo_pattern_create_linear(box.right() - 24, 0, box.right(), 0);
            cairo_pattern_add_color_stop_rgba(fade, 0, kCard.r, kCard.g, kCard.b, 0);
            cairo_pattern_add_color_stop_rgba(fade, 1, kCard.r, kCard.g, kCard.b, 1);
            cairo_set_source(cr, fade);
            cairo_rectangle(cr, box.right() - 24, box.y, 24, box.h);
            cairo_fill(cr);
            cairo_pattern_destroy(fade);
        }
        fillRounded(cr, {box.x, box.bottom() + 6, box.w, 6}, 3, kIconBox);
        const double thumbW = std::max(40.0, box.w * box.w / contentH);
        const double thumbX = box.x + (box.w - thumbW) * (listScroll_ / listMaxScroll_);
        fillRounded(cr, {thumbX, box.bottom() + 6, thumbW, 6}, 3, kBorder);
        return;
    }
    // 下に続きがあるときは、下の端をカードの色へぼかす
    if (listScroll_ < listMaxScroll_ - 0.5) {
        cairo_pattern_t* fade = cairo_pattern_create_linear(0, box.bottom() - 28, 0, box.bottom());
        cairo_pattern_add_color_stop_rgba(fade, 0, kCard.r, kCard.g, kCard.b, 0);
        cairo_pattern_add_color_stop_rgba(fade, 1, kCard.r, kCard.g, kCard.b, 1);
        cairo_set_source(cr, fade);
        cairo_rectangle(cr, box.x - 2, box.bottom() - 28, box.w + 4, 28);
        cairo_fill(cr);
        cairo_pattern_destroy(fade);
    }
    // 細いスクロールバー（溝とつまみ）
    fillRounded(cr, {barX, box.y, 6, box.h}, 3, kIconBox);
    const double thumbH = std::max(40.0, box.h * box.h / contentH);
    const double thumbY = box.y + (box.h - thumbH) * (listScroll_ / listMaxScroll_);
    fillRounded(cr, {barX, thumbY, 6, thumbH}, 3, kBorder);
}

void MicPanel::drawOutputPicker(const frame_ui::Canvas& ui, const UiText& t, const Config& config, const PanelModel& model) {
    cairo_t* cr = ui.cr;
    drawModal(ui, kOutputModal, t.outPickerTitle, t.outPickerSub);
    const AudioDevices& devices = model.state.devices;
    const std::string activeKey = model.activeOutputKey.empty() ? devices.defaultOutputKey : model.activeOutputKey;
    const std::vector<OutputEntry> entries = outputEntries(config, devices, activeKey, t);

    // 並び: 「つながっている出口」の見出し・行 → 「前に使った出口」の見出し・行（どれも間 8）
    /** 一覧の 1 項目（見出しか行）。 */
    struct Item {
        bool header;
        bool firstHeader;
        const OutputEntry* entry;
    };
    std::vector<Item> items;
    bool pastStarted = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i == 0 && entries[i].connected) items.push_back({true, true, nullptr});
        if (!entries[i].connected && !pastStarted) {
            items.push_back({true, false, nullptr});
            pastStarted = true;
        }
        items.push_back({false, false, &entries[i]});
    }
    const auto itemH = [](const Item& item) { return item.header ? (item.firstHeader ? 30.0 : 38.0) : kRowH; };
    double contentH = 0;
    for (const Item& item : items) contentH += itemH(item) + kRowGap;
    if (!items.empty()) contentH -= kRowGap;
    beginList(kOutputList, contentH);

    cairo_save(cr);
    cairo_rectangle(cr, kOutputList.x - 2, kOutputList.y, kOutputList.w + 4, kOutputList.h);
    cairo_clip(cr);
    double y = kOutputList.y - listScroll_;
    for (const Item& item : items) {
        const double h = itemH(item);
        if (y + h < kOutputList.y || y > kOutputList.bottom()) {
            y += h + kRowGap;
            continue;
        }
        if (item.header) {
            const std::string text = item.firstHeader ? t.outConnected : t.outPast;
            ui.text(kOutputList.x, y + h - 10, text, 15, rgb(kLabelSoft), true);
            y += h + kRowGap;
            continue;
        }
        const OutputEntry& e = *item.entry;
        const Rect row {kOutputList.x, y, kOutputList.w, kRowH};
        const bool viewing = e.key == viewedKey_;
        const Rect button {row.right() - 12 - kRowButtonW, y + (kRowH - 56) / 2, kRowButtonW, 56};
        const Rect rowArea {row.x, row.y, button.x - 12 - row.x, row.h};
        const int l = look(rowArea);
        fillRounded(cr, row, 16, viewing ? kRowSelected : (l == 2 ? kControlDown : (l == 1 ? kControl : kInset)));
        if (viewing) strokeRounded(cr, row, 16, kAccent, 2);
        // 右: ここから音を出す / いま使用中 / 忘れる
        if (e.active) {
            const Rect chip {button.x, y + (kRowH - 44) / 2, kRowButtonW, 44};
            fillRounded(cr, chip, 22, kSuccessTint);
            const double size = ui.fit(t.statusActive, 16, 12, chip.w - 48, true);
            const double w = 12 + 8 + ui.measure(t.statusActive, size, true);
            const double cx = chip.x + (chip.w - w) / 2;
            fillCircle(cr, cx + 6, chip.y + chip.h / 2, 6, kSuccess);
            ui.text(cx + 20, frame_ui::centerBaseline(chip.y, chip.h, size), t.statusActive, size, rgb(kSuccess), true);
        } else {
            const PanelAction action = e.connected ? PanelAction::OutputUse : PanelAction::OutputForget;
            const Rect hit = frame_ui::drawButton(ui, button, e.connected ? t.useOutput : t.forget);
            addButton({action, 0, e.key}, intersect(hit, kOutputList), true);
        }
        // 左: 絵・名前（設定を表示中の札）・状態（前に使った出口は絵と名前を少し薄く）
        const double cy = y + kRowH / 2;
        const double nameX = row.x + 14 + 44 + 14;
        const std::string summary = summaryText(e.profile.echo, e.profile.ns, t);
        const double summaryW = ui.measure(summary, 14);
        const double summaryRight = button.x - 20;
        const double nameRight = summaryRight - summaryW - 20;
        withAlpha(cr, e.connected ? 1.0 : 0.7, [&] {
            fillRounded(cr, {row.x + 14, cy - 22, 44, 44}, 11, kIconBox);
            drawOutputIcon(cr, isBuiltinSpeaker(e.key), row.x + 14 + 10, cy - 12, 24, kText);
        });
        double tagW = 0;
        if (viewing) tagW = ui.measure(t.viewingTag, 13, true) + 20 + 10;
        const double nameRoom = std::max(60.0, nameRight - nameX - tagW);
        const double nameSize = ui.fit(e.name, 19, 15, nameRoom, true);
        const std::string name = ui.ellipsize(e.name, nameSize, nameRoom, true);
        double nameW = 0;
        withAlpha(cr, e.connected ? 1.0 : 0.7,
                  [&] { nameW = ui.text(nameX, cy - 4, name, nameSize, rgb(kText), true); });
        if (viewing) {
            const Rect tag {nameX + nameW + 10, cy - 4 - 17, tagW - 10, 24};
            fillRounded(cr, tag, 12, kAccentTint);
            ui.text(tag.x + 10, frame_ui::centerBaseline(tag.y, tag.h, 13), t.viewingTag, 13, rgb(kAccentText), true);
        }
        std::string status;
        Color statusColor = kTextMuted;
        if (e.connected) {
            status = e.active ? t.statusActive : t.statusConnected;
            if (e.active) statusColor = kSuccess;
            fillCircle(cr, nameX + 4, cy + 17, 4, statusColor);
            ui.text(nameX + 14, cy + 22, status, 14, rgb(statusColor));
        } else {
            const std::string date = monthDay(e.profile.lastUsed);
            status = date.empty() ? std::string(t.lastUsedUnknown) : format1(t.lastUsedFormat, date);
            ui.text(nameX, cy + 22, status, 14, rgb(kTextMuted));
        }
        ui.text(summaryRight, frame_ui::centerBaseline(row.y, row.h, 14), summary, 14, rgb(kTextMuted), false, true);
        addButton({PanelAction::OutputView, 0, e.key}, intersect(rowArea, kOutputList), true);
        y += kRowH + kRowGap;
    }
    cairo_restore(cr);
    endList(ui, contentH, kOutputList.right() + 8);

    drawLines(ui, kOutputList.x, 650 + 14, 20, wrapText(ui, t.outPickerNote, 13, false, kOutputList.w, 2), 13, kTextMuted);
}

void MicPanel::drawMicPicker(const frame_ui::Canvas& ui, const UiText& t, const PanelModel& model) {
    cairo_t* cr = ui.cr;
    drawModal(ui, kMicModal, t.micPickerTitle, t.micPickerSub);
    const AudioDevices& devices = model.state.devices;
    const double contentH = devices.inputs.empty() ? 0 : devices.inputs.size() * (kRowH + kRowGap) - kRowGap;
    beginList(kMicList, contentH);
    if (devices.inputs.empty()) ui.text(kMicList.x, kMicList.y + 30, t.loading, 16, rgb(kTextMuted));
    cairo_save(cr);
    cairo_rectangle(cr, kMicList.x - 2, kMicList.y, kMicList.w + 4, kMicList.h);
    cairo_clip(cr);
    double y = kMicList.y - listScroll_;
    for (const AudioEndpoint& e : devices.inputs) {
        if (y + kRowH >= kMicList.y && y <= kMicList.bottom()) {
            const Rect row {kMicList.x, y, kMicList.w, kRowH};
            fillRounded(cr, row, 16, kInset);
            const double cy = y + kRowH / 2;
            fillRounded(cr, {row.x + 14, cy - 22, 44, 44}, 11, kIconBox);
            drawMicIcon(cr, row.x + 14 + 11, cy - 11, 22, kText, false);
            const bool builtin = isBuiltinMic(e.key);
            const Rect button {row.right() - 12 - kRowButtonW, y + (kRowH - 56) / 2, kRowButtonW, 56};
            const double textX = row.x + 14 + 44 + 14;
            const double room = button.x - 16 - textX;
            const std::string name = builtin ? std::string(t.builtinMicName) : format1(t.micOfFormat, e.name);
            const double nameSize = ui.fit(name, 19, 15, room, true);
            ui.text(textX, cy - 4, ui.ellipsize(name, nameSize, room, true), nameSize, rgb(kText), true);
            const std::string note = builtin ? t.micNoteBuiltin : t.micNoteExternal;
            ui.text(textX, cy + 22, ui.ellipsize(note, 14, room), 14, rgb(kTextMuted));
            if (e.key == devices.defaultInputKey) {
                const Rect chip {button.x, y + (kRowH - 44) / 2, kRowButtonW, 44};
                fillRounded(cr, chip, 22, kSuccessTint);
                const double size = ui.fit(t.statusActive, 16, 12, chip.w - 48, true);
                const double w = 12 + 8 + ui.measure(t.statusActive, size, true);
                const double cx = chip.x + (chip.w - w) / 2;
                fillCircle(cr, cx + 6, chip.y + chip.h / 2, 6, kSuccess);
                ui.text(cx + 20, frame_ui::centerBaseline(chip.y, chip.h, size), t.statusActive, size, rgb(kSuccess), true);
            } else {
                addButton({PanelAction::MicUse, 0, e.key}, intersect(frame_ui::drawButton(ui, button, t.useMic), kMicList), true);
            }
        }
        y += kRowH + kRowGap;
    }
    cairo_restore(cr);
    endList(ui, contentH, kMicList.right() + 8);
}

// ============================================================================
// 全体
// ============================================================================

void MicPanel::render(const Config& config, const PanelModel& model) {
    const UiText& t = uiText(config.language);
    buttons_.clear();
    tracks_.clear();
    listRect_ = {};
    listMaxScroll_ = 0;
    listHorizontal_ = false;
    const double modalScroll = listScroll_;

    // 地（不透明。コントラスト比は不透明な地で計算している）
    cairo_save(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr_);
    cairo_restore(cr_);

    // 重ねた画面を出している間は、後ろの部品は乗っている・押している見た目にしない（押せない）
    const bool modal = overlay_ != PanelOverlay::None;
    const frame_ui::Pointer live = pointer_;
    frame_ui::Pointer shown = pointer_;
    if (listDragging_) shown.inside = false;
    if (modal) pointer_.inside = false;  // look() と frame-ui の部品が後ろでポインターを見ないように
    const frame_ui::Canvas back {cr_, fonts_.regular(), fonts_.bold(), modal ? frame_ui::Pointer {} : shown};
    frame_ui::drawPanelBackground(back, kWidth, kHeight);
    if (view_ == PanelView::Settings) {
        drawSettings(back, t, config, model);
    } else {
        drawApps(back, t, config, model);
    }
    drawHeader(back, t, model);
    drawFooter(back, t, config, model.state);

    if (modal) {
        // 後ろのミックス列が beginList でスクロールを 0 に戻す。候補の一覧は自分の位置を使う。
        listScroll_ = modalScroll;
        pointer_ = live;
        buttons_.clear();  // 後ろのボタンは押せない
        tracks_.clear();
        const frame_ui::Canvas top {cr_, fonts_.regular(), fonts_.bold(), shown};
        switch (overlay_) {
            case PanelOverlay::OutputPicker: drawOutputPicker(top, t, config, model); break;
            case PanelOverlay::MicPicker: drawMicPicker(top, t, model); break;
            case PanelOverlay::AppsList: drawAppsList(top, t, config, model); break;
            case PanelOverlay::MixSuggest: drawMixSuggest(top, t, config, model); break;
            case PanelOverlay::AppsConfirm:
                if (confirmBack_ == PanelOverlay::AppsList) {
                    // 一覧の上に確認を重ねる: 一覧は押せないようにして、もう一段暗くする
                    const frame_ui::Canvas listUi {cr_, fonts_.regular(), fonts_.bold(), frame_ui::Pointer {}};
                    drawAppsList(listUi, t, config, model);
                    buttons_.clear();
                }
                listRect_ = {};
                listMaxScroll_ = 0;
                drawAppsConfirm(top, t, config, model);
                break;
            case PanelOverlay::None: break;
        }
    }
    if (modal) {
        languageOpen_ = false;
    } else if (languageOpen_) {
        drawLanguageMenu(back, config.language);
    }

    // 押せなくなったボタンに乗っていた印は外す
    bool hoverFound = false;
    for (const auto& b : buttons_) hoverFound |= b.hit == hover_;
    if (!hoverFound) hover_ = {};
    cairo_surface_flush(surface_);
}

const std::vector<uint8_t>& MicPanel::toRgba() {
    surfaceToRgba(surface_, rgba_);
    return rgba_;
}

bool MicPanel::writePng(const std::string& path) const {
    return cairo_surface_write_to_png(surface_, path.c_str()) == CAIRO_STATUS_SUCCESS;
}

bool MicPanel::trackCenter(PanelAction action, double& x, double& y) const {
    for (const Track& track : tracks_) {
        if (track.action != action) continue;
        x = (track.x0 + track.x1) / 2;
        y = track.cy;
        return true;
    }
    return false;
}

bool MicPanel::buttonCenter(PanelAction action, double& x, double& y, const std::string& key) const {
    for (const auto& b : buttons_) {
        if (b.hit.action != action || (!key.empty() && b.hit.key != key)) continue;
        x = b.rect.x + b.rect.w / 2;
        y = b.rect.y + b.rect.h / 2;
        return true;
    }
    return false;
}

Rect MicPanel::buttonRect(PanelAction action) const {
    for (const auto& b : buttons_) {
        if (b.hit.action == action) return b.rect;
    }
    return {};
}

int MicPanel::width() const {
    return kWidth;
}

int MicPanel::height() const {
    return kHeight;
}

void renderThumbnail(const FontSet& fonts, int size, std::vector<uint8_t>& rgba, const std::string& pngPath) {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* cr = cairo_create(surface);
    drawThumbnail(cr, fonts, size);
    cairo_surface_flush(surface);
    surfaceToRgba(surface, rgba);
    if (!pngPath.empty()) cairo_surface_write_to_png(surface, pngPath.c_str());
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}
