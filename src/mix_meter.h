// ミックスのタブを開いている間だけ、チャンネルと既定のマイクのピークを測る。
// 自分の PipeWire ループでキャプチャを張る（PipeWire・WirePlumber は再起動しない）。
// node.virtual は付けるので、マイクのフィルターはこれではオンにならない。
// チャンネルは受動（音が来たときだけ）。マイクは受動にしない（誰も使っていなくても話すとピークが出る）。
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

struct pw_thread_loop;
struct MixMeterTap;

/** 測る対象。key はパネルが引く名前、object は PipeWire のノード名。 */
struct MixMeterTarget {
    std::string key;
    std::string object;     ///< target.object に書くノード名
    bool captureSink = false;  ///< シンクのモニターを聴く（stream.capture.sink）
    bool passive = true;       ///< false のマイクだけ、ほかにキャプチャが無くてもソースを起こす
};

/**
 * チャンネルのシンクと既定のマイクのピーク（0〜1、減衰つき）。
 * ミックスのタブを閉じたら setTargets({}) か shutdown() でストリームを外す。
 */
class MixMeter {
public:
    MixMeter();
    ~MixMeter();
    MixMeter(const MixMeter&) = delete;
    MixMeter& operator=(const MixMeter&) = delete;

    /**
     * 聴く対象を揃える。同じ組なら何もしない。空ならストリームとスレッドを片付ける。
     * @param targets チャンネルのシンクと、あれば既定のマイク
     * @return 揃えた、または空にして止めたら true。スレッドを作れなければ false
     */
    bool setTargets(std::vector<MixMeterTarget> targets);

    /** @return いまのピーク（key と、減衰したあとの線形 0〜1） */
    std::vector<std::pair<std::string, float>> snapshot() const;

    /** ストリームとスレッドを止める。もう一度 setTargets すれば作り直す。 */
    void shutdown();

private:
    bool ensureLoop();

    pw_thread_loop* loop_ = nullptr;
    std::vector<std::unique_ptr<MixMeterTap>> taps_;
};
