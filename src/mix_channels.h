// ミックスのチャンネル（名前・個数・PipeWire のノード名）と、名前の検査。
// グラフ自体は contrib/pipewire/frame-mic-mix-run.sh が pw-loopback で作る。ここはそれを起動・停止し、pw-dump を読む。
#pragma once

#include <cmath>
#include <string>
#include <vector>

/** チャンネルは ch1〜ch8 まで。 */
constexpr int kMixChannelMax = 8;
/** 表示名の最大文字数（UTF-8 の文字数。バイト数ではない）。 */
constexpr int kMixNameMaxChars = 32;
/** スライダーの範囲（線形。1.0 = 100%、1.5 = 150%）。 */
constexpr double kMixVolumeMin = 0.0;
constexpr double kMixVolumeMax = 1.5;
constexpr double kMixDefaultMic = 1.0;
constexpr double kMixDefaultToMic = 0.5;
constexpr double kMixDefaultPhones = 1.0;

constexpr const char* kMixCoreService = "frame-mic-mix.service";
constexpr const char* kMixSettingKey = "frame-mic.mix";
constexpr const char* kMixSourceNode = "frame_micmix_source";
constexpr const char* kMixBusNode = "frame_micmix_sink";
constexpr const char* kMixMicLevelNode = "frame_micmix_mic_virtual_sink";
/** VRLink の取り込みストリームの media.name。 */
constexpr const char* kVrlinkMicName = "Valve VRLink Microphone Input";

/** 設定ファイルに覚えるチャンネル（id は安定、name は表示名）。 */
struct MixChannel {
    std::string id;
    std::string name;

    /** @return 同じ id と名前なら true */
    bool operator==(const MixChannel& other) const { return id == other.id && name == other.name; }
};

/** 名前を断る理由。 */
enum class MixNameError {
    Ok,
    Empty,
    TooLong,
    Duplicate,
    BadChar,  ///< PipeWire の説明文に使えない文字（" \ { } や改行）
};

/** pw-dump から読んだ、音量を持つノード。 */
struct MixNodeVolume {
    bool known = false;
    int nodeId = -1;
    double linear = 1.0;  ///< 0〜1.5
    std::string nodeName;

    /** @return 画面に出すパーセント（0〜150） */
    double percent() const { return linear * 100.0; }
};

/** 動いているチャンネル 1 つ。 */
struct MixChannelLive {
    std::string id;
    bool running = false;
    MixNodeVolume toMic;
    MixNodeVolume toPhones;
    std::vector<std::string> apps;  ///< いまこのチャンネルに出しているアプリ（表示用）

    /** @return 同じなら true */
    bool operator==(const MixChannelLive& other) const {
        return id == other.id && running == other.running && toMic.known == other.toMic.known &&
               toMic.nodeId == other.toMic.nodeId && apps == other.apps &&
               (toMic.known == other.toMic.known) && std::abs(toMic.linear - other.toMic.linear) < 0.02 &&
               toPhones.nodeId == other.toPhones.nodeId && std::abs(toPhones.linear - other.toPhones.linear) < 0.02;
    }
};

/** グラフの今（pw-dump と frame-mic.mix の設定）。 */
struct MixGraph {
    bool known = false;          ///< pw-dump を読めたか
    bool settingKnown = false;   ///< frame-mic.mix が WirePlumber の設定にあるか
    bool enabled = false;        ///< frame-mic.mix（PC のマイクへ流すか）
    bool coreRunning = false;    ///< frame_micmix_source があるか
    bool vrlinkPresent = false;  ///< VRLink のマイクストリームがあるか
    bool routed = false;         ///< そのストリームが frame_micmix_source から取っているか
    MixNodeVolume mic;
    std::vector<MixChannelLive> channels;
};

/**
 * 前後の空白を取り、連続する空白を 1 つにする。
 * @param raw 入力
 * @return 整えた名前（空なら空）
 */
std::string normalizeChannelName(const std::string& raw);

/**
 * UTF-8 の文字数（壊れたバイトは 1 文字として数える）。
 * @param text 文字列
 * @return 文字数
 */
int utf8Length(const std::string& text);

/**
 * 名前を検査する（空・長すぎ・重複・使えない文字）。比較は ASCII の大文字小文字を無視する。
 * @param raw 入力
 * @param channels すでにあるチャンネル
 * @param ignoreId 名前を変えるチャンネル自身（重複に数えない）。空なら誰も除外しない
 * @param normalized 通ったときの整えた名前
 * @return 結果
 */
MixNameError validateChannelName(const std::string& raw, const std::vector<MixChannel>& channels,
                                 const std::string& ignoreId, std::string& normalized);

/**
 * まだ使っていない id（ch1〜ch8）を返す。
 * @param channels すでにあるチャンネル
 * @return id。8 個とも使っていれば空
 */
std::string nextChannelId(const std::vector<MixChannel>& channels);

/** @return チャンネルのシンク（アプリが選ぶ出力）のノード名 */
std::string channelSinkNode(const std::string& id);
/** @return チャンネルをマイクへ混ぜるノード名 */
std::string channelToMicNode(const std::string& id);
/** @return チャンネルをヘッドホンへ出すノード名 */
std::string channelToPhonesNode(const std::string& id);
/** @return systemd のインスタンス名（frame-mic-mix-channel@ch1.service） */
std::string channelUnit(const std::string& id);

/**
 * パーセント（0〜150）を線形の音量（0〜1.5）にして 1% 単位に丸める。
 * @param percent パーセント
 * @return 線形
 */
double clampMixLinear(double percent);

/**
 * 名前ファイルのフォルダ（~/.config/pipewire/frame-mic-mix）。
 * @return パス
 */
std::string mixConfigDir();

/**
 * チャンネルの表示名を 1 行で書く（サービスがこれを読む）。
 * @param id ch1〜ch8
 * @param name 整えた名前
 * @param error 失敗したとき
 * @return 書けたら true
 */
bool writeChannelNameFile(const std::string& id, const std::string& name, std::string& error);

/**
 * チャンネルの名前ファイルを消す。
 * @param id ch1〜ch8
 */
void removeChannelNameFile(const std::string& id);

/**
 * コア（マイクとバス）を起動または停止する。
 * @param on 起動するなら true
 * @return systemctl が成功したら true
 */
bool setMixCoreRunning(bool on);

/**
 * チャンネルのサービスを起動する（名前ファイルは先に書いておく）。
 * @param id ch1〜ch8
 * @return 起動できたら true
 */
bool startMixChannel(const std::string& id);

/**
 * チャンネルのサービスを止めて無効にする。
 * @param id ch1〜ch8
 * @return 命令が届いたら true（元から止まっていても true）
 */
bool stopMixChannel(const std::string& id);

/**
 * チャンネルのサービスを再起動する（名前を変えたあと。ほかのチャンネルは止まらない）。
 * @param id ch1〜ch8
 * @return 再起動できたら true
 */
bool restartMixChannel(const std::string& id);

/**
 * ノードの音量を決める（wpctl set-volume）。
 * @param nodeId ノードの id
 * @param linear 0〜1.5
 * @return 書けたら true
 */
bool writeMixVolume(int nodeId, double linear);

/**
 * PC へ流す設定（frame-mic.mix）を書いて保存する。
 * @param on 流すなら true
 * @return 書けたら true
 */
bool writeMixEnabled(bool on);

/**
 * ノードが出るまで少し待つ（追加した直後）。
 * @param nodeName ノード名
 * @param nodeId 見つかった id
 * @return 見つかったら true
 */
bool waitForMixNode(const std::string& nodeName, int& nodeId);

/**
 * pw-dump の JSON からミックスのグラフを読む（外部コマンドは呼ばない）。
 * @param text pw-dump の出力
 * @param channels 設定にあるチャンネル（動いていなくても行に出す）
 * @return グラフ
 */
MixGraph parseMixDump(const std::string& text, const std::vector<MixChannel>& channels);

/**
 * pw-dump を実行してグラフを読む。失敗しても setting 以外は空。
 * @param channels 設定にあるチャンネル
 * @return グラフ
 */
MixGraph readMixGraph(const std::vector<MixChannel>& channels);

/**
 * 画面の更新が要る差分か。
 * @param a グラフ
 * @param b グラフ
 * @return 同じなら true
 */
bool sameMixGraph(const MixGraph& a, const MixGraph& b);
