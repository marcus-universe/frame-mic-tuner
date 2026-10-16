// ミックスのピーク測定。声のチェックとは別のスレッド（録音の node.virtual 無しとは混ぜない）。
#include "mix_meter.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <pthread.h>
#include <signal.h>

#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

/** サンプルの絶対値の減衰（dB/秒）。無音になるとバーが空まで戻る。 */
constexpr float kDecayDbPerSec = 24.0f;
constexpr uint32_t kRate = 48000;

/**
 * PipeWire のライブラリを 1 回だけ初期化する（pw_init は重ねて呼んでもよい）。
 */
void initPipeWireOnce() {
    static bool done = false;
    if (done) return;
    pw_init(nullptr, nullptr);
    done = true;
}

/**
 * ノード名に使えない文字を落とす。
 * @param object PipeWire のノード名
 * @return ストリームの node.name
 */
std::string meterNodeName(const std::string& object) {
    std::string name = "fmt-level-" + object;
    for (char& c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) c = '-';
    }
    return name;
}

}  // namespace

struct MixMeterTap {
    std::string key;
    std::string object;
    bool captureSink = false;
    bool passive = true;
    pw_stream* stream = nullptr;
    std::atomic<float> peak {0.0f};
    bool logged = false;
};

namespace {

const pw_stream_events& meterEvents() {
    static const pw_stream_events events = [] {
        pw_stream_events e {};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = [](void* data, pw_stream_state, pw_stream_state state, const char* error) {
            if (state != PW_STREAM_STATE_ERROR) return;
            auto* tap = static_cast<MixMeterTap*>(data);
            if (tap->logged) return;
            tap->logged = true;
            std::fprintf(stderr, "[ミックス] レベルのストリームが失敗: %s (%s)\n", tap->object.c_str(),
                         error != nullptr ? error : "?");
        };
        e.process = [](void* data) {
            auto* tap = static_cast<MixMeterTap*>(data);
            if (tap->stream == nullptr) return;
            pw_buffer* buffer = pw_stream_dequeue_buffer(tap->stream);
            if (buffer == nullptr) return;
            float instant = 0.0f;
            uint32_t samples = 0;
            const spa_data& d = buffer->buffer->datas[0];
            if (d.data != nullptr && d.chunk != nullptr) {
                const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
                const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
                const auto* pcm = reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(d.data) + offset);
                const size_t count = size / sizeof(int16_t);
                samples = static_cast<uint32_t>(count / 2);
                for (size_t i = 0; i < count; ++i) {
                    instant = std::max(instant, std::abs(static_cast<int>(pcm[i])) / 32768.0f);
                }
            }
            pw_stream_queue_buffer(tap->stream, buffer);
            const float dt = samples > 0 ? static_cast<float>(samples) / static_cast<float>(kRate) : 0.0f;
            const float decay = std::pow(10.0f, -kDecayDbPerSec / 20.0f * dt);
            const float held = tap->peak.load(std::memory_order_relaxed);
            tap->peak.store(std::max(instant, held * decay), std::memory_order_relaxed);
        };
        return e;
    }();
    return events;
}

/**
 * ループをロックした状態で、1 本のキャプチャをつなぐ。
 * node.virtual は残す（tracker がフィルターを入れない）。マイクは passive にしない。
 * 受動だと、ほかにキャプチャが無いときソースが止まっていてピークが出ない。
 * @param loop ロック中のループ
 * @param tap userdata（stream を書き戻す）
 * @return つながったら true
 */
bool connectTap(pw_thread_loop* loop, MixMeterTap* tap) {
    const std::string nodeName = meterNodeName(tap->object);
    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE,
                                             "DSP", PW_KEY_APP_NAME, "Frame Mic Tuner", PW_KEY_NODE_DESCRIPTION,
                                             "Frame Mic Tuner level", PW_KEY_NODE_NAME, nodeName.c_str(), "node.virtual", "true",
                                             "target.object", tap->object.c_str(), nullptr);
    if (tap->passive) pw_properties_set(props, "node.passive", "true");
    if (tap->captureSink) pw_properties_set(props, "stream.capture.sink", "true");
    pw_stream* stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), nodeName.c_str(), props, &meterEvents(), tap);
    if (stream == nullptr) return false;
    tap->stream = stream;

    uint8_t podBuffer[1024];
    spa_pod_builder builder;
    spa_pod_builder_init(&builder, podBuffer, sizeof(podBuffer));
    spa_audio_info_raw info {};
    info.format = SPA_AUDIO_FORMAT_S16;
    info.rate = kRate;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[1] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
    if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0) {
        std::fprintf(stderr, "[ミックス] レベルをつなげません: %s\n", tap->object.c_str());
        pw_stream_destroy(stream);
        tap->stream = nullptr;
        return false;
    }
    std::fprintf(stderr, "[ミックス] レベルを聴き始めました: %s\n", tap->object.c_str());
    return true;
}

}  // namespace

MixMeter::MixMeter() = default;

MixMeter::~MixMeter() {
    shutdown();
}

bool MixMeter::ensureLoop() {
    if (loop_ != nullptr) return true;
    initPipeWireOnce();
    loop_ = pw_thread_loop_new("frame-mic-tuner-meter", nullptr);
    if (loop_ == nullptr) {
        std::fprintf(stderr, "[ミックス] レベルのスレッドを作れません\n");
        return false;
    }
    // 終了のシグナルはメインスレッドのままにする
    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    const int started = pw_thread_loop_start(loop_);
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (started != 0) {
        std::fprintf(stderr, "[ミックス] レベルのスレッドを始められません\n");
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
        return false;
    }
    return true;
}

bool MixMeter::setTargets(std::vector<MixMeterTarget> targets) {
    targets.erase(std::remove_if(targets.begin(), targets.end(),
                                 [](const MixMeterTarget& t) { return t.key.empty() || t.object.empty(); }),
                  targets.end());
    bool same = targets.size() == taps_.size();
    if (same) {
        for (size_t i = 0; i < targets.size(); ++i) {
            if (targets[i].key != taps_[i]->key || targets[i].object != taps_[i]->object ||
                targets[i].captureSink != taps_[i]->captureSink || targets[i].passive != taps_[i]->passive) {
                same = false;
                break;
            }
        }
    }
    if (same) return true;
    if (targets.empty()) {
        shutdown();
        return true;
    }
    if (!ensureLoop()) return false;

    pw_thread_loop_lock(loop_);
    for (auto it = taps_.begin(); it != taps_.end();) {
        const bool keep = std::any_of(targets.begin(), targets.end(), [&](const MixMeterTarget& t) {
            return t.key == (*it)->key && t.object == (*it)->object && t.captureSink == (*it)->captureSink &&
                   t.passive == (*it)->passive;
        });
        if (keep) {
            ++it;
            continue;
        }
        if ((*it)->stream != nullptr) pw_stream_destroy((*it)->stream);
        it = taps_.erase(it);
    }
    for (const MixMeterTarget& target : targets) {
        const bool have = std::any_of(taps_.begin(), taps_.end(), [&](const std::unique_ptr<MixMeterTap>& tap) {
            return tap->key == target.key && tap->object == target.object && tap->captureSink == target.captureSink &&
                   tap->passive == target.passive;
        });
        if (have) continue;
        auto tap = std::make_unique<MixMeterTap>();
        tap->key = target.key;
        tap->object = target.object;
        tap->captureSink = target.captureSink;
        tap->passive = target.passive;
        if (!connectTap(loop_, tap.get())) continue;
        taps_.push_back(std::move(tap));
    }
    pw_thread_loop_unlock(loop_);
    return true;
}

std::vector<std::pair<std::string, float>> MixMeter::snapshot() const {
    std::vector<std::pair<std::string, float>> out;
    out.reserve(taps_.size());
    for (const std::unique_ptr<MixMeterTap>& tap : taps_) {
        out.emplace_back(tap->key, tap->peak.load(std::memory_order_relaxed));
    }
    return out;
}

void MixMeter::shutdown() {
    if (loop_ == nullptr) return;
    pw_thread_loop_lock(loop_);
    for (std::unique_ptr<MixMeterTap>& tap : taps_) {
        if (tap->stream != nullptr) pw_stream_destroy(tap->stream);
    }
    taps_.clear();
    pw_thread_loop_unlock(loop_);
    pw_thread_loop_stop(loop_);
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
}
