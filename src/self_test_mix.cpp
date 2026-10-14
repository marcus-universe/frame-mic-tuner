// --self-test の一部（ミックスの名前・グラフの読み取り・設定ファイル）。
// OpenVR も pw-dump も systemctl も呼ばない。
#include "self_test.h"

#include "config.h"
#include "mix_channels.h"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

void mixSelfTests(const Expect& expect) {
    std::string normalized;
    expect("名前: 前後の空白を取り、連続する空白は 1 つ", normalizeChannelName("  Music \t board  ") == "Music board");
    expect("名前: 空白だけは空", validateChannelName(" \t ", {}, "", normalized) == MixNameError::Empty && normalized.empty());
    expect("名前: 32 文字は通り、33 文字は長すぎ",
           validateChannelName(std::string(32, 'a'), {}, "", normalized) == MixNameError::Ok &&
               validateChannelName(std::string(33, 'b'), {}, "", normalized) == MixNameError::TooLong);
    std::string kana;
    for (int i = 0; i < 32; ++i) kana += "あ";
    expect("名前: 日本語は文字数（32 は通り、33 は長すぎ）",
           utf8Length("音楽") == 2 && utf8Length(kana) == 32 &&
               validateChannelName(kana, {}, "", normalized) == MixNameError::Ok &&
               validateChannelName(kana + "あ", {}, "", normalized) == MixNameError::TooLong);

    const std::vector<MixChannel> channels = {{"ch1", "Music"}};
    expect("名前: 大文字小文字と空白が違っても同じ名前",
           validateChannelName("  music ", channels, "", normalized) == MixNameError::Duplicate);
    expect("名前: 付け直しでは自分の名前は重複にしない",
           validateChannelName("Music", channels, "ch1", normalized) == MixNameError::Ok && normalized == "Music");
    expect("名前: 引用符・バックスラッシュ・波括弧・改行は使えない",
           validateChannelName("a\"b", {}, "", normalized) == MixNameError::BadChar &&
               validateChannelName("a\\b", {}, "", normalized) == MixNameError::BadChar &&
               validateChannelName("a{b}", {}, "", normalized) == MixNameError::BadChar &&
               validateChannelName("a\nb", {}, "", normalized) == MixNameError::BadChar);
    expect("id: 空なら ch1、ch1 の次は ch2", nextChannelId({}) == "ch1" && nextChannelId(channels) == "ch2");
    std::vector<MixChannel> full;
    for (int i = 1; i <= kMixChannelMax; ++i) full.push_back({"ch" + std::to_string(i), "N" + std::to_string(i)});
    expect("id: 8 個あるとこれ以上は空", nextChannelId(full).empty());
    expect("音量: パーセントは 1% 単位で 0〜150 に収める",
           clampMixLinear(50.4) == 0.5 && clampMixLinear(-3) == 0.0 && clampMixLinear(200) == 1.5);
    expect("ノード名: シンク・マイクへ・ヘッドホン",
           channelSinkNode("ch1") == "frame_mix_ch1" && channelToMicNode("ch1") == "frame_mix_ch1_virtual_sink_to_bus" &&
               channelToPhonesNode("ch1") == "frame_mix_ch1_virtual_sink_playback" &&
               channelUnit("ch3") == "frame-mic-mix-channel@ch3.service");

    const std::string dump = R"([
      {"id": 10, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "frame_micmix_source", "media.class": "Audio/Source"}}},
      {"id": 11, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "frame_micmix_mic_virtual_sink", "media.class": "Audio/Sink"}, "params": {"Props": [{"volume": 0.8}]}}},
      {"id": 12, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "frame_mix_ch1", "media.class": "Audio/Sink"}}},
      {"id": 13, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "frame_mix_ch1_virtual_sink_to_bus"}, "params": {"Props": [{"channelVolumes": [0.5]}]}}},
      {"id": 14, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "frame_mix_ch1_virtual_sink_playback"}, "params": {"Props": [{"volume": 1.2}]}}},
      {"id": 15, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "frame_mix_ch1_from_monitor", "media.class": "Stream/Input/Audio"}}},
      {"id": 20, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "app-stream", "media.class": "Stream/Output/Audio", "application.name": "SoundNinja"}}},
      {"id": 30, "type": "PipeWire:Interface:Node", "info": {"props": {"node.name": "vrlink-mic", "media.class": "Stream/Input/Audio", "media.name": "Valve VRLink Microphone Input"}}},
      {"id": 40, "type": "PipeWire:Interface:Link", "info": {"output-node-id": 20, "input-node-id": 12}},
      {"id": 41, "type": "PipeWire:Interface:Link", "info": {"output-node-id": 10, "input-node-id": 30}}
    ])";
    const MixGraph graph = parseMixDump(dump, {{"ch1", "Music"}});
    const MixChannelLive* live = graph.channels.empty() ? nullptr : &graph.channels.front();
    const auto near = [](double a, double b) { return std::abs(a - b) < 0.001; };
    expect("pw-dump: コア・マイク音量・VRLink の接続を読む",
           graph.known && graph.coreRunning && graph.mic.known && graph.mic.nodeId == 11 && near(graph.mic.linear, 0.8) &&
               graph.vrlinkPresent && graph.routed);
    expect("pw-dump: チャンネルの 2 つの音量と、再生中のアプリ（自分のループバックは数えない）",
           live != nullptr && live->running && live->apps.size() == 1 && live->apps[0] == "SoundNinja" &&
               live->toMic.known && near(live->toMic.linear, 0.5) && near(live->toPhones.linear, 1.2) &&
               graph.channels.size() == 1);
    expect("pw-dump: 壊れた JSON は読めない", !parseMixDump("{", {}).known);

    char path[] = "/tmp/frame-mic-tuner-selftest-mix-XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd >= 0) ::close(fd);
    Config saved;
    saved.language = Language::En;
    saved.tab = PanelTab::Mix;
    saved.mixEnabled = true;
    saved.mixChannels = {{"ch2", "Sound board"}, {"ch1", "音楽"}};
    std::string error;
    std::vector<std::string> warnings;
    Config loaded;
    const bool roundTrip = fd >= 0 && saveConfig(path, saved, error) && loadConfig(path, loaded, warnings, error);
    expect("設定ファイル: ミックスのオンとチャンネル（空白・日本語）を保存して読み直すと同じ",
           roundTrip && warnings.empty() && loaded.tab == PanelTab::Mix && loaded.mixEnabled &&
               loaded.mixChannels.size() == 2 && loaded.mixChannels[0].id == "ch2" &&
               loaded.mixChannels[0].name == "Sound board" && loaded.mixChannels[1].name == "音楽");
    if (fd >= 0) ::unlink(path);

    const std::string bad =
        "{\"mix_enabled\": true, \"mix_channels\": ["
        "{\"id\": \"ch1\", \"name\": \"Music\"},"
        "{\"id\": \"ch9\", \"name\": \"Nope\"},"
        "{\"id\": \"ch2\", \"name\": \" music \"},"
        "{\"id\": \"ch3\", \"name\": \"\"}"
        "]}";
    char badPath[] = "/tmp/frame-mic-tuner-selftest-mixbad-XXXXXX";
    const int badFd = ::mkstemp(badPath);
    bool skipped = false;
    if (badFd >= 0) {
        const ssize_t wrote = ::write(badFd, bad.data(), bad.size());
        ::close(badFd);
        Config badLoaded;
        std::vector<std::string> badWarnings;
        skipped = wrote == static_cast<ssize_t>(bad.size()) && loadConfig(badPath, badLoaded, badWarnings, error) &&
                  badLoaded.mixChannels.size() == 1 && badLoaded.mixChannels[0].id == "ch1" && badWarnings.size() >= 3;
        ::unlink(badPath);
    }
    expect("設定ファイル: 範囲外の id・重複・空の名前は読み飛ばす", skipped);
}
