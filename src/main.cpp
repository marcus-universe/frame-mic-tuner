// Frame Mic Tuner: Steam Frame のマイクのエコー除去・ノイズ除去を、SteamVR のダッシュボードから切り替えるパネル。
// 切り替えは「wpctl settings --save」だけで行う（PipeWire・WirePlumber の再起動・amixer の書き換えはしない）。
#include "command.h"
#include "config.h"
#include "draw.h"
#include "i18n.h"
#include "mic_panel.h"
#include "mic_state.h"
#include "mic_worker.h"
#include "mix_meter.h"
#include "output_sync.h"
#include "outputs.h"
#include "self_test.h"
#include "theme.h"
#include "voice_check.h"
#include "vr_overlay.h"

#include "frame_apps.h"
#include "update_check.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef FRAME_MIC_TUNER_VERSION
#define FRAME_MIC_TUNER_VERSION "unknown"  // CMake を通さずにビルドしたとき
#endif

namespace {

volatile std::sig_atomic_t gStopRequested = 0;
volatile std::sig_atomic_t gShowRequested = 0;  ///< SIGUSR1（2 つ目の起動から）でパネルを開く

/** 「終了」「閉じる」で終わったときの終了コード（systemd の RestartPreventExitStatus= に書く）。 */
constexpr int kExitCodeUserQuit = 3;

constexpr int kThumbnailSize = 256;         ///< ダッシュボードのサムネイルの一辺（px）
constexpr double kPanelPollSec = 0.033;     ///< パネルが見えている間のイベント確認の間隔
constexpr double kClosedPollSec = 0.25;     ///< パネルが見えていない間のイベント確認の間隔
constexpr double kVoiceFrameSec = 1.0 / 15; ///< 録音中・再生中の描き直しの間隔
constexpr double kOverlayCheckSec = 3.0;    ///< 自己修復: 自分のオーバーレイがまだあるかを確かめる間隔（閉じている間も）

// 新しい版の確認・更新（vendor/frame-updater）。frame-update.sh は install.sh が置く場所を読む
constexpr const char* kUpdateAppName = "frame-mic-tuner";
constexpr const char* kUpdateRepo = "sasaken1102r/frame-mic-tuner";
constexpr const char* kUpdateAssetPattern = "frame-mic-tuner-{version}.tar.gz";

/** コマンドラインの内容。 */
struct Options {
    enum class Mode { Overlay, Print, DumpPng, Probe, SwitchAway, TestRecord, ContrastReport, SelfTest, Version, Help };
    Mode mode = Mode::Overlay;
    std::string configPath;
    std::string pngPath;
    std::string thumbnailPngPath;
    int thumbnailSize = 256;       ///< --thumbnail-png で書き出す一辺（px）
    std::string language;          ///< 空でなければ PNG の書き出しで設定の言語の代わりに使う（ja / en / sc / de / fr / es / it）
    bool previewQuit = false;      ///< 「もう一度押すと終了」の状態で描く
    std::vector<MicCommand> sets;  ///< --set-echo などの書き込み（--print の前に実行する）
    // 見た目の確認用のダミーの状態（どれか 1 つでも付けると、実際の値を読まずにダミーで描く）
    bool fake = false;
    bool fakeEcho = true;
    bool fakeNs = false;
    bool fakeIdle = false;
    bool fakeLoading = false;
    bool fakeMuted = false;
    Autostart fakeAutostart = Autostart::Disabled;
    MicError fakeError = MicError::None;
    // 声のチェックのダミー（--dump-png 用）
    bool fakeRecording = false;
    int fakeHistory = 0;       ///< 履歴の件数（0〜5）
    int fakePlaying = -1;      ///< 再生中の行（新しい順の何件目か。-1 でなし）
    VoiceError fakeVoiceError = VoiceError::None;
    std::string previewPressed;  ///< 押している見た目にするボタン（earphone / speaker / record）
    double testRecordSec = 3.0;  ///< --test-record の秒数
    bool debugRecordOnOpen = false;  ///< 確認用: パネルが開いたら自動で録音を始める
    double switchAwaySec = 3.0;      ///< --probe-switch-away で切り替えたままにする秒数
    // ノイズ除去の強さ（--set-ns-vad / --set-ns-grace。その場でかけるだけで、設定ファイルには保存しない）
    bool setNsVad = false;
    double nsVad = 0.0;
    bool setNsGrace = false;
    double nsGrace = 0.0;
    // ノイズ除去の強さのダミーと、バーをドラッグしている見た目（--dump-png 用）
    double fakeNsVad = kNsVadDefault;
    double fakeNsGrace = kNsGraceDefault;
    PanelAction previewDrag = PanelAction::None;
    double previewDragValue = 0.0;
    std::string tab;  ///< 空でなければ PNG の書き出しで設定のタブの代わりに使う（quick / fine / mix）
    int fakeMix = -1;             ///< 0 以上ならミックスのタブを、この数のチャンネルで描く（--fake-mix）
    // 版の行のダミー（--dump-png 用）
    std::string fakeUpdate;         ///< unknown/uptodate/checking/available/manual/installing/installed/checkfailed/installfailed
    bool previewUpdateConfirm = false;  ///< 「更新する」の確認の表示にする（available と組み合わせる）
    bool fakeEchoGiven = false;     ///< --fake-echo を付けた（付けなければ今の出口のプリセットに合わせる）
    // 音の出口・マイクのダミー（--dump-png 用）
    std::string fakeOutput = "speaker";  ///< 今の出口: speaker（Frame のスピーカー）/ earphones（AB13X）
    int fakePastOutputs = 3;        ///< 前に使った（いまはつながっていない）出口の数（多いと一覧がスクロールする）
    bool fakeSwitched = false;      ///< 自動で切り替えた知らせ（元に戻す）を出す
    bool fakeExternalMic = false;   ///< 外付けのマイク（AB13X）を使っている
    std::string fakeViewing;        ///< 設定を表示する出口: 空 = 今の出口、ab13x、past（前に使った出口の 1 つ目）
    std::string view;               ///< settings / apps
    std::string overlay;            ///< output / mic / apps / confirm / list-confirm
    std::string previewScroll;      ///< 重ねた画面の一覧のスクロール（px か end）
    // ほかのアプリのダミー（--dump-png 用）
    std::string fakeAux = "missing";  ///< frame-aux-shortcuts の状態: missing / installed / running
    std::string fakeAppsBusy;         ///< Konsole を開いている呼び名（カンマ区切り。menu はメニュー）
    int fakeAppsExtra = 0;            ///< 一覧に足すダミーのアプリの数（スクロールの見た目）
    std::string fakeConfirm = "aux";  ///< 確認の画面のアプリ（呼び名。menu はメニュー）
    std::string fakeLaunch;           ///< Konsole を開けなかった理由: busy / nodisplay / nokonsole / failed
};

/**
 * SIGTERM / SIGINT を受けたら止める印をつける。
 * @param signal 受けたシグナル（使わない）
 */
void onSignal(int /*signal*/) {
    gStopRequested = 1;
}

/**
 * SIGUSR1 を受けたら、パネルを開く印をつける（2 つ目の起動が送ってくる）。
 * @param signal 受けたシグナル（使わない）
 */
void onShowSignal(int /*signal*/) {
    gShowRequested = 1;
}

/**
 * SIGTERM / SIGINT で行儀よく終われるようにし、SIGUSR1 でパネルを開けるようにする。
 */
void installSignalHandlers() {
    struct sigaction show {};
    show.sa_handler = onShowSignal;
    sigemptyset(&show.sa_mask);
    sigaction(SIGUSR1, &show, nullptr);

    struct sigaction action {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}

/**
 * 単調増加の時計で今の時刻を秒で返す。
 * @return 秒
 */
double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/**
 * 止める印・パネルを開く印がつくまで、または指定時間が経つまで待つ。
 * @param seconds 待つ秒数
 */
void sleepInterruptible(double seconds) {
    const double end = nowSeconds() + seconds;
    while (!gStopRequested && !gShowRequested) {
        const double left = end - nowSeconds();
        if (left <= 0) break;
        std::this_thread::sleep_for(std::chrono::duration<double>(std::fmin(left, 0.5)));
    }
}

/** 使い方を表示する。 */
void printUsage() {
    std::printf(
        "使い方: frame-mic-tuner [オプション]\n"
        "  （なし）              SteamVR のダッシュボードにパネルを出して常駐する（SteamVR が無ければ待つ）\n"
        "                        すでに常駐していれば、そちらのパネルを開いて終わる\n"
        "  --print               OpenVR なしで、今の値・マイク使用中か・つながりを表示して終わる\n"
        "  --set-echo on|off     エコー除去を切り替えてから --print と同じ表示をする\n"
        "  --set-ns on|off       ノイズ除去を切り替えてから表示する\n"
        "  --set-autostart on|off  SteamVR と一緒に起動（systemctl --user enable/disable）を切り替えてから表示する\n"
        "  --set-ns-vad N        ノイズ除去の判定の厳しさ（0〜99%%）をその場でかけてから表示する（保存はしない）\n"
        "  --set-ns-grace N      ノイズ除去の余韻（0〜1000ms）をその場でかけてから表示する（保存はしない）\n"
        "  --dump-png PATH       OpenVR なしでパネルの画像を PNG に書き出して終わる（今の値で描く）\n"
        "  --thumbnail-png PATH  ダッシュボードのサムネイル（＋のアイコンと同じ絵）を PNG に書き出す\n"
        "      --thumbnail-size N  そのサムネイルの一辺（既定 256）\n"
        "      --language ja|en|sc|de|fr|es|it  設定の言語の代わりにこの言語で描く\n"
        "      --preview-quit    「もう一度押すと終了」の状態で描く\n"
        "      --tab quick|fine|mix  設定のタブの代わりに、このタブ（かんたん / 細かく調整 / ミックス）で描く\n"
        "      --fake-mix N      ミックスのタブを、チャンネル N 個（0〜8）のダミーで描く\n"
        "      --fake            実際の値を読まず、ダミーの状態（スピーカー・使用中）で描く。次の --fake-* も同じ\n"
        "      --fake-echo on|off / --fake-ns on|off   ダミーのエコー除去・ノイズ除去\n"
        "      --fake-idle       マイク未使用\n"
        "      --fake-loading    まだ読んでいない状態\n"
        "      --fake-muted      既定のマイクがミュートされている（ミュート中のバッジと帯）\n"
        "      --fake-autostart on|off|missing|unknown  自動起動の状態（missing = ユニットファイルが無い）\n"
        "      --fake-error read|not-installed|links|write|write-echo|write-ns|write-mute|write-mute-on|\n"
        "                   write-output|write-input|autostart  赤い失敗の表示（write-mute は解除の失敗）\n"
        "      --fake-recording  録音中（メーター・経過）の見た目\n"
        "      --fake-history N  ダミーの履歴を N 件（0〜5）\n"
        "      --fake-playing I  履歴の I 件目（0 が最新）を再生中にする\n"
        "      --fake-voice-error record|play  声のチェックの失敗の表示\n"
        "      --fake-update STATE  版の行のダミー（unknown/uptodate/checking/available/manual/installing/\n"
        "                        installed/checkfailed/installfailed）\n"
        "      --preview-update-confirm  「更新する」を確認の表示にする（--fake-update available と組み合わせる）\n"
        "      --preview-pressed earphone|speaker|record|mute|apps  押している間の見た目\n"
        "      --fake-ns-vad N / --fake-ns-grace N  ダミーのノイズ除去の強さ（既定 23 / 500）\n"
        "      --preview-drag-vad N / --preview-drag-grace N  そのバーを N までドラッグしている見た目\n"
        "      --fake-output speaker|earphones  今の音の出口（Frame のスピーカー / AB13X USB Audio）\n"
        "      --fake-past-outputs N  前に使った（いまはつながっていない）出口の数（既定 3。多いと一覧がスクロールする）\n"
        "      --fake-switched   出口をつないで自動で切り替えた知らせ（元に戻す）を出す\n"
        "      --fake-external-mic  外付けのマイク（AB13X）を使っている\n"
        "      --fake-viewing ab13x|past  今の出口ではなく、その出口の設定を表示する\n"
        "      --view settings|apps  画面（マイクの設定 / アプリと更新）\n"
        "      --overlay output|mic|apps|confirm|list-confirm|suggest  重ねて出す画面（音の出口を選ぶ / 使うマイク /\n"
        "                        ささけんの Frame アプリ / 入れる前の確認 / 一覧から開いた確認 / ミックスの名前）\n"
        "      --preview-scroll PX|end  重ねた画面の一覧のスクロール\n"
        "      --fake-aux missing|installed|running  frame-aux-shortcuts の状態（ほかは見本の状態）\n"
        "      --fake-apps-busy KEYS  Konsole を開いているアプリの呼び名（カンマ区切り。menu はメニュー）\n"
        "      --fake-apps-extra N  一覧にダミーのアプリを N 個足す（一覧のスクロールの見た目）\n"
        "      --fake-confirm KEY|menu  確認の画面のアプリ（既定 aux）\n"
        "      --fake-launch busy|nodisplay|nokonsole|failed  Konsole を開けなかった表示\n"
        "  --test-record [S]     OpenVR なしで、既定の入力から S 秒（既定 3）録音 → ピークと長さを表示 → 既定の出力で再生\n"
        "                        （録音中と後に pw-metadata -n filters も表示する。音声はメモリの中だけ）\n"
        "  --contrast-report     画面の文字色・部品の色と背景の組み合わせごとに、WCAG のコントラスト比と合否を出す\n"
        "  --self-test           OpenVR なしで、判定の関数（自己修復のオーバーレイの判定・出力の読み取り）を試して結果を出す\n"
        "  --probe-switch-away [S]  確認用: 一時的なダッシュボードのオーバーレイに S 秒（既定 3）切り替えて、Mic のパネルを閉じた状態にする\n"
        "  --debug-record-on-open   確認用（常駐）: パネルが開いたら自動で録音を始める（閉じたら止まることのログ確認用）\n"
        "  --probe               診断用: SteamVR に Background 型でつなぎ、常駐しているパネルを探して状態を出す\n"
        "  --version             版を表示して終わる\n"
        "  --config PATH         設定ファイル（既定 ~/.config/frame-mic-tuner/config.json）\n");
}

/**
 * on / off を読む。
 * @param text 引数
 * @param value 読めたときの書き込み先
 * @return 読めたら true
 */
bool parseOnOff(const std::string& text, bool& value) {
    if (text == "on" || text == "true") {
        value = true;
        return true;
    }
    if (text == "off" || text == "false") {
        value = false;
        return true;
    }
    std::fprintf(stderr, "on か off で指定してください: %s\n", text.c_str());
    return false;
}

/**
 * コマンドラインを読む。
 * @param argc 引数の数
 * @param argv 引数
 * @param options 書き込み先
 * @return 正しく読めたら true
 */
bool parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasNext = i + 1 < argc;
        bool value = false;
        if (arg == "--print") {
            options.mode = Options::Mode::Print;
        } else if ((arg == "--set-echo" || arg == "--set-ns" || arg == "--set-autostart") && hasNext) {
            if (!parseOnOff(argv[++i], value)) return false;
            const MicCommand::Kind kind = arg == "--set-echo" ? MicCommand::Kind::SetEcho
                                          : arg == "--set-ns" ? MicCommand::Kind::SetNs
                                                              : MicCommand::Kind::SetAutostart;
            options.sets.push_back({kind, value});
            options.mode = Options::Mode::Print;
        } else if (arg == "--dump-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.pngPath = argv[++i];
        } else if (arg == "--thumbnail-png" && hasNext) {
            options.mode = Options::Mode::DumpPng;
            options.thumbnailPngPath = argv[++i];
        } else if (arg == "--thumbnail-size" && hasNext) {
            options.thumbnailSize = std::max(16, std::min(1024, std::atoi(argv[++i])));
        } else if (arg == "--language" && hasNext) {
            options.language = argv[++i];
            Language check;
            if (!parseLanguage(options.language, check)) {
                std::fprintf(stderr, "--language は ja / en / sc / de / fr / es / it です: %s\n", options.language.c_str());
                return false;
            }
        } else if (arg == "--preview-quit") {
            options.previewQuit = true;
        } else if (arg == "--tab" && hasNext) {
            options.tab = argv[++i];
            if (options.tab != "quick" && options.tab != "fine" && options.tab != "mix") {
                std::fprintf(stderr, "--tab は quick / fine / mix です: %s\n", options.tab.c_str());
                return false;
            }
        } else if (arg == "--fake-mix" && hasNext) {
            options.fakeMix = std::max(0, std::min(kMixChannelMax, std::atoi(argv[++i])));
            options.fake = true;
        } else if (arg == "--set-ns-vad" && hasNext) {
            options.setNsVad = true;
            options.nsVad = clampNsVad(std::atof(argv[++i]));
            options.mode = Options::Mode::Print;
        } else if (arg == "--set-ns-grace" && hasNext) {
            options.setNsGrace = true;
            options.nsGrace = clampNsGrace(std::atof(argv[++i]));
            options.mode = Options::Mode::Print;
        } else if (arg == "--fake-ns-vad" && hasNext) {
            options.fakeNsVad = clampNsVad(std::atof(argv[++i]));
            options.fake = true;
        } else if (arg == "--fake-ns-grace" && hasNext) {
            options.fakeNsGrace = clampNsGrace(std::atof(argv[++i]));
            options.fake = true;
        } else if ((arg == "--preview-drag-vad" || arg == "--preview-drag-grace") && hasNext) {
            options.previewDrag = arg == "--preview-drag-vad" ? PanelAction::NsVadSlider : PanelAction::NsGraceSlider;
            options.previewDragValue = std::atof(argv[++i]);
        } else if (arg == "--fake") {
            options.fake = true;
        } else if (arg == "--fake-echo" && hasNext) {
            if (!parseOnOff(argv[++i], options.fakeEcho)) return false;
            options.fakeEchoGiven = true;
            options.fake = true;
        } else if (arg == "--fake-output" && hasNext) {
            options.fakeOutput = argv[++i];
            if (options.fakeOutput != "speaker" && options.fakeOutput != "earphones") {
                std::fprintf(stderr, "--fake-output は speaker か earphones です: %s\n", options.fakeOutput.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-past-outputs" && hasNext) {
            options.fakePastOutputs = std::max(0, std::min(20, std::atoi(argv[++i])));
            options.fake = true;
        } else if (arg == "--fake-switched") {
            options.fakeSwitched = true;
            options.fake = true;
        } else if (arg == "--fake-external-mic") {
            options.fakeExternalMic = true;
            options.fake = true;
        } else if (arg == "--fake-viewing" && hasNext) {
            options.fakeViewing = argv[++i];
            if (options.fakeViewing != "ab13x" && options.fakeViewing != "past") {
                std::fprintf(stderr, "--fake-viewing は ab13x か past です: %s\n", options.fakeViewing.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--view" && hasNext) {
            options.view = argv[++i];
            if (options.view != "settings" && options.view != "apps") {
                std::fprintf(stderr, "--view は settings か apps です: %s\n", options.view.c_str());
                return false;
            }
        } else if (arg == "--overlay" && hasNext) {
            options.overlay = argv[++i];
            if (options.overlay != "output" && options.overlay != "mic" && options.overlay != "apps" &&
                options.overlay != "confirm" && options.overlay != "list-confirm" && options.overlay != "suggest") {
                std::fprintf(stderr, "--overlay は output / mic / apps / confirm / list-confirm / suggest です: %s\n",
                             options.overlay.c_str());
                return false;
            }
        } else if (arg == "--fake-aux" && hasNext) {
            options.fakeAux = argv[++i];
            if (options.fakeAux != "missing" && options.fakeAux != "installed" && options.fakeAux != "running") {
                std::fprintf(stderr, "--fake-aux は missing / installed / running です: %s\n", options.fakeAux.c_str());
                return false;
            }
        } else if (arg == "--fake-apps-busy" && hasNext) {
            options.fakeAppsBusy = argv[++i];
        } else if (arg == "--fake-apps-extra" && hasNext) {
            options.fakeAppsExtra = std::max(0, std::min(20, std::atoi(argv[++i])));
        } else if (arg == "--fake-confirm" && hasNext) {
            options.fakeConfirm = argv[++i];
        } else if (arg == "--fake-launch" && hasNext) {
            options.fakeLaunch = argv[++i];
            if (options.fakeLaunch != "busy" && options.fakeLaunch != "nodisplay" && options.fakeLaunch != "nokonsole" &&
                options.fakeLaunch != "failed") {
                std::fprintf(stderr, "--fake-launch は busy / nodisplay / nokonsole / failed です: %s\n",
                             options.fakeLaunch.c_str());
                return false;
            }
        } else if (arg == "--preview-scroll" && hasNext) {
            options.previewScroll = argv[++i];
        } else if (arg == "--fake-ns" && hasNext) {
            if (!parseOnOff(argv[++i], options.fakeNs)) return false;
            options.fake = true;
        } else if (arg == "--fake-idle") {
            options.fakeIdle = true;
            options.fake = true;
        } else if (arg == "--fake-loading") {
            options.fakeLoading = true;
            options.fake = true;
        } else if (arg == "--fake-muted") {
            options.fakeMuted = true;
            options.fake = true;
        } else if (arg == "--fake-autostart" && hasNext) {
            const std::string state = argv[++i];
            if (state == "on") {
                options.fakeAutostart = Autostart::Enabled;
            } else if (state == "off") {
                options.fakeAutostart = Autostart::Disabled;
            } else if (state == "missing") {
                options.fakeAutostart = Autostart::Missing;
            } else if (state == "unknown") {
                options.fakeAutostart = Autostart::Unknown;
            } else {
                std::fprintf(stderr, "--fake-autostart は on / off / missing / unknown です: %s\n", state.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-error" && hasNext) {
            const std::string kind = argv[++i];
            if (kind == "read") {
                options.fakeError = MicError::ReadSettings;
            } else if (kind == "not-installed") {
                options.fakeError = MicError::NotInstalled;
            } else if (kind == "links") {
                options.fakeError = MicError::ReadLinks;
            } else if (kind == "write") {
                options.fakeError = MicError::WriteSettings;
            } else if (kind == "write-echo") {
                options.fakeError = MicError::WriteEcho;
            } else if (kind == "write-ns") {
                options.fakeError = MicError::WriteNs;
            } else if (kind == "write-mute") {
                options.fakeError = MicError::WriteMute;
            } else if (kind == "write-mute-on") {
                options.fakeError = MicError::WriteMuteOn;
            } else if (kind == "write-output") {
                options.fakeError = MicError::WriteOutput;
            } else if (kind == "write-input") {
                options.fakeError = MicError::WriteInput;
            } else if (kind == "autostart") {
                options.fakeError = MicError::WriteAutostart;
            } else {
                std::fprintf(stderr,
                             "--fake-error は read / not-installed / links / write / write-echo / write-ns / write-mute / "
                             "write-mute-on / write-output / write-input / autostart です: %s\n",
                             kind.c_str());
                return false;
            }
            options.fake = true;
        } else if (arg == "--fake-recording") {
            options.fakeRecording = true;
        } else if (arg == "--fake-history" && hasNext) {
            options.fakeHistory = std::max(0, std::min(static_cast<int>(kVoiceHistory), std::atoi(argv[++i])));
        } else if (arg == "--fake-playing" && hasNext) {
            options.fakePlaying = std::atoi(argv[++i]);
        } else if (arg == "--fake-voice-error" && hasNext) {
            const std::string kind = argv[++i];
            if (kind != "record" && kind != "play") {
                std::fprintf(stderr, "--fake-voice-error は record / play です: %s\n", kind.c_str());
                return false;
            }
            options.fakeVoiceError = kind == "record" ? VoiceError::Record : VoiceError::Play;
        } else if (arg == "--fake-update" && hasNext) {
            options.fakeUpdate = argv[++i];
            static const char* kKnown[] = {"unknown",  "uptodate",     "checking",     "available",
                                           "manual",   "installing",   "installed",    "checkfailed",
                                           "installfailed"};
            bool known = false;
            for (const char* name : kKnown) known |= options.fakeUpdate == name;
            if (!known) {
                std::fprintf(stderr,
                             "--fake-update は unknown/uptodate/checking/available/manual/installing/installed/"
                             "checkfailed/installfailed です: %s\n",
                             options.fakeUpdate.c_str());
                return false;
            }
        } else if (arg == "--preview-update-confirm") {
            options.previewUpdateConfirm = true;
        } else if (arg == "--preview-pressed" && hasNext) {
            options.previewPressed = argv[++i];
        } else if (arg == "--test-record") {
            options.mode = Options::Mode::TestRecord;
            if (hasNext && argv[i + 1][0] != '-') {
                options.testRecordSec = std::max(0.5, std::min(kVoiceMaxSec, std::atof(argv[++i])));
            }
        } else if (arg == "--contrast-report") {
            options.mode = Options::Mode::ContrastReport;
        } else if (arg == "--self-test") {
            options.mode = Options::Mode::SelfTest;
        } else if (arg == "--probe-switch-away") {
            options.mode = Options::Mode::SwitchAway;
            if (hasNext && argv[i + 1][0] != '-') options.switchAwaySec = std::max(0.5, std::atof(argv[++i]));
        } else if (arg == "--debug-record-on-open") {
            options.debugRecordOnOpen = true;
        } else if (arg == "--probe") {
            options.mode = Options::Mode::Probe;
        } else if (arg == "--config" && hasNext) {
            options.configPath = argv[++i];
        } else if (arg == "--version") {
            options.mode = Options::Mode::Version;
        } else if (arg == "--help" || arg == "-h") {
            options.mode = Options::Mode::Help;
        } else {
            std::fprintf(stderr, "知らない引数です: %s\n", arg.c_str());
            return false;
        }
    }
    if (options.configPath.empty()) options.configPath = defaultConfigPath();
    return true;
}

/**
 * 自動起動の状態を日本語にする（--print 用）。
 * @param autostart 状態
 * @return 文言
 */
const char* autostartName(Autostart autostart) {
    switch (autostart) {
        case Autostart::Enabled: return "オン（enabled）";
        case Autostart::Disabled: return "オフ（disabled）";
        case Autostart::Missing: return "ユニットファイルなし（not-found）";
        case Autostart::Unknown: break;
    }
    return "読めません";
}

/**
 * ノイズ除去の強さを短い日本語にする（ログ・--print 用）。
 * @param ns 値
 * @return 例:「判定の厳しさ 23%・余韻 500ms（ns_capture の id 53）」
 */
std::string describeNsParams(const NsParams& ns) {
    if (!ns.nodeKnown) return std::string(kNsNodeName) + " が見つかりません";
    char text[160];
    std::snprintf(text, sizeof(text), "判定の厳しさ %s・余韻 %s（%s の id %d）",
                  ns.vadKnown ? (std::to_string(static_cast<int>(std::lround(ns.vad))) + "%").c_str() : "?",
                  ns.graceKnown ? (std::to_string(static_cast<int>(std::lround(ns.grace))) + "ms").c_str() : "?",
                  kNsNodeName, ns.nodeId);
    return text;
}

/**
 * 表示する状態が変わったときに、ログへ 1 行出す（外から変えられたときの追従をあとで確かめるため）。
 * @param state 状態
 */
void logState(const MicState& state) {
    const auto onOff = [](bool known, bool value) { return known ? (value ? "オン" : "オフ") : "?"; };
    const UiText& text = uiText(Language::Ja);
    const MicError error = state.writeError != MicError::None ? state.writeError : state.readError;
    const NsParams& ns = state.nsParams;
    std::fprintf(stderr, "[マイク] 表示を更新: エコー除去 %s・ノイズ除去 %s（%s）・%s・%s・%s・自動起動 %s%s%s\n",
                 onOff(state.echoKnown, state.echo), onOff(state.nsKnown, state.ns), describeNsParams(ns).c_str(),
                 !state.linksKnown ? "使用中か不明" : (state.inUse ? "使用中" : "未使用"),
                 !state.muteKnown ? "ミュート不明" : (state.muted ? "ミュート中" : "ミュートなし"),
                 describeChain(state).c_str(), autostartName(state.autostart), error != MicError::None ? "・失敗: " : "",
                 errorText(error, text).c_str());
}

/**
 * 出口ごとの設定を短い日本語にする（--print 用）。
 * @param p 設定
 * @return 例:「エコー除去オン・ノイズ除去オフ・23%/500ms（スピーカー、最後に使った日 2026-10-10）」
 */
std::string describeProfile(const OutputProfile& p) {
    char text[200];
    std::snprintf(text, sizeof(text), "エコー除去%s・ノイズ除去%s・%.0f%%/%.0fms（%s、最後に使った日 %s）",
                  p.echo ? "オン" : "オフ", p.ns ? "オン" : "オフ", p.nsVad, p.nsGrace,
                  p.kind == OutputKind::Speaker ? "スピーカー" : "イヤホン", p.lastUsed.empty() ? "不明" : p.lastUsed.c_str());
    return text;
}

/**
 * 音の出口・マイクの一覧と、出口ごとに覚えている設定を書き出す（--print 用。読むだけで、設定ファイルは書かない）。
 * @param state 状態（devices）
 * @param config 設定ファイルの中身
 */
void printOutputs(const MicState& state, const Config& config) {
    const AudioDevices& d = state.devices;
    if (!d.known) std::printf("音の出口とマイク: 一覧を読めません（pw-dump）\n");
    std::printf("既定の出力（default.audio.sink）: %s → キー %s\n", d.defaultSinkName.empty() ? "（なし）" : d.defaultSinkName.c_str(),
                d.defaultOutputKey.empty() ? "（なし）" : d.defaultOutputKey.c_str());
    std::printf("既定の入力（default.audio.source）: %s → キー %s%s\n",
                d.defaultSourceName.empty() ? "（なし）" : d.defaultSourceName.c_str(),
                d.defaultInputKey.empty() ? "（なし）" : d.defaultInputKey.c_str(),
                d.defaultInputKey.empty() ? "" : (isBuiltinMic(d.defaultInputKey) ? "（Frame 内蔵マイク）" : "（外付けのマイク）"));
    std::printf("つながっている音の出口:\n");
    for (const AudioEndpoint& e : d.outputs) {
        std::printf("  %s %s  キー %s  既定にするノード %s（id %d）%s\n", e.key == d.defaultOutputKey ? "*" : " ",
                    e.name.c_str(), e.key.c_str(), e.nodeName.c_str(), e.nodeId,
                    isBuiltinSpeaker(e.key) ? "  Frame のスピーカー" : "");
    }
    std::printf("つながっているマイク:\n");
    for (const AudioEndpoint& e : d.inputs) {
        std::printf("  %s %s  キー %s  既定にするノード %s（id %d）%s\n", e.key == d.defaultInputKey ? "*" : " ",
                    e.name.c_str(), e.key.c_str(), e.nodeName.c_str(), e.nodeId, isBuiltinMic(e.key) ? "  Frame 内蔵" : "");
    }
    std::printf("出口ごとに覚えている設定（%s）:\n", config.outputsSaved ? "config.json の outputs" : "まだ無い。次の起動で今の設定を今の出口へ移す");
    for (const auto& entry : config.outputs) {
        std::printf("  %s %s（%s）: %s%s\n", entry.first == d.defaultOutputKey ? "*" : " ", entry.second.name.c_str(),
                    entry.first.c_str(), describeProfile(entry.second).c_str(),
                    d.findOutput(entry.first) != nullptr ? "" : "  いまはつながっていない");
    }
    if (!d.defaultOutputKey.empty() && config.outputs.count(d.defaultOutputKey) == 0) {
        if (config.outputsSaved) {
            const OutputProfile first = firstProfile(d.defaultOutputKey, "");
            std::printf("  今の出口はまだ覚えていません（常駐がこの出口を見つけると %s から始めます）\n",
                        describeProfile(first).c_str());
        } else {
            std::printf("  今の出口はまだ覚えていません（常駐が起動すると、今かかっている設定をこの出口の設定として移します）\n");
        }
    }
}

/**
 * 状態を端末向けに書き出す（--print 用）。
 * @param state 状態
 */
const char* mixNameMessage(MixNameError error, const UiText& text) {
    switch (error) {
        case MixNameError::Empty: return text.mixNameEmpty;
        case MixNameError::TooLong: return text.mixNameTooLong;
        case MixNameError::Duplicate: return text.mixNameDuplicate;
        case MixNameError::BadChar: return text.mixNameBadChar;
        case MixNameError::Ok: return "";
    }
    return "";
}

/** ワーカーへ渡す「id\\t名前」の行。 */
std::string mixChannelBlob(const std::vector<MixChannel>& channels) {
    std::string blob;
    for (const MixChannel& channel : channels) {
        blob += channel.id;
        blob += '\t';
        blob += channel.name;
        blob += '\n';
    }
    return blob;
}

void printState(const MicState& state) {
    const auto onOff = [](bool known, bool value) { return known ? (value ? "オン（true）" : "オフ（false）") : "読めません"; };
    std::printf("エコー除去 (%s): %s\n", kEchoCancelKey, onOff(state.echoKnown, state.echo));
    std::printf("ノイズ除去 (%s): %s\n", kNoiseSuppressionKey, onOff(state.nsKnown, state.ns));
    std::printf("ノイズ除去の強さ: %s\n", describeNsParams(state.nsParams).c_str());
    if (state.echoKnown && state.nsKnown) {
        // プリセット: イヤホン = エコー除去オフ・ノイズ除去オフ、スピーカー = エコー除去オン・ノイズ除去オフ
        const char* preset = state.ns ? "細かく調整した設定（どちらのプリセットとも違う）"
                                      : (state.echo ? "スピーカー" : "イヤホン");
        std::printf("プリセット: %s\n", preset);
    }
    std::printf("マイク: %s\n", !state.linksKnown ? "読めません" : (state.inUse ? "使用中" : "未使用"));
    std::printf("ミュート（既定のマイク、wpctl get-volume @DEFAULT_AUDIO_SOURCE@）: %s\n",
                !state.muteKnown ? "読めません" : (state.muted ? "ミュート中" : "ミュートなし"));
    std::printf("つながり: %s\n", describeChain(state).c_str());
    if (!state.users.empty()) {
        std::printf("マイクから音を取っているノード:");
        for (const auto& user : state.users) std::printf(" %s", user.c_str());
        std::printf("\n");
    }
    std::printf("ミックス (%s): %s\n", kMixSettingKey,
                !state.mix.settingKnown ? "設定がありません（ヘッドセットの再起動が要る）"
                                        : (state.mix.enabled ? "オン（PC のマイクへ流す）" : "オフ"));
    if (state.mix.known) {
        std::printf("  コア: %s  VRLink: %s\n", state.mix.coreRunning ? "動作中" : "停止",
                    !state.mix.vrlinkPresent ? "ストリームなし" : (state.mix.routed ? "ミックスへ接続" : "未接続"));
        for (const MixChannelLive& channel : state.mix.channels) {
            std::printf("  %s  マイクへ %s  ヘッドホン %s", channel.id.c_str(),
                        channel.toMic.known ? (std::to_string(static_cast<int>(channel.toMic.percent())) + "%").c_str() : "-",
                        channel.toPhones.known ? (std::to_string(static_cast<int>(channel.toPhones.percent())) + "%").c_str()
                                               : "-");
            if (!channel.apps.empty()) {
                std::printf("  再生中:");
                for (const std::string& app : channel.apps) std::printf(" %s", app.c_str());
            }
            std::printf("\n");
        }
    }
    std::printf("SteamVR と一緒に起動 (%s): %s\n", kServiceName, autostartName(state.autostart));
    const UiText& text = uiText(Language::Ja);
    if (state.readError != MicError::None) std::printf("失敗: %s\n", errorText(state.readError, text).c_str());
    if (state.writeError != MicError::None) std::printf("失敗: %s\n", errorText(state.writeError, text).c_str());
    std::printf("-- pw-link -l のうちマイクの通り道 --\n%s", state.rawLinks.c_str());
}

/**
 * --print（と --set-*）: OpenVR なしで、書き込みをしてから今の状態を表示する。
 * @param options コマンドライン
 * @return 終了コード（書き込みか読み取りに失敗したら 1）
 */
int runPrint(const Options& options) {
    MicError writeError = MicError::None;
    for (const MicCommand& command : options.sets) {
        bool ok = false;
        switch (command.kind) {
            case MicCommand::Kind::SetEcho: ok = writeSetting(kEchoCancelKey, command.value); break;
            case MicCommand::Kind::SetNs: ok = writeSetting(kNoiseSuppressionKey, command.value); break;
            case MicCommand::Kind::SetAutostart: ok = writeAutostart(command.value); break;
            case MicCommand::Kind::SetNsParams: break;  // 下でまとめて扱う
            // --print からは使わない（常駐のパネルだけが使う）
            case MicCommand::Kind::ApplySettings:
            case MicCommand::Kind::SetMute:
            case MicCommand::Kind::SetDefaultOutput:
            case MicCommand::Kind::SetDefaultInput:
            case MicCommand::Kind::MixEnable:
            case MicCommand::Kind::AddChannel:
            case MicCommand::Kind::RenameChannel:
            case MicCommand::Kind::RemoveChannel:
            case MicCommand::Kind::SetMixVolume:
            case MicCommand::Kind::SetMixMute: break;
        }
        if (!ok) {
            writeError = command.kind == MicCommand::Kind::SetAutostart ? MicError::WriteAutostart
                                                                         : MicError::WriteSettings;
        }
    }
    if (options.setNsVad || options.setNsGrace) {
        // 指定しなかったほうは今の値のまま。その場でかけるだけで、設定ファイルには保存しない
        const NsParams now = readNsParams();
        const double vad = options.setNsVad ? options.nsVad : (now.vadKnown ? now.vad : kNsVadDefault);
        const double grace = options.setNsGrace ? options.nsGrace : (now.graceKnown ? now.grace : kNsGraceDefault);
        if (!now.nodeKnown || !writeNsParams(now.nodeId, vad, grace)) writeError = MicError::WriteNsParams;
    }
    MicState state = readMicState();
    state.devices = readAudioDevices();
    state.writeError = writeError;
    printState(state);
    // 設定ファイルは読むだけ（--print は出口の設定を移したり、かけたりしない）
    printOutputs(state, loadConfigOrDefault(options.configPath));
    return (state.readError != MicError::None || writeError != MicError::None) ? 1 : 0;
}

// ダミーの音の出口とマイク（--dump-png・--self-test。名前は Frame で見た AB13X と、よくある機器の例）
constexpr const char* kFakeAb13xOut = "alsa_output.usb-Generic_AB13X_USB_Audio_202405280846-00.analog-stereo";
constexpr const char* kFakeAb13xIn = "alsa_input.usb-Generic_AB13X_USB_Audio_202405280846-00.analog-stereo";
constexpr const char* kFakeBluetooth = "bluez_output.AA_BB_CC_DD_EE_FF";

/**
 * ダミーの音の出口とマイクの一覧（Frame のスピーカー・AB13X・Bluetooth のイヤホンがつながっている）。
 * @param earphones 今の出口を AB13X にするか（false なら Frame のスピーカー）
 * @param externalMic 今のマイクを AB13X にするか
 * @return 一覧
 */
AudioDevices fakeDevices(bool earphones, bool externalMic) {
    AudioDevices d;
    d.known = d.defaultsKnown = true;
    d.outputs = {{kBuiltinSpeakerKey, "alsa_loopback_device.stereo.alsa_output.platform-sound.HiFi__Speaker__sink", 126,
                  "Built-in Audio", true},
                 {kFakeAb13xOut, kFakeAb13xOut, 65, "AB13X USB Audio", false},
                 {kFakeBluetooth, "bluez_output.AA_BB_CC_DD_EE_FF.1", 141, "WH-1000XM4", false}};
    d.inputs = {{kBuiltinMicKey, "alsa_loopback_device.alsa_input.platform-sound.HiFi__Mic__source", 69, "Built-in Audio", true},
                {kFakeAb13xIn, kFakeAb13xIn, 114, "AB13X USB Audio", false}};
    d.defaultOutputKey = earphones ? kFakeAb13xOut : kBuiltinSpeakerKey;
    d.defaultSinkName = d.findOutput(d.defaultOutputKey)->nodeName;
    d.defaultInputKey = externalMic ? kFakeAb13xIn : kBuiltinMicKey;
    d.defaultSourceName = d.findInput(d.defaultInputKey)->nodeName;
    return d;
}

/**
 * ダミーの出口ごとの設定（つながっている 3 つと、前に使った出口 past 個）。
 * @param past 前に使った出口の数
 * @return 出口ごとの設定
 */
std::map<std::string, OutputProfile> fakeProfiles(int past) {
    std::map<std::string, OutputProfile> outputs;
    OutputProfile speaker = firstProfile(kBuiltinSpeakerKey, "Built-in Audio");
    speaker.lastUsed = "2026-10-10";
    outputs[kBuiltinSpeakerKey] = speaker;
    OutputProfile ab13x = firstProfile(kFakeAb13xOut, "AB13X USB Audio");
    ab13x.lastUsed = "2026-10-10";
    outputs[kFakeAb13xOut] = ab13x;
    OutputProfile bluetooth = firstProfile(kFakeBluetooth, "WH-1000XM4");
    bluetooth.ns = true;
    bluetooth.lastUsed = "2026-10-09";
    outputs[kFakeBluetooth] = bluetooth;
    // 前に使った出口（いまはつながっていない）。日付の新しい順に並ぶ
    const struct {
        const char* key;
        const char* name;
        const char* date;
        bool ns;
    } examples[] = {
        {"bluez_output.11_22_33_44_55_66", "Galaxy Buds2 Pro", "2026-10-08", false},
        {"alsa_output.usb-Logitech_PRO_X_Wireless_Gaming_Headset-00.analog-stereo", "PRO X Wireless Gaming Headset",
         "2026-09-30", true},
        {"alsa_output.usb-Apple_USB-C_to_3.5mm_Headphone_Jack_Adapter-00.analog-stereo",
         "USB-C to 3.5mm Headphone Jack Adapter", "2026-09-12", false},
        {"bluez_output.77_88_99_AA_BB_CC", "AirPods Pro", "2026-09-03", false},
        {"bluez_output.DD_EE_FF_00_11_22", "Soundcore Liberty 4", "2026-08-21", true},
        {"alsa_output.usb-Creative_Sound_BlasterX_G1-00.analog-stereo", "Sound BlasterX G1", "2026-08-02", false},
    };
    for (int i = 0; i < past; ++i) {
        const auto& e = examples[i % 6];
        const std::string key = i < 6 ? std::string(e.key) : std::string(e.key) + "_" + std::to_string(i);
        OutputProfile p = firstProfile(key, i < 6 ? e.name : std::string(e.name) + " " + std::to_string(i / 6 + 1));
        p.ns = e.ns;
        p.lastUsed = e.date;
        outputs[key] = p;
    }
    return outputs;
}

/**
 * 見た目の確認用のダミーの状態を作る。
 * @param options コマンドライン（--fake-*）
 * @return 状態
 */
MicState fakeState(const Options& options) {
    MicState state;
    if (options.fakeLoading) return state;
    const bool earphones = options.fakeOutput == "earphones";
    state.loaded = true;
    state.echoKnown = true;
    // --fake-echo を付けなければ、今の出口のプリセット（スピーカー = オン、イヤホン = オフ）に合わせる
    state.echo = options.fakeEchoGiven ? options.fakeEcho : !earphones;
    state.nsKnown = true;
    state.ns = options.fakeNs;
    state.devices = fakeDevices(earphones, options.fakeExternalMic);
    state.linksKnown = true;
    state.inUse = !options.fakeIdle;
    state.muteKnown = true;
    state.muted = options.fakeMuted;
    if (state.inUse) {
        // tracker と同じ: 使用中は EQ が必ず入り、エコー除去・ノイズ除去は設定しだい
        state.chain.push_back({ChainStage::Kind::Eq, "eq"});
        if (state.echo) state.chain.push_back({ChainStage::Kind::EchoCancel, "echo_cancel"});
        if (state.ns) state.chain.push_back({ChainStage::Kind::NoiseSuppression, "ns"});
    }
    state.autostart = options.fakeAutostart;
    state.nsParams.nodeKnown = true;
    state.nsParams.nodeId = 53;
    state.nsParams.vadKnown = true;
    state.nsParams.vad = options.fakeNsVad;
    state.nsParams.graceKnown = true;
    state.nsParams.grace = options.fakeNsGrace;
    const MicError error = options.fakeError;
    if (error == MicError::WriteSettings || error == MicError::WriteAutostart || error == MicError::WriteEcho ||
        error == MicError::WriteNs || error == MicError::WriteMute || error == MicError::WriteMuteOn ||
        error == MicError::WriteOutput || error == MicError::WriteInput) {
        state.writeError = error;
    } else if (error != MicError::None) {
        state.readError = error;
        if (error == MicError::ReadSettings || error == MicError::NotInstalled) {
            state.echoKnown = false;
            state.nsKnown = false;
        }
        if (error == MicError::ReadLinks) {
            state.linksKnown = false;
            state.inUse = false;
            state.chain.clear();
        }
    }
    return state;
}

/**
 * 見た目の確認用のダミーの声のチェックの状態を作る。
 * @param options コマンドライン（--fake-recording・--fake-history など）
 * @return 状態（履歴の音声は無音、波形だけそれらしく作る）
 */
VoiceView fakeVoiceView(const Options& options) {
    VoiceView view;
    view.recording = options.fakeRecording;
    view.recordSec = 3.4;
    view.levelDb = -14.2f;
    view.error = options.fakeVoiceError;
    // 新しい順。録ったときの出口・マイク・設定をいろいろにして、聞き比べの様子にする（見本と同じ並び）
    const double lengths[] = {3.2, 5.0, 4.1, 2.4, 8.1};
    const bool echoes[] = {false, true, false, true, true};
    const bool nss[] = {false, false, false, true, false};
    const bool speakers[] = {false, true, false, true, true};
    const bool externals[] = {false, false, true, false, false};
    const std::time_t now = std::time(nullptr);
    for (int i = 0; i < options.fakeHistory; ++i) {
        auto clip = std::make_shared<VoiceClip>();
        clip->id = static_cast<uint64_t>(100 - i);
        clip->recordedAt = now - 90 * (i + 1);
        clip->echoKnown = clip->nsKnown = true;
        clip->echo = echoes[i];
        clip->ns = nss[i];
        clip->outputKnown = true;
        clip->outputSpeaker = speakers[i];
        clip->outputName = speakers[i] ? "Built-in Audio" : "AB13X USB Audio";
        clip->externalMic = externals[i];
        clip->micName = "AB13X USB Audio";
        clip->nsParamsKnown = true;
        clip->nsVad = i == 3 ? 10.0 : kNsVadDefault;      // 4 件目は強さを変えて録った例
        clip->nsGrace = i == 3 ? 800.0 : kNsGraceDefault;
        clip->samples.assign(static_cast<size_t>(lengths[i] * kVoiceRate), 0);
        clip->wave.resize(kWaveBins);
        for (int b = 0; b < kWaveBins; ++b) {
            // 話し声らしく、ふくらみと切れ目のある包絡にする
            const double t = static_cast<double>(b) / kWaveBins;
            const double envelope = std::fabs(std::sin(t * (9 + i * 2))) * (0.35 + 0.65 * std::fabs(std::sin(t * 3.1 + i)));
            clip->wave[b] = static_cast<float>(0.02 + 0.5 * envelope * envelope);
        }
        clip->peakDb = -6.0f - i;
        view.clips.push_back(clip);
    }
    if (options.fakePlaying >= 0 && options.fakePlaying < static_cast<int>(view.clips.size())) {
        view.playing = true;
        view.playingId = view.clips[options.fakePlaying]->id;
        view.playSec = view.clips[options.fakePlaying]->seconds() * 0.4;
    }
    return view;
}

/**
 * 見た目の確認用のダミーの更新の状態を作る（--dump-png 用）。
 * @param options コマンドライン（--fake-update）
 * @return 状態
 */
frame_updater::UpdateStatus fakeUpdateStatus(const Options& options) {
    using frame_updater::UpdateState;
    frame_updater::UpdateStatus status;
    status.current = FRAME_MIC_TUNER_VERSION;
    const std::string& state = options.fakeUpdate;
    if (state == "uptodate") {
        status.state = UpdateState::UpToDate;
    } else if (state == "checking") {
        status.state = UpdateState::UpToDate;
        status.checking = true;
    } else if (state == "available") {
        status.state = UpdateState::Available;
        status.latest = "9.9.9";
        status.url = "https://github.com/" + std::string(kUpdateRepo) + "/releases/tag/v9.9.9";
        status.installable = true;
    } else if (state == "manual") {
        status.state = UpdateState::Available;
        status.latest = "9.9.9";
        status.url = "https://github.com/" + std::string(kUpdateRepo) + "/releases/tag/v9.9.9";
        status.installable = false;
        status.reason = "no-checksums";
    } else if (state == "installing") {
        status.state = UpdateState::Installing;
        status.step = "download";
        status.version = "9.9.9";
    } else if (state == "installed") {
        status.state = UpdateState::Installed;
        status.version = "9.9.9";
    } else if (state == "checkfailed") {
        status.state = UpdateState::CheckFailed;
        status.error = "network";
    } else if (state == "installfailed") {
        status.state = UpdateState::InstallFailed;
        status.error = "checksum-mismatch";
    } else {
        status.state = UpdateState::Unknown;  // "unknown" か、指定なし
    }
    return status;
}

/**
 * ほかのアプリの一覧の設定（このアプリ自身の名前と、このアプリで使うアプリとその理由）。
 * @return 設定（一覧の置き場所・ホームは今のユーザーのもの）
 */
frame_apps::ManagerConfig makeAppsConfig() {
    frame_apps::ManagerConfig cfg;
    cfg.self = kUpdateAppName;  // "frame-mic-tuner"（一覧に自分は出さない）
    cfg.related = {{"frame-aux-shortcuts", uiText(Language::Ja).appsReasonAux, uiText(Language::En).appsReasonAux}};
    return cfg;
}

/**
 * このアプリの言語を frame-apps の言語にする（frame-apps は日英だけ。それ以外は英語の説明）。
 * @param language 言語
 * @return frame-apps の言語
 */
frame_apps::Lang appsLang(Language language) {
    return language == Language::Ja ? frame_apps::Lang::Ja : frame_apps::Lang::En;
}

/**
 * 見た目の確認用のダミーのアプリの一覧（同梱の一覧から。見本と同じく eye・perf は動作中、keyboard は入っている）。
 * @param options コマンドライン（--fake-aux・--fake-apps-busy・--fake-apps-extra）
 * @param menuBusy メニューの Konsole を開いているかの書き込み先
 * @return 一覧
 */
std::vector<frame_apps::Entry> fakeApps(const Options& options, bool& menuBusy) {
    using frame_apps::AppState;
    frame_apps::ManagerConfig previewConfig = makeAppsConfig();
    previewConfig.fetch.cacheDir.clear();  // 見本は同梱の一覧で（取ってきた一覧は使わない）
    frame_apps::AppsManager apps(previewConfig);
    std::vector<std::string> busy;
    std::string rest = options.fakeAppsBusy;
    while (!rest.empty()) {
        const size_t comma = rest.find(',');
        const std::string key = rest.substr(0, comma);
        busy.push_back(key == "menu" ? std::string() : key);
        rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
    }
    const AppState aux = options.fakeAux == "running"     ? AppState::Running
                         : options.fakeAux == "installed" ? AppState::Installed
                                                          : AppState::Missing;
    apps.setForPreview({{"frameeyeosc", AppState::Running},
                        {"frame-jp-keyboard", AppState::Installed},
                        {"frame-perf-overlay", AppState::Running},
                        {"frame-aux-shortcuts", aux}},
                       busy);
    menuBusy = apps.busy("");
    std::vector<frame_apps::Entry> entries = apps.entries();
    for (int i = 0; i < options.fakeAppsExtra; ++i) {
        frame_apps::Entry extra;
        extra.app = {"frame-example-" + std::to_string(i + 1), "eye", "Ex", "[今後のアプリの説明がここに入ります]",
                     "[A future app's description]", "frame-example.service", ".local/bin/frame-example",
                     "frame-example-128.png", ""};
        extra.state = AppState::Missing;
        entries.push_back(extra);
    }
    return entries;
}

/**
 * --dump-png / --thumbnail-png: OpenVR なしでパネル（とサムネイル）を描いて PNG に書き出す。
 * @param options コマンドライン
 * @return 終了コード
 */
int runDumpPng(const Options& options) {
    Config config = loadConfigOrDefault(options.configPath);
    if (!options.language.empty()) parseLanguage(options.language, config.language);
    if (!options.tab.empty()) {
        config.tab = options.tab == "fine" ? PanelTab::Fine : options.tab == "mix" ? PanelTab::Mix : PanelTab::Quick;
    }
    FontSet fonts;
    fonts.load(kFontPath, kBoldFontPath);
    if (!options.pngPath.empty()) {
        // 設定ファイルは読むだけ（ダミーのときは出口ごとの設定もダミーにする。どちらも書かない）
        PanelModel model;
        if (options.fake) {
            model.state = fakeState(options);
            config.outputs = fakeProfiles(options.fakePastOutputs);
            config.outputsSaved = true;
            if (model.state.loaded) {
                // 今の出口の覚えた設定は、今かかっている値と同じにしておく（自動でかけたあとの状態）
                OutputProfile& active = config.outputs[model.state.devices.defaultOutputKey];
                active.echo = model.state.echo;
                active.ns = model.state.ns;
                active.nsVad = model.state.nsParams.vad;
                active.nsGrace = model.state.nsParams.grace;
                model.activeOutputKey = model.state.devices.defaultOutputKey;
            }
        } else {
            model.state = readMicState();
            model.state.devices = readAudioDevices();
            model.activeOutputKey = model.state.devices.defaultOutputKey;
        }
        model.voice = fakeVoiceView(options);
        model.update = fakeUpdateStatus(options);
        model.apps = fakeApps(options, model.menuBusy);
        if (options.fakeSwitched) {
            model.switchNotice = true;
            model.switchKey = model.activeOutputKey;
        }
        MicPanel panel(fonts);
        const bool appsOverlay = options.overlay == "apps" || options.overlay == "confirm" || options.overlay == "list-confirm";
        panel.showForPreview(options.view == "apps" || appsOverlay ? PanelView::Apps : PanelView::Settings,
                             options.overlay == "output" ? PanelOverlay::OutputPicker
                             : options.overlay == "mic"  ? PanelOverlay::MicPicker
                             : options.overlay == "apps"    ? PanelOverlay::AppsList
                             : options.overlay == "suggest" ? PanelOverlay::MixSuggest
                                                            : PanelOverlay::None);
        if (options.overlay == "confirm" || options.overlay == "list-confirm") {
            panel.openConfirmForPreview(options.fakeConfirm == "menu" ? std::string() : options.fakeConfirm,
                                        options.overlay == "list-confirm");
        }
        if (!options.fakeLaunch.empty()) {
            using frame_apps::LaunchResult;
            panel.setLaunchResult(options.fakeLaunch == "busy"        ? LaunchResult::Busy
                                  : options.fakeLaunch == "nodisplay" ? LaunchResult::NoDisplay
                                  : options.fakeLaunch == "nokonsole" ? LaunchResult::NoKonsole
                                                                      : LaunchResult::Failed);
        }
        if (options.fakeMix >= 0 || options.overlay == "suggest") {
            if (options.tab.empty() || options.overlay == "suggest") config.tab = PanelTab::Mix;
            if (options.fakeMix >= 0) config.mixEnabled = true;
            if (options.fakeMix >= 0) {
                const UiText& names = uiText(config.language);
                const char* suggestion[] = {names.suggestMusic, names.suggestBoard, names.suggestBrowser, names.suggestGame,
                                            names.suggestVideo, "Extra", "Extra 2", "Extra 3"};
                config.mixChannels.clear();
                model.state.mix.known = true;
                model.state.mix.settingKnown = true;
                model.state.mix.enabled = true;
                model.state.mix.coreRunning = true;
                model.state.mix.vrlinkPresent = true;
                model.state.mix.routed = true;
                model.state.mix.mic.known = true;
                model.state.mix.mic.nodeId = 80;
                model.state.mix.mic.linear = 0.8;
                model.state.mix.mic.nodeName = kMixMicLevelNode;
                model.state.sourceVolumeKnown = true;
                model.state.sourceVolume = 0.8;
                for (int i = 0; i < options.fakeMix; ++i) {
                    MixChannel channel;
                    channel.id = "ch" + std::to_string(i + 1);
                    channel.name = suggestion[i];
                    config.mixChannels.push_back(channel);
                    MixChannelLive live;
                    live.id = channel.id;
                    live.running = true;
                    live.toMic.known = true;
                    live.toMic.nodeId = 100 + i;
                    live.toMic.linear = 0.35 + 0.1 * i;
                    live.toMic.nodeName = channelToMicNode(channel.id);
                    live.toPhones.known = true;
                    live.toPhones.nodeId = 200 + i;
                    live.toPhones.linear = 1.0;
                    live.toPhones.nodeName = channelToPhonesNode(channel.id);
                    if (i == 0) live.apps = {"SoundNinja"};
                    if (i == 1) live.apps = {"Zen"};
                    model.state.mix.channels.push_back(live);
                }
            }
            if (options.overlay == "suggest") {
                model.state.mix.playbacks.push_back({"Zen", 9, -1, ""});
                model.state.mix.runningApps.push_back("Notes");
            }
        }
        if (options.fakeViewing == "ab13x") panel.setViewedOutputForPreview(kFakeAb13xOut);
        if (options.fakeViewing == "past") panel.setViewedOutputForPreview("bluez_output.11_22_33_44_55_66");
        if (!options.previewScroll.empty()) {
            panel.setListScrollForPreview(options.previewScroll == "end" ? 1e9 : std::atof(options.previewScroll.c_str()));
        }
        if (options.previewQuit) panel.armQuitForPreview();
        if (options.previewDrag != PanelAction::None) panel.setDragForPreview(options.previewDrag, options.previewDragValue);
        if (options.previewUpdateConfirm) panel.armUpdateForPreview();
        if (options.fakeMix >= 0) {
            // 画像確認用。実機のピークはミックスのタブを開いているときだけ測る。
            std::vector<std::pair<std::string, float>> peaks;
            peaks.emplace_back(kDefaultSource, 0.28f);
            for (int i = 0; i < options.fakeMix; ++i) {
                peaks.emplace_back(channelSinkNode("ch" + std::to_string(i + 1)), std::min(0.95f, 0.15f + 0.22f * static_cast<float>(i)));
            }
            panel.setMixPeaks(std::move(peaks));
        }
        panel.render(config, model);
        if (!options.previewPressed.empty()) {
            // 押している見た目: 1 回描いてボタンの位置を知り、そこにポインターを置いて描き直す
            const std::string& p = options.previewPressed;
            const PanelAction action = p == "earphone" ? PanelAction::Earphone
                                       : p == "speaker" ? PanelAction::Speaker
                                       : p == "record"  ? PanelAction::Record
                                       : p == "mute"    ? PanelAction::MuteToggle
                                       : p == "apps"    ? PanelAction::ShowApps
                                                        : PanelAction::None;
            double x = 0.0;
            double y = 0.0;
            if (panel.buttonCenter(action, x, y)) {
                panel.setPointerForPreview(x, y, true);
                panel.render(config, model);
            } else {
                std::fprintf(stderr, "--preview-pressed のボタンが見つかりません: %s\n", p.c_str());
            }
        }
        if (!panel.writePng(options.pngPath)) {
            std::fprintf(stderr, "PNG を書き出せませんでした: %s\n", options.pngPath.c_str());
            return 1;
        }
        std::printf("PNG を書き出しました: %s（%dx%d）\n", options.pngPath.c_str(), panel.width(), panel.height());
    }
    if (!options.thumbnailPngPath.empty()) {
        std::vector<uint8_t> rgba;
        renderThumbnail(fonts, options.thumbnailSize, rgba, options.thumbnailPngPath);
        std::printf("サムネイルを書き出しました: %s（%dx%d）\n", options.thumbnailPngPath.c_str(), options.thumbnailSize,
                    options.thumbnailSize);
    }
    return 0;
}

/**
 * pw-metadata -n filters の filter.smart.disabled を 1 行にまとめて表示する（--test-record 用）。
 * @param when いつの表示か
 */
void printFilterMetadata(const char* when) {
    const CommandResult result = runCommand({"pw-metadata", "-n", "filters"});
    std::string line;
    size_t pos = 0;
    while ((pos = result.out.find("id:", pos)) != std::string::npos) {
        const size_t end = result.out.find('\n', pos);
        const std::string entry = result.out.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        const size_t id = entry.find(' ');
        const size_t value = entry.find("value:'");
        if (id != std::string::npos && value != std::string::npos) {
            line += " " + entry.substr(0, id) + "=" + entry.substr(value + 7, entry.find('\'', value + 7) - value - 7);
        }
        pos = end == std::string::npos ? result.out.size() : end;
    }
    if (line.empty()) line = " （読めません: " + describeCommand({"pw-metadata", "-n", "filters"}, result) + "）";
    std::printf("  [%s] pw-metadata -n filters の filter.smart.disabled:%s\n", when, line.c_str());
}

/**
 * --test-record: OpenVR なしで、既定の入力から録音 → ピークと長さを表示 → メモリから既定の出力へ再生。
 * 音声はメモリの中だけで、ファイルにもログにも書かない。
 * @param options コマンドライン
 * @return 終了コード
 */
int runTestRecord(const Options& options) {
    const MicState settings = readMicState(false);
    std::printf("録ったときの設定: %s\n", describeChain(settings).c_str());
    printFilterMetadata("録音前");
    VoiceCheck voice;
    if (!voice.startRecording(settings)) {
        std::printf("録音を始められませんでした\n");
        return 1;
    }
    const double start = nowSeconds();
    bool checked = false;
    float loudest = -120.0f;
    int ticks = 0;
    while (!gStopRequested && voice.recording() && nowSeconds() - start < options.testRecordSec) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        voice.update();
        const VoiceView view = voice.view();
        loudest = std::max(loudest, view.levelDb);
        if (++ticks % 5 == 0) std::printf("  %.1f 秒: メーター %.1f dBFS\n", view.recordSec, view.levelDb);
        if (!checked && nowSeconds() - start > 1.2) {
            checked = true;
            printFilterMetadata("録音中");
            const MicState during = readMicState(false);
            std::printf("  [録音中] つながり: %s\n  [録音中] マイクから音を取っているノード:", describeChain(during).c_str());
            for (const auto& user : during.users) std::printf(" %s", user.c_str());
            std::printf("\n");
        }
    }
    voice.stopRecording();
    if (voice.clips().empty()) {
        std::printf("録れた音がありません（短すぎるか失敗）\n");
        voice.shutdown();
        return 1;
    }
    const VoiceClip& clip = *voice.clips().front();
    std::printf("録音: %.2f 秒（%zu サンプル・%d Hz・mono int16）・全体のピーク %.1f dBFS・メーターの最大 %.1f dBFS\n",
                clip.seconds(), clip.samples.size(), kVoiceRate, clip.peakDb, loudest);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    printFilterMetadata("録音後 1.5 秒");

    if (!voice.play(clip.id)) {
        std::printf("再生を始められませんでした\n");
        voice.shutdown();
        return 1;
    }
    const double playStart = nowSeconds();
    double lastPos = 0.0;
    while (!gStopRequested && voice.playingId() != 0 && nowSeconds() - playStart < clip.seconds() + 5.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        lastPos = std::max(lastPos, voice.view().playSec);
        voice.update();
    }
    std::printf("再生: %.2f 秒かかって終わりました（渡した位置 %.2f 秒）\n", nowSeconds() - playStart, lastPos);
    voice.shutdown();
    return 0;
}

/**
 * バーのドラッグを終わらせ、最後の値を返す（離したとき・タブを切り替えたとき・パネルを閉じたとき）。
 * 値を送って保存するのは呼び出し側。
 * @param panel パネル
 * @param vad 判定の厳しさ（%）の書き込み先
 * @param grace 余韻（ms）の書き込み先
 * @return ドラッグしていたら true
 */
bool finishDrag(MicPanel& panel, double& vad, double& grace) {
    if (!panel.dragging()) return false;
    const PanelAction action = panel.dragAction();
    const bool ns = action == PanelAction::NsVadSlider || action == PanelAction::NsGraceSlider;
    if (ns) panel.displayedNsValues(vad, grace);  // ドラッグ中のバーはその値、もう 1 本は表示していた値
    panel.endDrag();                              // ドラッグと押している見た目を終わらせる
    return ns;
}

/**
 * --self-test: OpenVR も外部コマンドも使わずに、判定の関数を決まった入力で試す。
 * 自己修復のオーバーレイの判定（FindOverlay の結果の読み方）と、wpctl・pw-link・systemctl・pw-dump の出力の読み取り。
 * @return すべて合えば 0、1 つでも違えば 1
 */
int runSelfTest() {
    int failures = 0;
    int total = 0;
    /**
     * 1 つの確かめの結果を出す。
     * @param what 何を確かめたか
     * @param ok 合っていれば true
     */
    const auto expect = [&](const char* what, bool ok) {
        ++total;
        if (!ok) ++failures;
        std::printf("%s  %s\n", ok ? "合格  " : "不合格", what);
    };

    // 自己修復: FindOverlay の結果から、自分のオーバーレイがまだあるか
    expect("オーバーレイ: 見つかり、自分のハンドル → ある",
           judgeOverlay(true, 0x1234, 0x1234) == OverlayHealth::Alive);
    expect("オーバーレイ: 見つからない（UnknownOverlay）→ 消えている",
           judgeOverlay(false, 0, 0x1234) == OverlayHealth::Missing);
    expect("オーバーレイ: 見つかるが別のハンドル → 別のものがキーを持っている",
           judgeOverlay(true, 0x9999, 0x1234) == OverlayHealth::Replaced);
    expect("オーバーレイ: 自分がハンドルを持っていない（前の作り直しに失敗）→ 消えている扱いで作り直す",
           judgeOverlay(true, 0x9999, 0) == OverlayHealth::Missing);
    expect("オーバーレイ: 見つからず、ハンドルも無い → 消えている", judgeOverlay(false, 0, 0) == OverlayHealth::Missing);

    // wpctl settings の出力
    bool value = false;
    expect("wpctl: 「Value: true (Saved: true)」→ true",
           parseSettingValue("Value: true (Saved: true)\n", value) && value);
    expect("wpctl: 「Setting '...' not found」→ 読めない",
           !parseSettingValue("Setting 'frame-mic.echo-cancel' not found\n", value));
    const std::string list =
        "Settings:\n\n- Id: frame-mic.echo-cancel\n  Value: false\t[Saved: false]\n\n"
        "- Id: frame-mic.noise-suppression\n  Default: false\n  Value: true\t[Saved: true]\n";
    bool echo = true;
    bool ns = false;
    expect("wpctl: 一覧からエコー除去 = false・ノイズ除去 = true",
           parseSettingFromList(list, kEchoCancelKey, echo) && !echo &&
               parseSettingFromList(list, kNoiseSuppressionKey, ns) && ns);

    // systemctl --user is-enabled の出力
    expect("systemctl: not-found → ユニットファイルなし", parseAutostart("not-found\n") == Autostart::Missing);
    expect("systemctl: enabled → 有効", parseAutostart("enabled\n") == Autostart::Enabled);

    // pw-link -l の出力（イヤホン: EQ → 出力。フィルターが入っているのでマイク使用中）
    const std::string links =
        "alsa_input.platform-sound.HiFi__Mic__source:capture_FL\n"
        "  |-> eq_capture:input_FL\n"
        "eq_source:capture_MONO\n"
        "  |-> alsa_loopback_stream.alsa_input.platform-sound.HiFi__Mic__source:input_MONO\n";
    MicState state;
    parseLinks(links, state);
    expect("pw-link: マイク → EQ → 出力、使用中",
           state.linksKnown && state.inUse && state.chain.size() == 1 &&
               state.chain[0].kind == ChainStage::Kind::Eq);

    // pw-dump ns_capture の出力（ノードの id と、判定の厳しさ・余韻）
    // param 名に「)"」が入るので、区切り付きの生の文字列にする
    const std::string dump = R"json([{"id": 53, "type": "PipeWire:Interface:Node", "info": {
        "props": {"node.name": "ns_capture"},
        "params": {"Props": [{"volume": 1.0},
            {"params": ["noise_suppressor_mono:VAD Threshold (%)", 23.0,
                        "noise_suppressor_mono:VAD Grace Period (ms)", 500.0]}]}}}])json";
    const NsParams params = parseNsDump(dump);
    expect("pw-dump: ns_capture の id 53・23%・500ms",
           params.nodeKnown && params.nodeId == 53 && params.vadKnown && params.vad == 23.0 && params.graceKnown &&
               params.grace == 500.0);
    expect("ノイズ除去の強さ: 範囲外を丸める（150% → 99%、-5ms → 0ms、512ms → 510ms）",
           clampNsVad(150) == 99.0 && clampNsGrace(-5) == 0.0 && clampNsGrace(512) == 510.0);

    // wpctl get-volume @DEFAULT_AUDIO_SOURCE@ の出力（ミュート）
    bool muted = true;
    expect("wpctl get-volume: 「Volume: 1.00」→ ミュートなし", parseMute("Volume: 1.00\n", muted) && !muted);
    muted = false;
    expect("wpctl get-volume: 「Volume: 1.00 [MUTED]」→ ミュート中", parseMute("Volume: 1.00 [MUTED]\n", muted) && muted);
    muted = false;
    expect("wpctl get-volume: 「Volume: 0.35 [MUTED]」（音量が 1 以外）→ ミュート中",
           parseMute("Volume: 0.35 [MUTED]\n", muted) && muted);
    muted = true;
    expect("wpctl get-volume: 不正な出力（空・エラー文・数字なし・知らない印）→ 読めない（値は変えない）",
           !parseMute("", muted) && !parseMute("Translate ID error: '@DEFAULT_AUDIO_SOURCE@' is not a valid ID\n", muted) &&
               !parseMute("Volume: \n", muted) && !parseMute("Volume: 1.00 [WHAT]\n", muted) && muted);
    double source = 0.0;
    expect("wpctl get-volume: 音量の数字（ミュート印があっても）",
           parseSourceVolume("Volume: 1.00\n", source) && std::abs(source - 1.0) < 0.001 &&
               parseSourceVolume("Volume: 0.35 [MUTED]\n", source) && std::abs(source - 0.35) < 0.001 &&
               !parseSourceVolume("", source) && !parseSourceVolume("Volume: \n", source));

    // 設定ファイル: タブ（と言語・ノイズ除去の強さ）を保存して読み直すと同じになる（一時ファイルで試して消す）
    {
        char path[] = "/tmp/frame-mic-tuner-selftest-XXXXXX";
        const int fd = ::mkstemp(path);
        bool ok = fd >= 0;
        if (fd >= 0) ::close(fd);
        Config saved;
        saved.language = Language::En;
        saved.tab = PanelTab::Fine;
        saved.hasNsParams = true;
        saved.nsVad = 30;
        saved.nsGrace = 600;
        std::string error;
        std::vector<std::string> warnings;
        Config loaded;
        ok = ok && saveConfig(path, saved, error) && loadConfig(path, loaded, warnings, error);
        expect("設定ファイル: タブ「細かく調整」・English・30%/600ms を保存して読み直すと同じ",
               ok && loaded.tab == PanelTab::Fine && loaded.language == Language::En && loaded.hasNsParams &&
                   loaded.nsVad == 30 && loaded.nsGrace == 600 && warnings.empty());
        saved.tab = PanelTab::Quick;
        ok = saveConfig(path, saved, error) && loadConfig(path, loaded, warnings, error);
        expect("設定ファイル: タブ「かんたん」を保存して読み直すと同じ", ok && loaded.tab == PanelTab::Quick);
        ::unlink(path);
        Config fresh;
        expect("設定ファイル: タブが無いときは「かんたん」", fresh.tab == PanelTab::Quick);
    }

    // ---- パネル（OpenVR なしで描いて、当たり判定を試す） ----
    FontSet fonts;
    fonts.load(kFontPath, kBoldFontPath);

    // 文言の表: どの言語も、どのフィールドも空でない（表の並びの数の食い違いに気づく）
    for (const Language language : kLanguages) {
        const UiText& t = uiText(language);
        const char* const* field = reinterpret_cast<const char* const*>(&t);
        bool filled = true;
        for (size_t i = 0; i < sizeof(UiText) / sizeof(const char*); ++i) {
            filled = filled && field[i] != nullptr && field[i][0] != '\0';
        }
        const std::string what = std::string("文言の表: ") + languageCode(language) + " のどのフィールドも空でない";
        expect(what.c_str(), filled);
        Language parsed = Language::Ja;
        expect((std::string("言語: ") + languageCode(language) + " を読み直すと同じ").c_str(),
               parseLanguage(languageCode(language), parsed) && parsed == language && languageEndonym(language)[0] != '\0');
    }

    /**
     * 読み込んだあとの、ふつうの状態（Frame のスピーカー・内蔵マイク・使用中・ノイズ除去の強さ 23%/500ms）。
     */
    const auto loadedModel = [](bool external) {
        PanelModel model;
        MicState& s = model.state;
        s.loaded = s.echoKnown = s.nsKnown = s.linksKnown = s.inUse = s.muteKnown = true;
        s.echo = true;
        s.nsParams.nodeKnown = s.nsParams.vadKnown = s.nsParams.graceKnown = true;
        s.nsParams.nodeId = 53;
        s.nsParams.vad = kNsVadDefault;
        s.nsParams.grace = kNsGraceDefault;
        s.devices = fakeDevices(false, external);
        model.activeOutputKey = s.devices.defaultOutputKey;
        return model;
    };

    // バーのドラッグ: 細かく調整のタブで判定の厳しさのバーをつかんで動かし、タブを切り替える（= ドラッグを終わらせる）と、
    // 最後の値が返り、ドラッグは終わっている
    {
        MicPanel panel(fonts);
        Config config;
        config.tab = PanelTab::Fine;
        PanelModel model = loadedModel(false);
        model.state.ns = true;
        panel.render(config, model);
        double x = 0.0;
        double y = 0.0;
        const bool found = panel.trackCenter(PanelAction::NsVadSlider, x, y);
        const PanelHit hit = panel.pointerDown(x, y, 0.0);  // 溝の真ん中を押す → 50% 前後へ飛ぶ
        panel.pointerMove(x + 60, y);                        // 右へドラッグ
        const bool draggingBefore = panel.dragging();
        double vad = 0.0;
        double grace = 0.0;
        const bool finished = finishDrag(panel, vad, grace);
        expect("ドラッグ: 溝を押すとドラッグが始まる", found && hit.action == PanelAction::NsVadSlider && draggingBefore);
        expect("ドラッグ: タブの切り替えで終わらせると、最後の値（右へ動かした 60% 以上）と今の余韻が返る",
               finished && vad >= 60 && vad <= kNsVadMax && grace == kNsGraceDefault);
        expect("ドラッグ: 終わらせたあとはドラッグしていない", !panel.dragging());
        double again = 0.0;
        expect("ドラッグ: ドラッグしていなければ何も返さない", !finishDrag(panel, again, again));
        config.tab = PanelTab::Quick;
        panel.render(config, model);
        double qx = 0.0;
        double qy = 0.0;
        expect("タブ: かんたんのタブではバーを描かない", !panel.trackCenter(PanelAction::NsVadSlider, qx, qy));
    }

    // アプリと更新: 「更新する」の 1 回目は確認の表示（「やめる」が出る）だけ、「やめる」で元に戻る。確認中に「更新する」で更新する
    {
        MicPanel panel(fonts);
        const Config config;
        PanelModel model = loadedModel(false);
        model.update.state = frame_updater::UpdateState::Available;
        model.update.current = "0.3.0";
        model.update.latest = "9.9.9";
        model.update.installable = true;
        panel.showForPreview(PanelView::Apps, PanelOverlay::None);
        double x = 0.0;
        double y = 0.0;
        panel.render(config, model);
        const bool noConfirmFirst = !panel.buttonCenter(PanelAction::UpdateConfirmNo, x, y);
        panel.buttonCenter(PanelAction::UpdateInstall, x, y);
        const PanelHit first = panel.pointerDown(x, y, 0.0);
        panel.pointerUp();
        panel.render(config, model);
        const bool confirmShown = panel.buttonCenter(PanelAction::UpdateConfirmNo, x, y);
        const PanelHit cancel = panel.pointerDown(x, y, 0.1);
        panel.pointerUp();
        panel.render(config, model);
        const bool confirmGone = !panel.buttonCenter(PanelAction::UpdateConfirmNo, x, y) &&
                                 panel.buttonCenter(PanelAction::UpdateInstall, x, y);
        expect("更新: 「更新する」の 1 回目は何も返さず「やめる」を出す",
               noConfirmFirst && first.action == PanelAction::None && confirmShown);
        expect("更新: 「やめる」を押すと何も返さず確認が消える", cancel.action == PanelAction::None && confirmGone);
        panel.pointerDown(x, y, 0.2);
        panel.pointerUp();
        panel.render(config, model);
        panel.buttonCenter(PanelAction::UpdateConfirmYes, x, y);
        const PanelHit second = panel.pointerDown(x, y, 0.3);
        expect("更新: 確認の「更新する」で UpdateConfirmYes を返す", second.action == PanelAction::UpdateConfirmYes);
    }

    // 言語: 足のドロップダウンを開くと選べて、隣を押すと閉じるだけ
    {
        MicPanel panel(fonts);
        Config config;
        PanelModel model = loadedModel(false);
        panel.render(config, model);
        double x = 0.0;
        double y = 0.0;
        const bool hasMenu = panel.buttonCenter(PanelAction::LanguageMenu, x, y);
        const PanelHit opened = panel.pointerDown(x, y, 0.0);
        panel.pointerUp();
        panel.render(config, model);
        double dx = 0.0;
        double dy = 0.0;
        const bool hasGerman = panel.buttonCenter(PanelAction::LanguagePick, dx, dy, "de");
        const PanelHit picked = panel.pointerDown(dx, dy, 0.1);
        panel.pointerUp();
        panel.render(config, model);
        expect("言語: メニューを開くと Deutsch を選べる",
               hasMenu && opened.action == PanelAction::None && hasGerman && picked.action == PanelAction::LanguagePick &&
                   picked.key == "de");
        panel.render(config, model);
        const bool closed = !panel.buttonCenter(PanelAction::LanguagePick, dx, dy, "de");
        panel.buttonCenter(PanelAction::LanguageMenu, x, y);
        panel.pointerDown(x, y, 0.2);
        panel.pointerUp();
        panel.render(config, model);
        const bool reopened = panel.buttonCenter(PanelAction::LanguagePick, dx, dy, "de");
        panel.buttonCenter(PanelAction::LanguageMenu, x, y);
        panel.pointerDown(x, y, 0.3);
        panel.pointerUp();
        panel.render(config, model);
        expect("言語: もう一度押すと一覧が閉じる", closed && reopened && !panel.buttonCenter(PanelAction::LanguagePick, dx, dy, "de"));
    }

    // ミュート: 「ミュートする」「ミュートを解除」は同じ位置・同じ幅（どの言語でも）。確認なしの 1 回で返す。読めないときは押せない
    {
        MicPanel panel(fonts);
        Config config;
        PanelModel model = loadedModel(false);
        bool same = true;
        for (const Language language : kLanguages) {
            config.language = language;
            for (const bool muted : {false, true}) {
                model.state.muted = muted;
                panel.render(config, model);
                const frame_ui::Rect r = panel.buttonRect(PanelAction::MuteToggle);
                same = same && r.x == 324 && r.y == 22 && r.w == 220 && r.h == 56;
            }
        }
        expect("ミュート: ボタンはミュートしてもしなくても、どの言語でも同じ位置・幅（324, 22, 220×56）", same);
        double x = 0.0;
        double y = 0.0;
        panel.buttonCenter(PanelAction::MuteToggle, x, y);
        const PanelHit hit = panel.pointerDown(x, y, 0.0);
        panel.pointerUp();
        expect("ミュート: 確認なしの 1 回で MuteToggle を返す", hit.action == PanelAction::MuteToggle);
        model.state.muteKnown = false;
        panel.render(config, model);
        expect("ミュート: 読めていない間は押せない", panel.buttonRect(PanelAction::MuteToggle).w == 0);
        MicState other = model.state;
        other.muteKnown = true;
        MicState unmuted = other;
        unmuted.muted = !other.muted;
        expect("ミュート: 読めたかどうか・ミュートかどうかが変わると描き直す",
               !sameMicState(model.state, other) && !sameMicState(other, unmuted) && sameMicState(other, other));
    }

    // 音の出口を選ぶ画面: 後ろは押せない。行は離したときに返す（ドラッグならスクロールで、何も返さない）
    {
        MicPanel panel(fonts);
        Config config;
        config.outputs = fakeProfiles(6);
        config.outputsSaved = true;
        const PanelModel model = loadedModel(false);
        panel.showForPreview(PanelView::Settings, PanelOverlay::OutputPicker);
        panel.render(config, model);
        double x = 0.0;
        double y = 0.0;
        expect("出口の一覧: 重ねている間は後ろのボタン（タブ）を押せない", !panel.buttonCenter(PanelAction::TabQuick, x, y));
        expect("出口の一覧: つながっている出口に「ここから音を出す」、今の出口には出さない、前の出口に「忘れる」",
               panel.buttonCenter(PanelAction::OutputUse, x, y, kFakeAb13xOut) &&
                   !panel.buttonCenter(PanelAction::OutputUse, x, y, kBuiltinSpeakerKey) &&
                   panel.buttonCenter(PanelAction::OutputForget, x, y, "bluez_output.11_22_33_44_55_66"));
        const bool scrolled = panel.scroll(-1.0);  // スティックを下へ
        panel.render(config, model);
        const bool back = panel.scroll(100.0);       // いちばん上へ戻す
        panel.render(config, model);
        expect("出口の一覧: 入りきらないときはスティックでスクロールできる", scrolled && back && !panel.scroll(1.0));
        panel.buttonCenter(PanelAction::OutputView, x, y, kFakeAb13xOut);
        const PanelHit down = panel.pointerDown(x, y, 0.0);
        panel.pointerMove(x, y - 60);  // 押したまま上へ = 下へスクロール
        const PanelHit dragged = panel.pointerUp();
        expect("出口の一覧: 行を押したままドラッグするとスクロールで、行は選ばない",
               down.action == PanelAction::None && dragged.action == PanelAction::None &&
                   panel.overlay() == PanelOverlay::OutputPicker);
        panel.setListScrollForPreview(0);
        panel.render(config, model);
        panel.buttonCenter(PanelAction::OutputView, x, y, kFakeAb13xOut);
        panel.pointerDown(x, y, 1.0);
        const PanelHit picked = panel.pointerUp();
        panel.render(config, model);
        expect("出口の一覧: 行を押して離すと、その出口の設定を表示して閉じる",
               picked.action == PanelAction::OutputView && picked.key == kFakeAb13xOut &&
                   panel.overlay() == PanelOverlay::None && panel.viewedOutputKey() == kFakeAb13xOut &&
                   !panel.viewingActive());
        panel.resetView();
        panel.render(config, model);
        expect("出口の一覧: パネルを閉じたら今の出口の表示に戻る",
               panel.viewedOutputKey() == kBuiltinSpeakerKey && panel.viewingActive());
    }

    // ほかのアプリ: 「入れる」はまず確認の画面（コマンドを見せる）。「Konsole で開く」が呼び名を返し、開けたら確認を閉じる。
    // 一覧から開いた確認は、やめると一覧に戻る
    {
        MicPanel panel(fonts);
        const Config config;
        PanelModel model = loadedModel(false);
        model.apps = fakeApps(Options(), model.menuBusy);
        panel.showForPreview(PanelView::Apps, PanelOverlay::None);
        panel.render(config, model);
        double x = 0.0;
        double y = 0.0;
        const bool installShown = panel.buttonCenter(PanelAction::AppInstall, x, y, "aux");
        const PanelHit install = panel.pointerDown(x, y, 0.0);
        panel.pointerUp();
        panel.render(config, model);
        double lx = 0.0;
        double ly = 0.0;
        const bool launchShown = panel.buttonCenter(PanelAction::AppsConfirmLaunch, lx, ly, "aux");
        const PanelHit launch = panel.pointerDown(lx, ly, 0.1);
        panel.pointerUp();
        panel.setLaunchResult(frame_apps::LaunchResult::Started);
        expect("アプリ: いっしょに使う aux の「入れる」で確認の画面、「Konsole で開く」が aux を返し、開けたら閉じる",
               installShown && install.action == PanelAction::AppInstall && launchShown &&
                   launch.action == PanelAction::AppsConfirmLaunch && launch.key == "aux" &&
                   panel.overlay() == PanelOverlay::None);
        expect("アプリ: 確認の画面のコマンドはインストーラーのもの",
               frame_apps::installerCommand("aux") == "curl -fsSL https://frame.sasaken1102s.net | sh -s -- install aux" &&
                   frame_apps::installerCommand("") == "curl -fsSL https://frame.sasaken1102s.net | sh");
        panel.render(config, model);
        panel.buttonCenter(PanelAction::AppsOpenList, x, y);
        panel.pointerDown(x, y, 0.2);
        panel.pointerUp();
        panel.render(config, model);
        const bool listOpen = panel.overlay() == PanelOverlay::AppsList && panel.buttonCenter(PanelAction::AppsOpenMenu, x, y);
        panel.buttonCenter(PanelAction::AppInstall, x, y, "aux");
        const PanelHit down = panel.pointerDown(x, y, 0.3);
        const PanelHit up = panel.pointerUp();
        panel.render(config, model);
        panel.buttonCenter(PanelAction::AppsConfirmCancel, x, y);
        panel.pointerDown(x, y, 0.4);
        panel.pointerUp();
        expect("アプリ: 一覧の「入れる」は離したときに確認を出し、やめると一覧に戻る",
               listOpen && down.action == PanelAction::None && up.action == PanelAction::AppInstall && up.key == "aux" &&
                   panel.overlay() == PanelOverlay::AppsList);
        expect("アプリ: 一覧に自分（frame-mic-tuner）は出さず、aux が「このアプリで使う」で先頭",
               !model.apps.empty() && model.apps.front().app.name == "frame-aux-shortcuts" && model.apps.front().related &&
                   std::none_of(model.apps.begin(), model.apps.end(),
                                [](const frame_apps::Entry& e) { return e.app.name == "frame-mic-tuner"; }));
    }

    // 外付けのマイク: かんたん・細かく調整は使えず、「Frame 内蔵マイクに戻す」を出す。自動で切り替えた知らせは「元に戻す」を出す
    {
        MicPanel panel(fonts);
        Config config;
        panel.render(config, loadedModel(true));
        double x = 0.0;
        double y = 0.0;
        expect("外付けのマイク: 設定の代わりに「Frame 内蔵マイクに戻す」を出し、タブ（ミックスを含む）は押せる",
               panel.buttonCenter(PanelAction::TabQuick, x, y) && panel.buttonCenter(PanelAction::TabMix, x, y) &&
                   panel.buttonCenter(PanelAction::UseBuiltinMic, x, y));
        PanelModel switched = loadedModel(false);
        switched.switchNotice = true;
        switched.switchKey = switched.activeOutputKey;
        panel.render(config, switched);
        expect("知らせ: 自動で切り替えたあとは「元に戻す」を出す", panel.buttonCenter(PanelAction::UndoSwitch, x, y));
    }

    // ミックスのタブ: チャンネルの行、消すは 2 回、候補は使っている名前を押せない
    {
        MicPanel panel(fonts);
        Config config;
        config.language = Language::En;
        config.tab = PanelTab::Mix;
        config.mixEnabled = true;
        config.mixChannels = {{"ch1", "Music"}};
        PanelModel model = loadedModel(false);
        model.state.mix.known = true;
        model.state.mix.settingKnown = true;
        model.state.mix.enabled = true;
        model.state.mix.routed = true;
        model.state.mix.vrlinkPresent = true;
        model.state.mix.mic.known = true;
        model.state.mix.mic.linear = 1.0;
        model.state.mix.mic.nodeName = kMixMicLevelNode;
        model.state.sourceVolumeKnown = true;
        model.state.sourceVolume = 1.0;
        panel.setMixPeaks({{std::string(kDefaultSource), 0.22f}, {channelSinkNode("ch1"), 0.55f}});
        MixChannelLive live;
        live.id = "ch1";
        live.running = true;
        live.toMic.known = true;
        live.toMic.linear = 0.5;
        live.toMic.nodeName = channelToMicNode("ch1");
        live.toPhones.known = true;
        live.toPhones.linear = 1.0;
        live.toPhones.nodeName = channelToPhonesNode("ch1");
        model.state.mix.channels.push_back(live);
        panel.render(config, model);
        double x = 0.0;
        double y = 0.0;
        expect("ミックス: オン・足す・消す・名前・声の録音・2 本のバーがある",
               panel.buttonCenter(PanelAction::MixOn, x, y) && panel.buttonCenter(PanelAction::MixAdd, x, y) &&
                   panel.buttonCenter(PanelAction::MixDelete, x, y, "ch1") &&
                   panel.buttonCenter(PanelAction::MixRename, x, y, "ch1") &&
                   panel.buttonCenter(PanelAction::Record, x, y) &&
                   panel.trackCenter(PanelAction::MixMicSlider, x, y) &&
                   panel.trackCenter(PanelAction::MixToMicSlider, x, y) &&
                   panel.trackCenter(PanelAction::MixToPhonesSlider, x, y));
        double micX = 0.0;
        double micY = 0.0;
        double toMicX = 0.0;
        double toMicY = 0.0;
        double phonesX = 0.0;
        double phonesY = 0.0;
        panel.buttonCenter(PanelAction::TabMix, x, y);
        const bool wideTabs = x > 560.0;
        const bool verticalStrips = panel.trackCenter(PanelAction::MixMicSlider, micX, micY) &&
                                    panel.trackCenter(PanelAction::MixToMicSlider, toMicX, toMicY) &&
                                    panel.trackCenter(PanelAction::MixToPhonesSlider, phonesX, phonesY) && micX < toMicX &&
                                    toMicX < phonesX && std::fabs(toMicY - phonesY) < 12.0;
        panel.pointerDown(micX, micY, 2.0);
        int at100 = 0;
        bool above100 = false;
        std::string dragNode;
        double dragPercent = 0.0;
        for (int step = 0; step <= 90; ++step) {
            panel.pointerMove(micX, micY - step);
            if (!panel.mixDrag(dragNode, dragPercent)) continue;
            if (dragPercent == 100.0) ++at100;
            if (dragPercent >= 110.0) above100 = true;
        }
        panel.pointerUp();
        expect("ミックス: タブは広い列、バーは縦、100% の近くで吸い付く", wideTabs && verticalStrips && at100 >= 4 && above100);
        model.state.mix.mic.known = false;
        model.state.mix.channels[0].toMic.known = false;
        model.state.mix.channels[0].toPhones.known = false;
        panel.render(config, model);
        expect("ミックス: 音量がまだ無くてもバーは押せる",
               panel.trackCenter(PanelAction::MixMicSlider, x, y) && panel.trackCenter(PanelAction::MixToMicSlider, x, y) &&
                   panel.trackCenter(PanelAction::MixToPhonesSlider, x, y));
        model.state.mix.mic.known = true;
        model.state.mix.channels[0].toMic.known = true;
        model.state.mix.channels[0].toPhones.known = true;
        panel.buttonCenter(PanelAction::MixDelete, x, y, "ch1");
        panel.pointerDown(x, y, 1.0);
        const PanelHit first = panel.pointerUp();
        panel.render(config, model);
        panel.buttonCenter(PanelAction::MixDelete, x, y, "ch1");
        panel.pointerDown(x, y, 1.2);
        const PanelHit second = panel.pointerUp();
        expect("ミックス: 消すは 1 回目では何も返さず、2 回目で MixDelete",
               first.action == PanelAction::None && second.action == PanelAction::MixDelete && second.key == "ch1");
        panel.showForPreview(PanelView::Settings, PanelOverlay::MixSuggest);
        model.state.mix.playbacks.push_back({"Zen", 9, -1, ""});
        panel.render(config, model);
        expect("ミックス: 使っている候補は押せない。空いている候補と自分の名前は押せる",
               !panel.buttonCenter(PanelAction::MixSuggest, x, y, "Music") &&
                   panel.buttonCenter(PanelAction::MixSuggest, x, y, "Soundboard") &&
                   panel.buttonCenter(PanelAction::MixCustom, x, y));
        expect("ミックス: 動いているアプリを選べる", panel.buttonCenter(PanelAction::MixUseApp, x, y, "Zen"));
        expect("ミックス: ボイスチャットとシステム音は候補に無い",
               !panel.buttonCenter(PanelAction::MixSuggest, x, y, "Voice chat") &&
                   !panel.buttonCenter(PanelAction::MixSuggest, x, y, "System sounds"));
        model.state.mix.runningApps = {"Notes"};
        panel.render(config, model);
        expect("ミックス: 音を出していないアプリも選べて、更新ボタンがある",
               panel.buttonCenter(PanelAction::MixUseApp, x, y, "Notes") &&
                   panel.buttonCenter(PanelAction::MixRefreshApps, x, y));
        model.state.mix.runningApps.clear();
        for (int i = 0; i < 16; ++i) model.state.mix.runningApps.push_back("App" + std::to_string(i));
        panel.render(config, model);
        const bool hiddenBefore = !panel.buttonCenter(PanelAction::MixUseApp, x, y, "App12");
        const bool appScrolled = panel.scroll(-2.0);
        panel.render(config, model);
        const bool shownAfter = panel.buttonCenter(PanelAction::MixUseApp, x, y, "App12");
        expect("ミックス: ソフトの一覧はスティックでスクロールできる", hiddenBefore && appScrolled && shownAfter);
        panel.closeOverlay();
        for (int i = 2; i <= 8; ++i) config.mixChannels.push_back({"ch" + std::to_string(i), "Ch " + std::to_string(i)});
        panel.render(config, model);
        double beforeX = 0.0;
        panel.buttonCenter(PanelAction::MixDelete, beforeX, y, "ch1");
        const bool scrolled = panel.scroll(-1.0);
        panel.render(config, model);
        double afterX = 0.0;
        panel.buttonCenter(PanelAction::MixDelete, afterX, y, "ch1");
        const bool back = panel.scroll(100.0);
        expect("ミックス: チャンネルが多いときは横にスクロールできる", scrolled && back && afterX < beforeX - 40.0);
        panel.render(config, model);
        panel.buttonCenter(PanelAction::MixDelete, beforeX, y, "ch1");
        const bool stickLeft = panel.scroll(0.0, -1.0);
        panel.render(config, model);
        panel.buttonCenter(PanelAction::MixDelete, afterX, y, "ch1");
        const bool stickRight = panel.scroll(0.0, 1.0);
        panel.render(config, model);
        double backX = 0.0;
        panel.buttonCenter(PanelAction::MixDelete, backX, y, "ch1");
        expect("ミックス: スティックの左で先へ、右で戻る", stickLeft && stickRight && afterX < beforeX - 20.0 && backX > afterX + 20.0);
    }

    // 音の出口: 一覧の読み取り（Frame で取った pw-dump・pw-metadata）・キー・出口ごとの設定のかけ方・設定ファイル
    outputSelfTests(expect);
    mixSelfTests(expect);

    std::printf("%d 件中 %d 件が不合格\n", total, failures);
    return failures == 0 ? 0 : 1;
}

/**
 * install.sh が frame-update.sh を置いた場所（$XDG_DATA_HOME か ~/.local/share の下）。
 * @return パス
 */
std::string updateScriptPath() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg != nullptr && xdg[0] == '/') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        base = std::string(home != nullptr ? home : ".") + "/.local/share";
    }
    return base + "/" + kUpdateAppName + "/frame-update.sh";
}

/**
 * 常駐のロックファイルのパス（$XDG_RUNTIME_DIR の下）。
 * @return パス
 */
std::string lockFilePath() {
    // Steam から・systemd から・SSH から起動しても同じ場所になるよう、XDG_RUNTIME_DIR が無ければ /run/user/<uid> を使う
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] != '\0') return std::string(runtime) + "/frame-mic-tuner.lock";
    const std::string userRuntime = "/run/user/" + std::to_string(::getuid());
    if (::access(userRuntime.c_str(), W_OK) == 0) return userRuntime + "/frame-mic-tuner.lock";
    return "/tmp/frame-mic-tuner-" + std::to_string(::getuid()) + ".lock";
}

/**
 * 常駐のロックを取る。取れたら自分の PID を書いて、ファイルを開いたままにする（終われば OS がロックを外す）。
 * 取れなければ、ロックを持っている常駐側の PID を返す。
 * @param lockFd 取れたときのファイル（開いたままにする）の書き込み先
 * @param holderPid 取れなかったときの、常駐側の PID の書き込み先（読めなければ 0）
 * @return ロックを取れた（またはロックを使えないので、そのまま起動してよい）なら true
 */
bool acquireInstanceLock(int& lockFd, pid_t& holderPid) {
    lockFd = -1;
    holderPid = 0;
    const std::string path = lockFilePath();
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        std::fprintf(stderr, "[起動] ロックファイル %s を開けないので、二重起動の確認をせずに起動します\n", path.c_str());
        return true;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        const std::string pid = std::to_string(::getpid()) + "\n";
        if (::ftruncate(fd, 0) != 0 || ::pwrite(fd, pid.data(), pid.size(), 0) < 0) {
            std::fprintf(stderr, "[起動] ロックファイルに PID を書けませんでした\n");
        }
        lockFd = fd;
        return true;
    }
    // 常駐側がロックを取った直後で、まだ PID を書いていないことがあるので少し待って読み直す
    for (int attempt = 0; attempt < 10 && holderPid <= 0; ++attempt) {
        char buffer[32] = {};
        const ssize_t n = ::pread(fd, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) holderPid = static_cast<pid_t>(std::atol(buffer));
        if (holderPid <= 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::close(fd);
    return false;
}

/**
 * このプロセスが systemd の frame-mic-tuner.service として起動されたか。
 * INVOCATION_ID だけでは決めない（Frame の Steam は steam.service で動いていて、＋から起動した子にも
 * INVOCATION_ID が引き継がれる）。自分の cgroup がこのサービスのものかで確かめる。
 * @return サービスとして起動されたなら true
 */
bool startedByOwnService() {
    const char* invocation = std::getenv("INVOCATION_ID");
    if (invocation == nullptr || invocation[0] == '\0') return false;
    std::ifstream cgroup("/proc/self/cgroup");
    std::string line;
    while (std::getline(cgroup, line)) {
        if (line.size() >= std::strlen("/frame-mic-tuner.service") &&
            line.compare(line.size() - std::strlen("/frame-mic-tuner.service"), std::string::npos,
                         "/frame-mic-tuner.service") == 0) {
            return true;
        }
    }
    return false;
}

/**
 * オーバーレイとして常駐する。SteamVR が無ければ数秒おきに待ち、終了の知らせで静かに終わる。
 * すでに常駐していれば、そちらにパネルを開くよう知らせてすぐ終わる（VR_Init はしない）。
 * @param options コマンドライン
 * @return 終了コード
 */
int runOverlay(const Options& options) {
    int lockFd = -1;
    pid_t holderPid = 0;
    if (!acquireInstanceLock(lockFd, holderPid)) {
        // systemd（Restart=always）から起動されたのに常駐がいるときは、5 秒ごとにパネルが開き続けないよう
        // 知らせを送らずに、起動し直されない終了コードで静かに終わる
        if (startedByOwnService()) {
            std::fprintf(stderr, "[起動] すでに常駐しています（PID %d）。サービスからの起動なので何もせずに終わります\n",
                         static_cast<int>(holderPid));
            return kExitCodeUserQuit;
        }
        if (holderPid > 0 && ::kill(holderPid, SIGUSR1) == 0) {
            std::fprintf(stderr, "[起動] すでに常駐しています（PID %d）。パネルを開いて終わります\n",
                         static_cast<int>(holderPid));
            return 0;
        }
        std::fprintf(stderr, "[起動] すでに常駐しているようですが、知らせを送れませんでした（PID %d）\n",
                     static_cast<int>(holderPid));
        return 1;
    }

    Config config = loadConfigOrDefault(options.configPath);
    FontSet fonts;
    fonts.load(kFontPath, kBoldFontPath);
    MicPanel panel(fonts);
    MicWorker worker;
    worker.start();
    // 保存したノイズ除去の強さがあれば、ノードが見つかりしだいかける（SteamOS は PipeWire の起動のたびに既定値に戻す）。
    // 出口ごとの設定が読めたら、そちら（OutputSync が今の出口の設定をかける）が優先する
    if (config.hasNsParams) worker.setDesiredNsParams(config.nsVad, config.nsGrace);
    worker.setMixPins(config.mixPins);
    if (config.mixEnabled) {
        MicCommand mix {MicCommand::Kind::MixEnable};
        mix.value = true;
        mix.text = mixChannelBlob(config.mixChannels);
        worker.request(mix);
    }
    // 音の出口ごとの設定: 起動したとき・出口が変わったときに、その出口の覚えた設定をかける（パネルを閉じていても）
    OutputSync outputSync([&worker](const MicCommand& command) { return worker.request(command); });
    /**
     * 設定ファイルに保存する（失敗はログだけ）。
     */
    const auto saveNow = [&]() {
        std::string error;
        if (!saveConfig(options.configPath, config, error)) std::fprintf(stderr, "[設定] 保存に失敗: %s\n", error.c_str());
    };
    VoiceCheck voice;
    MixMeter meter;
    std::string meterKey;
    VrOverlay vr;
    // ほかのアプリの一覧（vendor/frame-apps）。サイトから取ってくるのは「新しい版の確認」がオンのときだけ（1 時間に 1 回まで。
    // 取れなければ前回の分 → 同梱の一覧）
    frame_apps::AppsManager apps(makeAppsConfig());
    apps.refreshNow();
    apps.setFetchEnabled(config.updateCheck);
    if (apps.fetchIfDue()) std::fprintf(stderr, "[アプリ] サイトから一覧を取りに行きます\n");
    std::string lastAppsError;

    // 新しい版の確認・更新（vendor/frame-updater）。古い版から来て install-args が無いときの既定はオプションなし
    frame_updater::UpdaterConfig updaterConfig;
    updaterConfig.script = updateScriptPath();
    updaterConfig.app = kUpdateAppName;
    updaterConfig.repo = kUpdateRepo;
    updaterConfig.currentVersion = FRAME_MIC_TUNER_VERSION;
    updaterConfig.assetPattern = kUpdateAssetPattern;
    frame_updater::UpdateChecker updater(updaterConfig);
    uint64_t drawnUpdateRevision = updater.revision();

    // SteamVR を待つ
    std::string lastMessage;
    while (!gStopRequested) {
        std::string message;
        const VrOverlay::ConnectResult result = vr.connect(panel.width(), panel.height(), message);
        if (result == VrOverlay::ConnectResult::Ok) break;
        if (message != lastMessage) {
            if (result == VrOverlay::ConnectResult::NotRunning) {
                std::fprintf(stderr, "[VR] SteamVR が起動していないので待ちます（3 秒おきに再試行）\n");
            } else {
                std::fprintf(stderr, "[VR] 接続に失敗: %s（3 秒後に再試行）\n", message.c_str());
            }
            lastMessage = message;
        }
        sleepInterruptible(3.0);
        if (gShowRequested) {
            gShowRequested = 0;
            std::fprintf(stderr, "[起動] パネルを開く知らせが来ましたが、SteamVR につながっていません\n");
        }
    }
    if (gStopRequested) {
        worker.stop();
        if (lockFd >= 0) ::close(lockFd);
        return 0;
    }
    std::fprintf(stderr, "[VR] SteamVR につながりました\n");
    MicState state;
    uint64_t drawnVersion = worker.snapshot(state);
    if (outputSync.update(state, config, todayText())) saveNow();  // ワーカーが SteamVR を待つ間に読み終えていたとき
    VoiceView voiceView;
    /**
     * 今の状態からパネルに渡すものを作る。
     * @return 描くときに渡すもの
     */
    const auto makeModel = [&]() {
        PanelModel model;
        model.state = state;
        model.voice = voiceView;
        model.update = updater.status();
        model.activeOutputKey = outputSync.activeKey();
        model.switchNotice = outputSync.notice().shown;
        model.switchKey = outputSync.notice().key;
        model.apps = apps.entries();
        model.menuBusy = apps.busy("");
        return model;
    };
    {
        std::vector<uint8_t> thumbnail;
        renderThumbnail(fonts, kThumbnailSize, thumbnail);
        vr.submitThumbnail(thumbnail.data(), kThumbnailSize);
        // パネルにも最初の 1 枚（読み込み中）を入れておく（初めて選ばれたとき、画像が無い瞬間を作らない）
        panel.render(config, makeModel());
        vr.submitPanel(panel.toRgba().data());
        vr.logOverlayState("接続直後");
    }

    double lastVoiceFrame = 0.0;
    // ノイズ除去の強さ: バーのドラッグ中は 100ms おきに最後の値だけ送り、離したときに必ず 1 回送って保存する
    double lastNsSendAt = -1.0;
    double sentVad = -1.0;
    double sentGrace = -1.0;
    std::string sentMixNode;
    double sentMixLinear = -1.0;
    enum class MixEdit { None, Add, Rename };
    MixEdit mixEdit = MixEdit::None;
    std::string mixRenameId;
    bool dirty = true;
    bool wasVisible = false;
    bool firstSubmit = true;
    bool userQuit = false;
    // プリセット: 書き込みと読み直しが終わるまで（受付番号まで終わるまで）、押したカードを選択中の見た目で保つ
    uint64_t presetTicket = 0;
    /**
     * 表示している出口の設定を書き換えて保存する（パネルの操作。自動の切り替えの知らせは消す）。
     * 今の出口なら、前の版のキー（ns_vad_*）にも強さを写す。
     * @param change 書き換え
     */
    const auto changeViewedProfile = [&](const std::function<void(OutputProfile&)>& change) {
        outputSync.dismissNotice();
        const std::string key = panel.viewedOutputKey();
        if (key.empty()) return;
        bool created = false;
        OutputProfile& profile = ensureProfile(config, key, state.devices, created);
        change(profile);
        if (key == outputSync.activeKey()) {
            config.hasNsParams = true;
            config.nsVad = profile.nsVad;
            config.nsGrace = profile.nsGrace;
        }
        saveNow();
    };
    /**
     * 表示している出口が、今かけている出口か（そうなら書き込みもする。ほかの出口は覚えるだけ）。
     * @return 今の出口なら true
     */
    const auto viewingLive = [&]() { return panel.viewingActive(); };
    /**
     * プリセットにする。イヤホン = エコー除去オフ・ノイズ除去オフ、スピーカー = エコー除去オン・ノイズ除去オフ。
     * 今の出口なら、ワーカーが 2 つ書いてから 1 回だけ読み直す。ノイズ除去のバーの値は変えない。
     * @param action Earphone か Speaker
     */
    const auto applyPreset = [&](PanelAction action) {
        const bool speaker = action == PanelAction::Speaker;
        std::fprintf(stderr, "[操作] %s（%s）\n",
                     speaker ? "スピーカー（エコー除去オン・ノイズ除去オフ）" : "イヤホン（エコー除去オフ・ノイズ除去オフ）",
                     viewingLive() ? "今の出口" : panel.viewedOutputKey().c_str());
        if (viewingLive()) {
            presetTicket = worker.request(MicCommand::applySettings(speaker, false, false, 0, 0, false));
            outputSync.noteWrite(presetTicket);
            panel.holdPreset(action, nowSeconds() + 8.0);  // 書き込みが詰まっても、8 秒で実際の値の表示に戻す
        }
        changeViewedProfile([speaker](OutputProfile& p) {
            p.echo = speaker;
            p.ns = false;
        });
    };
    /**
     * エコー除去・ノイズ除去を 1 つ切り替える（表示している出口の設定として覚え、今の出口なら書く）。
     * @param action EchoOn / EchoOff / NsOn / NsOff
     */
    const auto applyToggle = [&](PanelAction action) {
        const bool echo = action == PanelAction::EchoOn || action == PanelAction::EchoOff;
        const bool on = action == PanelAction::EchoOn || action == PanelAction::NsOn;
        std::fprintf(stderr, "[操作] %s %s（%s）\n", echo ? "エコー除去" : "ノイズ除去", on ? "オン" : "オフ",
                     viewingLive() ? "今の出口" : panel.viewedOutputKey().c_str());
        if (viewingLive()) {
            MicCommand command {echo ? MicCommand::Kind::SetEcho : MicCommand::Kind::SetNs};
            command.value = on;
            outputSync.noteWrite(worker.request(command));
        }
        changeViewedProfile([echo, on](OutputProfile& p) { (echo ? p.echo : p.ns) = on; });
    };
    /**
     * ノイズ除去の強さを変える。今の出口ならワーカーに頼み（前に送った値と同じなら頼まない）、読み直しが追いつくまで表示を保つ。
     * @param vad 判定の厳しさ（%）
     * @param grace 余韻（ms）
     * @param save 出口の設定として保存するか（離したとき・標準に戻す）
     */
    const auto sendNsParams = [&](double vad, double grace, bool save) {
        vad = clampNsVad(vad);
        grace = clampNsGrace(grace);
        if (viewingLive() && (vad != sentVad || grace != sentGrace)) {
            MicCommand command {MicCommand::Kind::SetNsParams};
            command.vad = vad;
            command.grace = grace;
            outputSync.noteWrite(worker.request(command));
            sentVad = vad;
            sentGrace = grace;
            lastNsSendAt = nowSeconds();
        }
        panel.holdNsValues(vad, grace, nowSeconds() + 1.5);
        if (!save) return;
        changeViewedProfile([vad, grace](OutputProfile& p) {
            p.nsVad = vad;
            p.nsGrace = grace;
        });
        std::fprintf(stderr, "[ノイズ除去] 判定の厳しさ %.0f%%・余韻 %.0fms を保存しました（%s）\n", vad, grace,
                     viewingLive() ? "今の出口" : panel.viewedOutputKey().c_str());
    };
    /**
     * タブを切り替えて保存する。バーをドラッグしていたら、先に最後の値を送って保存する。
     * @param tab 切り替え先
     */
    const auto mixNodeIdOf = [&](const std::string& node) {
        for (const MixChannelLive& channel : state.mix.channels) {
            if (channel.toMic.nodeName == node && channel.toMic.known) return channel.toMic.nodeId;
            if (channel.toPhones.nodeName == node && channel.toPhones.known) return channel.toPhones.nodeId;
        }
        return -1;
    };
    const auto sendMixVolume = [&](const std::string& node, double percent) {
        const int nodeId = mixNodeIdOf(node);
        if (node != kDefaultSource && nodeId < 0) {
            panel.wantMixVolume(node, percent);
            return;
        }
        panel.clearWantedMix(node);
        const double linear = clampMixLinear(percent);
        if (node == sentMixNode && std::abs(linear - sentMixLinear) < 0.001) return;
        MicCommand command {MicCommand::Kind::SetMixVolume};
        command.nodeId = nodeId;
        command.nodeName = node;
        command.vad = linear;
        worker.request(command);
        sentMixNode = node;
        sentMixLinear = linear;
        lastNsSendAt = nowSeconds();
        panel.holdMixVolume(node, linear * 100.0, nowSeconds() + 4.0);
    };
    const auto commitDrag = [&](bool save) {
        std::string node;
        double percent = 0.0;
        if (panel.mixDrag(node, percent)) {
            sentMixNode.clear();
            sendMixVolume(node, percent);
            panel.endDrag();
            return;
        }
        double vad = 0.0;
        double grace = 0.0;
        if (finishDrag(panel, vad, grace)) sendNsParams(vad, grace, save);
    };
    const auto showMixKeyboard = [&](const std::string& existing) {
        const UiText& text = uiText(config.language);
        if (!vr.showKeyboard(text.mixSuggestTitle, existing, kMixNameMaxChars * 3)) {
            panel.setNameError(text.errWriteMix, nowSeconds() + 4.0);
            mixEdit = MixEdit::None;
        }
    };
    /**
     * 検査を通った名前でチャンネルを足す。
     * @param normalized 整えた名前
     */
    const auto addMixChannel = [&](const std::string& normalized) {
        if (static_cast<int>(config.mixChannels.size()) >= kMixChannelMax) return std::string();
        const std::string id = nextChannelId(config.mixChannels);
        if (id.empty()) return std::string();
        config.mixChannels.push_back({id, normalized});
        config.tab = PanelTab::Mix;
        saveNow();
        panel.closeOverlay();
        MicCommand command {MicCommand::Kind::AddChannel};
        command.nodeName = id;
        command.text = normalized;
        command.value = config.mixEnabled;
        worker.request(command);
        std::fprintf(stderr, "[ミックス] チャンネル %s「%s」を足しました\n", id.c_str(), normalized.c_str());
        return id;
    };
    const auto rememberPin = [&](const std::string& app, const std::string& id) {
        for (MixPin& pin : config.mixPins) {
            if (pin.app != app) continue;
            pin.id = id;
            saveNow();
            worker.setMixPins(config.mixPins);
            return;
        }
        config.mixPins.push_back({app, id});
        saveNow();
        worker.setMixPins(config.mixPins);
    };
    const auto switchTab = [&](PanelTab tab) {
        commitDrag(true);
        if (config.tab == tab) return;
        config.tab = tab;
        const char* name = tab == PanelTab::Quick ? "かんたん" : tab == PanelTab::Fine ? "細かく調整" : "ミックス";
        std::fprintf(stderr, "[操作] タブ: %s\n", name);
        saveNow();
    };
    /**
     * 既定の出力か入力を切り替える（ワーカーが wpctl set-default して読み返す）。
     * @param output 出力なら true
     * @param key 出口・マイクのキー
     */
    const auto setDefault = [&](bool output, const std::string& key) {
        const AudioEndpoint* endpoint = output ? state.devices.findOutput(key) : state.devices.findInput(key);
        if (endpoint == nullptr || endpoint->nodeId < 0) {
            std::fprintf(stderr, "[操作] %s %s が一覧にありません\n", output ? "出口" : "マイク", key.c_str());
            return;
        }
        std::fprintf(stderr, "[操作] %s: %s（ノード %s・id %d）\n", output ? "ここから音を出す" : "このマイクを使う",
                     endpoint->name.c_str(), endpoint->nodeName.c_str(), endpoint->nodeId);
        worker.request(MicCommand::setDefault(output, *endpoint));
    };
    /**
     * パネルのボタンの操作を実行する（押したとき・一覧の中は離したとき）。
     * @param hit 押されたボタン
     */
    const auto handleHit = [&](const PanelHit& hit) {
        const PanelAction action = hit.action;
        switch (action) {
            case PanelAction::None:
            case PanelAction::ShowSettings:      // 画面・重ねた画面の切り替えはパネルの中だけ
            case PanelAction::CloseOverlay:
            case PanelAction::UpdateInstall:     // 1 回目は確認の表示（パネルの中）
            case PanelAction::UpdateConfirmNo:
            case PanelAction::AppInstall:        // まず確認の画面（パネルの中）。開くのは確認の「Konsole で開く」
            case PanelAction::AppsOpenMenu:
            case PanelAction::AppsConfirmCancel:
            case PanelAction::NsVadSlider:       // バーはドラッグのたびに下で送る
            case PanelAction::NsGraceSlider:
            case PanelAction::MixMicSlider:
            case PanelAction::MixToMicSlider:
            case PanelAction::MixToPhonesSlider:
            case PanelAction::MixAdd:              // 候補の画面はパネルの中で開く。開いたらすぐ読み直す
            case PanelAction::MixRefreshApps:
                worker.refreshMixNow();
                break;
            case PanelAction::ShowApps: apps.refreshIfStale(0); break;  // 入っているか・動いているかを見直す
            case PanelAction::AppsOpenList:
                // 一覧を開いたら、古ければサイトから取りに行く（オンのときだけ・1 時間に 1 回まで）
                if (apps.fetchIfDue()) std::fprintf(stderr, "[アプリ] 一覧を開いたので、サイトから取りに行きます\n");
                apps.refreshIfStale(0);
                break;
            case PanelAction::AppsConfirmLaunch: {
                // ほかのアプリを入れる: Konsole でインストーラーを開く（中身は手で打ったときと同じ）
                const frame_apps::LaunchResult result = apps.openInstaller(hit.key, appsLang(config.language));
                panel.setLaunchResult(result);
                std::fprintf(stderr, "[アプリ] Konsole でインストーラーを%s（%s）\n",
                             result == frame_apps::LaunchResult::Started ? "開きました" : "開けませんでした",
                             hit.key.empty() ? "メニュー" : hit.key.c_str());
                break;
            }
            case PanelAction::Quit:
                std::fprintf(stderr, "[VR] パネルの「終了」で終了します\n");
                userQuit = true;
                break;
            case PanelAction::MuteToggle: {
                if (!state.muteKnown) break;
                std::fprintf(stderr, "[操作] %s\n", state.muted ? "ミュートを解除" : "ミュートする");
                MicCommand command {MicCommand::Kind::SetMute};
                command.value = !state.muted;
                worker.request(command);
                break;
            }
            case PanelAction::OpenOutputPicker:
            case PanelAction::OpenMicPicker: worker.refreshDevicesNow(); break;  // 一覧をすぐ読み直す
            case PanelAction::TabQuick:
            case PanelAction::TabFine:
            case PanelAction::TabMix:
                switchTab(action == PanelAction::TabQuick ? PanelTab::Quick
                          : action == PanelAction::TabFine ? PanelTab::Fine
                                                           : PanelTab::Mix);
                break;
            case PanelAction::MixOn:
            case PanelAction::MixOff: {
                const bool on = action == PanelAction::MixOn;
                if (config.mixEnabled == on) break;
                config.mixEnabled = on;
                saveNow();
                MicCommand command {MicCommand::Kind::MixEnable};
                command.value = on;
                command.text = mixChannelBlob(config.mixChannels);
                worker.request(command);
                std::fprintf(stderr, "[ミックス] PC のマイクへ%s\n", on ? "流します" : "流すのをやめます");
                break;
            }
            case PanelAction::MixToMicMute:
            case PanelAction::MixToPhonesMute: {
                const int nodeId = mixNodeIdOf(hit.key);
                if (nodeId < 0) break;
                bool muted = false;
                bool found = false;
                for (const MixChannelLive& channel : state.mix.channels) {
                    if (channel.toMic.nodeName == hit.key && channel.toMic.known) {
                        muted = channel.toMic.muted;
                        found = true;
                    }
                    if (channel.toPhones.nodeName == hit.key && channel.toPhones.known) {
                        muted = channel.toPhones.muted;
                        found = true;
                    }
                }
                bool held = false;
                if (panel.heldMixMute(hit.key, held)) muted = held;
                if (!found && !panel.heldMixMute(hit.key, held)) break;
                const bool next = !muted;
                MicCommand command {MicCommand::Kind::SetMixMute};
                command.nodeId = nodeId;
                command.nodeName = hit.key;
                command.value = next;
                worker.request(command);
                panel.holdMixMute(hit.key, next, nowSeconds() + 4.0);
                std::fprintf(stderr, "[ミックス] %s を%s\n", hit.key.c_str(), next ? "ミュート" : "ミュート解除");
                break;
            }
            case PanelAction::MixSuggest: {
                std::string normalized;
                const MixNameError error = validateChannelName(hit.key, config.mixChannels, "", normalized);
                const UiText& text = uiText(config.language);
                if (error != MixNameError::Ok) {
                    panel.setNameError(mixNameMessage(error, text), nowSeconds() + 4.0);
                    break;
                }
                addMixChannel(normalized);
                break;
            }
            // 動いているアプリを選ぶと、その名前のチャンネルを作り、そのアプリをそこへ向ける
            case PanelAction::MixUseApp: {
                const UiText& text = uiText(config.language);
                const std::string name = channelNameFromApp(hit.key);
                std::string normalized;
                const MixNameError error = name.empty() ? MixNameError::Empty
                                                        : validateChannelName(name, config.mixChannels, "", normalized);
                std::string id;
                if (error == MixNameError::Duplicate) {
                    for (const MixChannel& channel : config.mixChannels) {
                        std::string existing;
                        if (validateChannelName(name, {channel}, "", existing) == MixNameError::Duplicate) id = channel.id;
                    }
                    panel.closeOverlay();
                } else if (error != MixNameError::Ok) {
                    panel.setNameError(mixNameMessage(error, text), nowSeconds() + 4.0);
                    break;
                } else {
                    id = addMixChannel(normalized);
                }
                if (id.empty()) {
                    if (error == MixNameError::Ok) panel.setNameError(text.mixFull, nowSeconds() + 4.0);
                    break;
                }
                rememberPin(hit.key, id);
                std::fprintf(stderr, "[ミックス] 「%s」をチャンネル %s へ向けます\n", hit.key.c_str(), id.c_str());
                break;
            }
            case PanelAction::MixCustom: {
                panel.closeOverlay();
                mixEdit = MixEdit::Add;
                char seed[80];
                std::snprintf(seed, sizeof(seed), uiText(config.language).mixChannelFormat,
                              static_cast<int>(config.mixChannels.size()) + 1);
                showMixKeyboard(seed);
                break;
            }
            case PanelAction::MixRename: {
                mixEdit = MixEdit::Rename;
                mixRenameId = hit.key;
                std::string current;
                for (const MixChannel& channel : config.mixChannels) {
                    if (channel.id == hit.key) current = channel.name;
                }
                showMixKeyboard(current);
                break;
            }
            case PanelAction::MixDelete: {
                const auto it = std::find_if(config.mixChannels.begin(), config.mixChannels.end(),
                                             [&](const MixChannel& channel) { return channel.id == hit.key; });
                if (it == config.mixChannels.end()) break;
                std::fprintf(stderr, "[ミックス] チャンネル %s「%s」を消します\n", it->id.c_str(), it->name.c_str());
                MicCommand command {MicCommand::Kind::RemoveChannel};
                command.nodeName = it->id;
                config.mixPins.erase(std::remove_if(config.mixPins.begin(), config.mixPins.end(),
                                                    [&](const MixPin& pin) { return pin.id == it->id; }),
                                     config.mixPins.end());
                config.mixChannels.erase(it);
                saveNow();
                worker.setMixPins(config.mixPins);
                worker.request(command);
                break;
            }
            case PanelAction::Earphone:
            case PanelAction::Speaker: applyPreset(action); break;
            case PanelAction::EchoOn:
            case PanelAction::EchoOff:
            case PanelAction::NsOn:
            case PanelAction::NsOff: applyToggle(action); break;
            case PanelAction::NsReset:
                std::fprintf(stderr, "[操作] ノイズ除去の強さを標準に戻す\n");
                sendNsParams(kNsVadDefault, kNsGraceDefault, true);
                break;
            case PanelAction::UndoSwitch:
                if (outputSync.undo(config)) saveNow();
                break;
            case PanelAction::UseBuiltinMic: setDefault(false, kBuiltinMicKey); break;
            case PanelAction::OutputView:
                std::fprintf(stderr, "[操作] 出口 %s の設定を表示\n", hit.key.c_str());
                outputSync.dismissNotice();
                break;
            case PanelAction::OutputUse: setDefault(true, hit.key); break;
            case PanelAction::MicUse: setDefault(false, hit.key); break;
            case PanelAction::OutputForget:
                // つながっていない出口だけ忘れられる（今の出口は消さない）
                if (hit.key != outputSync.activeKey() && state.devices.findOutput(hit.key) == nullptr &&
                    config.outputs.erase(hit.key) > 0) {
                    std::fprintf(stderr, "[操作] 出口 %s の設定を忘れました\n", hit.key.c_str());
                    saveNow();
                }
                break;
            case PanelAction::Record:
                if (voice.recording()) {
                    std::fprintf(stderr, "[操作] 録音を止める\n");
                    voice.stopRecording();
                } else {
                    std::fprintf(stderr, "[操作] 録音\n");
                    voice.startRecording(state);
                }
                break;
            case PanelAction::Play: {
                if (hit.index < 0 || hit.index >= static_cast<int>(voiceView.clips.size())) break;
                const uint64_t id = voiceView.clips[hit.index]->id;
                if (voice.playingId() == id) {
                    std::fprintf(stderr, "[操作] 再生を止める\n");
                    voice.stopPlayback();
                } else {
                    std::fprintf(stderr, "[操作] %d 件目を再生\n", hit.index + 1);
                    voice.play(id);
                }
                break;
            }
            case PanelAction::UpdateCheckNow:
                std::fprintf(stderr, "[更新] 確認します\n");
                updater.checkNow();
                break;
            case PanelAction::UpdateConfirmYes:
            case PanelAction::UpdateRetry:
                std::fprintf(stderr, "[更新] 更新を始めます\n");
                if (!updater.install()) std::fprintf(stderr, "[更新] 始められませんでした\n");
                break;
            case PanelAction::UpdateDismiss: updater.dismiss(); break;
            case PanelAction::UpdateCheckOn:
            case PanelAction::UpdateCheckOff: {
                const bool on = action == PanelAction::UpdateCheckOn;
                if (config.updateCheck == on) break;
                config.updateCheck = on;
                std::fprintf(stderr, "[操作] 新しい版の確認 %s\n", on ? "オン" : "オフ");
                saveNow();
                break;
            }
            case PanelAction::AutostartOn:
            case PanelAction::AutostartOff:
                std::fprintf(stderr, "[操作] SteamVR と一緒に起動 %s\n", action == PanelAction::AutostartOn ? "オン" : "オフ");
                worker.request({MicCommand::Kind::SetAutostart, action == PanelAction::AutostartOn});
                break;
            case PanelAction::LanguageMenu: break;  // 開閉はパネルの中
            case PanelAction::LanguagePick: {
                Language language = Language::Ja;
                if (!parseLanguage(hit.key, language) || language == config.language) break;
                config.language = language;
                std::fprintf(stderr, "[操作] 言語: %s\n", languageCode(language));
                saveNow();
                break;
            }
        }
    };
    /**
     * ポインターを離した（パネルから外れた）とき。バーをドラッグしていたら、最後の値を必ず送って保存する。
     * 一覧の中を押してドラッグせずに離したときは、その行・ボタンの操作を実行する。
     * @param leave パネルから外れたなら true
     */
    const auto releasePointer = [&](bool leave) {
        commitDrag(true);
        PanelHit hit;
        if (leave) {
            panel.pointerLeave();
        } else {
            hit = panel.pointerUp();
        }
        handleHit(hit);
    };
    double nextOverlayCheck = nowSeconds() + kOverlayCheckSec;
    /**
     * 自己修復: 自分のダッシュボードのオーバーレイがまだ SteamVR にあるかを確かめ（FindOverlay 1 回）、
     * 消えていれば作り直して、サムネイルとパネルの画像を送り直す。作り直せなければ次の確かめ（3 秒後）で再試行する。
     */
    const auto checkOverlayNow = [&]() {
        nextOverlayCheck = nowSeconds() + kOverlayCheckSec;
        if (vr.ensureOverlay() != OverlayRepair::Repaired) return;
        std::vector<uint8_t> thumbnail;
        renderThumbnail(fonts, kThumbnailSize, thumbnail);
        vr.submitThumbnail(thumbnail.data(), kThumbnailSize);
        drawnUpdateRevision = updater.revision();
        panel.render(config, makeModel());
        vr.submitPanel(panel.toRgba().data());
        vr.logOverlayState("作り直した後");
        dirty = true;
    };
    while (!gStopRequested && !userQuit) {
        if (gShowRequested) {
            gShowRequested = 0;
            std::fprintf(stderr, "[起動] 2 つ目の起動から知らせが来たので、パネルを開きます\n");
            checkOverlayNow();  // 開く前に、オーバーレイが消えていないか確かめる（消えていれば作り直してから開く）
            vr.showPanel();
        }
        // 自己修復: 閉じている間も 3 秒おきに、自分のオーバーレイがまだ SteamVR にあるかを確かめる
        if (nowSeconds() >= nextOverlayCheck) checkOverlayNow();
        const VrEvents events = vr.pollEvents();
        // SteamVR 自体の終了（VREvent_Quit）は終了コード 0。ダッシュボードのアイコンの「閉じる」
        // （VREvent_OverlayClosed）はユーザーの終了なので、「終了」と同じく終了コード 3
        if (events.keyboardDone && mixEdit != MixEdit::None) {
            const MixEdit edit = mixEdit;
            const std::string ignore = edit == MixEdit::Rename ? mixRenameId : std::string();
            std::string normalized;
            const MixNameError error = validateChannelName(events.keyboardText, config.mixChannels, ignore, normalized);
            const UiText& text = uiText(config.language);
            if (error != MixNameError::Ok) {
                panel.setNameError(mixNameMessage(error, text), nowSeconds() + 4.0);
                showMixKeyboard(events.keyboardText);
            } else if (edit == MixEdit::Add) {
                mixEdit = MixEdit::None;
                addMixChannel(normalized);
            } else {
                mixEdit = MixEdit::None;
                for (MixChannel& channel : config.mixChannels) {
                    if (channel.id != mixRenameId || channel.name == normalized) continue;
                    channel.name = normalized;
                    saveNow();
                    MicCommand command {MicCommand::Kind::RenameChannel};
                    command.nodeName = channel.id;
                    command.text = normalized;
                    worker.request(command);
                    std::fprintf(stderr, "[ミックス] %s の名前を「%s」にしました\n", channel.id.c_str(), normalized.c_str());
                }
            }
        } else if (events.keyboardClosed) {
            mixEdit = MixEdit::None;
        }
        if (events.quit) break;
        if (events.closeRequested) {
            std::fprintf(stderr, "[VR] ダッシュボードの「閉じる」で終了します\n");
            userQuit = true;
            break;
        }
        if (!vr.steamVrAlive()) {
            std::fprintf(stderr, "[VR] vrserver がいなくなったので終了します\n");
            break;
        }

        // 新しい版の確認・更新の状態を進める（見えていない間も。GitHub に行くのはスクリプトのキャッシュが切れたときだけ）
        updater.tick(config.updateCheck);
        if (updater.revision() != drawnUpdateRevision) dirty = true;
        // ほかのアプリ: 開いた Konsole が閉じた・systemctl が答えた・一覧を取ってきた（「新しい版の確認」がオフなら取りに行かない）
        apps.setFetchEnabled(config.updateCheck);
        if (apps.tick()) dirty = true;
        if (apps.lastError() != lastAppsError) {
            lastAppsError = apps.lastError();
            if (!lastAppsError.empty()) std::fprintf(stderr, "[アプリ] 一覧: %s\n", lastAppsError.c_str());
        }

        // ワーカーの状態が変わったら写しを取り、出口の設定を合わせる（閉じている間も: 出口が変わったら、その出口の設定をかける）
        if (worker.version() != drawnVersion) {
            drawnVersion = worker.snapshot(state);
            logState(state);
            const std::string activeBefore = outputSync.activeKey();
            if (outputSync.update(state, config, todayText())) saveNow();
            if (outputSync.activeKey() != activeBefore) panel.followActiveOutput();  // 表示も新しい出口へ
            for (const auto& wanted : panel.wantedMixVolumes()) sendMixVolume(wanted.first, wanted.second);
            dirty = true;
        }

        // パネルが見えている間だけ、ワーカーが 1 秒ごとに読み直す（見えていない間は既定の出力・入力を 2 秒おきに見るだけ）
        const bool visible = vr.panelVisible();
        worker.setActive(visible);
        // アプリと更新の画面を見ている間は、ほかのアプリが入っているか・動いているかを 5 秒おきに見直す
        if (visible && panel.view() == PanelView::Apps) apps.refreshIfStale(5);
        // パネルが閉じたら、録音はすぐ止める（見えないところで録らない）。再生も止めて、PipeWire のストリームを片付ける
        if (!visible && wasVisible) {
            if (voice.busy()) std::fprintf(stderr, "[声] パネルが閉じたので、録音・再生を止めます\n");
            voice.shutdown();
            meter.shutdown();
            meterKey.clear();
            // バーをドラッグしたまま閉じたときも、最後の値を送って保存する
            commitDrag(true);
            // 次に開いたときは、マイクの設定の画面・今の出口の表示から（重ねた画面も閉じる）
            panel.resetView();
        }
        if (options.debugRecordOnOpen && visible && !wasVisible) {
            std::fprintf(stderr, "[声] 確認用: パネルが開いたので録音を始めます\n");
            voice.startRecording(state);
        }
        dirty |= voice.update();

        for (const PointerInput& input : events.pointer) {
            switch (input.type) {
                case PointerInput::Type::Move: dirty |= panel.pointerMove(input.x, input.y); break;
                case PointerInput::Type::Down: {
                    const PanelHit hit = panel.pointerDown(input.x, input.y, nowSeconds());
                    if (hit.action == PanelAction::NsVadSlider || hit.action == PanelAction::NsGraceSlider) {
                        // バーを押した直後は、押したところの値を送る（保存は離したとき）
                        double vad = 0.0;
                        double grace = 0.0;
                        panel.displayedNsValues(vad, grace);
                        sendNsParams(vad, grace, false);
                    } else {
                        std::string node;
                        double percent = 0.0;
                        if (panel.mixDrag(node, percent)) sendMixVolume(node, percent);
                    }
                    handleHit(hit);
                    dirty = true;
                    break;
                }
                case PointerInput::Type::Up:
                    releasePointer(false);
                    dirty = true;
                    break;
                case PointerInput::Type::Leave:
                    releasePointer(true);
                    dirty = true;
                    break;
                case PointerInput::Type::Scroll: dirty |= panel.scroll(input.y, input.x); break;
            }
        }
        if (userQuit) break;
        // プリセットの書き込みと読み直しが終わったら、カードの見た目を実際の値に戻す
        if (panel.presetHeld() && worker.completed() >= presetTicket) {
            panel.clearPresetHold();
            dirty = true;
        }
        // バーのドラッグ中は、値が変わっていれば 100ms おきに送る（連打しない）
        if (panel.dragging() && nowSeconds() - lastNsSendAt >= 0.1) {
            std::string node;
            double percent = 0.0;
            if (panel.mixDrag(node, percent)) {
                sendMixVolume(node, percent);
            } else {
                double vad = 0.0;
                double grace = 0.0;
                panel.displayedNsValues(vad, grace);
                sendNsParams(vad, grace, false);
            }
        }
        dirty |= panel.tick(nowSeconds());  // 「もう一度押すと終了」の期限切れ
        // ミックスのタブを開いている間だけ、チャンネルと既定マイクのピークを聴く
        if (visible && config.tab == PanelTab::Mix) {
            std::string key;
            std::vector<MixMeterTarget> targets;
            const AudioEndpoint* input = state.devices.findInput(state.devices.defaultInputKey);
            if (input != nullptr && !input->nodeName.empty()) {
                // マイクだけ受動にしない。話すだけでピークが出る。node.virtual は MixMeter が付ける。
                MixMeterTarget mic;
                mic.key = kDefaultSource;
                mic.object = input->nodeName;
                mic.captureSink = false;
                mic.passive = false;
                targets.push_back(std::move(mic));
                key += input->nodeName;
            }
            key += "|";
            for (const MixChannel& channel : config.mixChannels) {
                const std::string sink = channelSinkNode(channel.id);
                // チャンネルは受動のまま（アプリが出したときだけ。シンクのモニター）。
                targets.push_back({sink, sink, true, true});
                key += sink;
                key += ",";
            }
            if (key != meterKey && meter.setTargets(targets)) meterKey = std::move(key);
        } else if (!meterKey.empty()) {
            meter.setTargets({});
            meterKey.clear();
            panel.setMixPeaks({});
        }
        // 録音中・再生中、またはミックスのタブでは、1 秒に 15 回描き直す
        const bool mixLive = visible && config.tab == PanelTab::Mix;
        if ((voice.busy() || mixLive) && nowSeconds() - lastVoiceFrame >= kVoiceFrameSec) dirty = true;

        // パネルは見えているときだけ、変化があったときだけ描く
        if (visible && (dirty || !wasVisible)) {
            if (mixLive) panel.setMixPeaks(meter.snapshot());
            voiceView = voice.view();
            lastVoiceFrame = nowSeconds();
            drawnUpdateRevision = updater.revision();
            panel.render(config, makeModel());
            vr.submitPanel(panel.toRgba().data());
            dirty = false;
            if (firstSubmit) {
                firstSubmit = false;
                vr.logOverlayState("初めてパネルを描いた後");
            }
        }
        wasVisible = visible;

        // 待つ: パネルが見えている間はポインターに素早く応えるため短く
        sleepInterruptible(visible ? kPanelPollSec : kClosedPollSec);
    }

    // SIGTERM / SIGINT・SteamVR の終了・vrserver の消滅・「終了」のどれでも同じ終了処理を通す
    voice.shutdown();  // 録音・再生を止める（録った音はメモリごと消える）
    meter.shutdown();
    vr.shutdown();
    worker.stop();
    std::fprintf(stderr, "[VR] 終了しました\n");
    if (lockFd >= 0) ::close(lockFd);
    // ユーザーが終了したときは、systemd（Restart=always）に起動し直させないよう決まった終了コードにする
    return userQuit ? kExitCodeUserQuit : 0;
}

}  // namespace

/**
 * エントリーポイント。
 * @param argc 引数の数
 * @param argv 引数
 * @return 終了コード
 */
int main(int argc, char** argv) {
    // journald でも行ごとにすぐ出るようにする
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    installSignalHandlers();

    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return 2;
    }
    switch (options.mode) {
        case Options::Mode::Help: printUsage(); return 0;
        case Options::Mode::Version: std::printf("frame-mic-tuner %s\n", FRAME_MIC_TUNER_VERSION); return 0;
        case Options::Mode::Print: return runPrint(options);
        case Options::Mode::DumpPng: return runDumpPng(options);
        case Options::Mode::Probe: return VrOverlay::probe();
        case Options::Mode::SwitchAway: return VrOverlay::switchAway(options.switchAwaySec);
        case Options::Mode::TestRecord: return runTestRecord(options);
        case Options::Mode::ContrastReport: return printContrastReport();
        case Options::Mode::SelfTest: return runSelfTest();
        case Options::Mode::Overlay: break;
    }
    return runOverlay(options);
}
