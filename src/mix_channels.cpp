// ミックスのチャンネルの実装。
#include "mix_channels.h"

#include "command.h"
#include "json.h"
#include "mic_state.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

bool makeDirectories(const std::string& dir) {
    for (size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos != dir.size() && dir[pos] != '/') continue;
        const std::string part = dir.substr(0, pos);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool validId(const std::string& id) {
    return id.size() == 3 && id[0] == 'c' && id[1] == 'h' && id[2] >= '1' && id[2] <= '8';
}

std::string foldAscii(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool systemctl(const std::vector<std::string>& args) {
    std::vector<std::string> argv = {"systemctl", "--user"};
    argv.insert(argv.end(), args.begin(), args.end());
    const CommandResult result = runCommand(argv, 15000);
    std::fprintf(stderr, "[ミックス] %s\n", describeCommand(argv, result).c_str());
    return result.ok();
}

const JsonValue* prop(const JsonValue& object, const char* key) {
    const JsonValue* info = object.get("info");
    const JsonValue* props = info != nullptr ? info->get("props") : nullptr;
    return props != nullptr ? props->get(key) : nullptr;
}

std::string propText(const JsonValue& object, const char* key) {
    const JsonValue* value = prop(object, key);
    return value != nullptr && value->isString() ? value->text : std::string();
}

double volumeOf(const JsonValue& object) {
    const JsonValue* info = object.get("info");
    const JsonValue* params = info != nullptr ? info->get("params") : nullptr;
    const JsonValue* props = params != nullptr ? params->get("Props") : nullptr;
    if (props == nullptr || props->type != JsonValue::Type::Array) return 1.0;
    // wpctl set-volume は線形の値を受け、channelVolumes にはその三乗を書く。volume は 1.0 のまま。
    // 表示は wpctl get-volume と同じく立方根（83% を 57% と読まない）。
    for (const JsonValue& entry : props->items) {
        if (const JsonValue* channels = entry.get("channelVolumes");
            channels != nullptr && channels->type == JsonValue::Type::Array && !channels->items.empty() &&
            channels->items[0].isNumber()) {
            return std::cbrt(std::max(0.0, channels->items[0].number));
        }
    }
    for (const JsonValue& entry : props->items) {
        if (const JsonValue* volume = entry.get("volume"); volume != nullptr && volume->isNumber()) return volume->number;
    }
    return 1.0;
}

bool muteOf(const JsonValue& object) {
    const JsonValue* info = object.get("info");
    const JsonValue* params = info != nullptr ? info->get("params") : nullptr;
    const JsonValue* props = params != nullptr ? params->get("Props") : nullptr;
    if (props == nullptr || props->type != JsonValue::Type::Array) return false;
    // mute が一度 false に戻っても、softMute が立っていれば音は止まっている。どちらかでミュート。
    bool muted = false;
    for (const JsonValue& entry : props->items) {
        for (const char* key : {"mute", "softMute"}) {
            if (const JsonValue* mute = entry.get(key); mute != nullptr && mute->isBool() && mute->boolean) muted = true;
        }
    }
    return muted;
}

int infoInt(const JsonValue& object, const char* key) {
    const JsonValue* info = object.get("info");
    const JsonValue* value = info != nullptr ? info->get(key) : nullptr;
    if (value != nullptr && value->isNumber()) return static_cast<int>(value->number);
    const JsonValue* fromProps = prop(object, key);
    if (fromProps != nullptr && fromProps->isNumber()) return static_cast<int>(fromProps->number);
    return -1;
}

bool isVrlink(const JsonValue& object) {
    const std::string media = propText(object, "media.name");
    const std::string app = propText(object, "application.name");
    const std::string binary = propText(object, "application.process.binary");
    return media == kVrlinkMicName || app == "vrlink" || binary == "vrlink";
}

std::string appLabel(const JsonValue& object) {
    std::string name = propText(object, "application.name");
    if (name.empty()) name = propText(object, "media.name");
    if (name.empty()) name = propText(object, "node.name");
    const std::string suffix = ".spatialize_filter_chain";
    const size_t at = name.find(suffix);
    if (at != std::string::npos) name = name.substr(0, at);
    return name;
}

/** ASCII の大文字を小文字にする（ピンの名前合わせ）。 */
std::string asciiFold(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/** パスのファイル名。「 (deleted)」は外す。 */
std::string baseName(std::string path) {
    const std::string deleted = " (deleted)";
    if (path.size() >= deleted.size() && path.compare(path.size() - deleted.size(), deleted.size(), deleted) == 0) {
        path.resize(path.size() - deleted.size());
    }
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

bool isListedRunningApp(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] == '.' || name[0] == '[') return false;
    const std::string fold = asciiFold(name);
    static const char* blocked[] = {"pipewire", "pipewire-pulse", "wireplumber", "systemd", "dbus", "dbus-daemon",
                                    "dbus-broker", "dbus-broker-launch", "gamescope", "steam", "vrserver", "vrcompositor",
                                    "vrmonitor", "pressure-vessel", "pressure-vessel-adverb", "frame-mic-tuner", "bwrap",
                                    "bash", "sh", "sleep", "cat", "sshd", "rtkit-daemon"};
    for (const char* item : blocked) {
        if (fold == item) return false;
    }
    if (fold.rfind("xdg-", 0) == 0 || fold.rfind("pw-", 0) == 0 || fold.rfind("systemd", 0) == 0 ||
        fold.rfind("steam", 0) == 0 || fold.rfind("gamescope", 0) == 0 || fold.rfind("dbus", 0) == 0) {
        return false;
    }
    return fold.find("pressure-vessel") == std::string::npos;
}

bool mixAppMatches(const std::string& pinApp, const std::string& streamApp, const std::string& binary) {
    if (pinApp.empty()) return false;
    const std::string pin = asciiFold(pinApp);
    if (!streamApp.empty() && pin == asciiFold(streamApp)) return true;
    return !binary.empty() && pin == asciiFold(binary);
}

namespace {

/**
 * /proc から、自分のユーザーで動いているアプリの名前を集める。
 * 音を出していなくても出す。システムとこのアプリ自身は入れない。
 */
std::vector<std::string> readRunningApps() {
    std::vector<std::string> apps;
    DIR* dir = ::opendir("/proc");
    if (dir == nullptr) return apps;
    const uid_t me = ::getuid();
    while (const dirent* ent = ::readdir(dir)) {
        if (ent->d_name[0] < '1' || ent->d_name[0] > '9') continue;
        bool numeric = true;
        for (const char* p = ent->d_name; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') numeric = false;
        }
        if (!numeric) continue;
        const std::string proc = std::string("/proc/") + ent->d_name;
        struct stat st {};
        if (::stat(proc.c_str(), &st) != 0 || st.st_uid != me) continue;
        std::ifstream commFile(proc + "/comm");
        std::string comm;
        std::getline(commFile, comm);
        while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r')) comm.pop_back();
        char buf[512];
        const ssize_t n = ::readlink((proc + "/exe").c_str(), buf, sizeof(buf) - 1);
        std::string exe;
        if (n > 0) {
            buf[n] = '\0';
            exe = baseName(buf);
        }
        const std::string shown = isListedRunningApp(exe) ? exe : comm;
        if (!isListedRunningApp(shown)) continue;
        bool seen = false;
        for (const std::string& have : apps) {
            if (mixAppMatches(shown, have, have)) seen = true;
        }
        if (!seen) apps.push_back(shown);
        if (apps.size() >= 40) break;
    }
    ::closedir(dir);
    std::sort(apps.begin(), apps.end(), [](const std::string& a, const std::string& b) { return asciiFold(a) < asciiFold(b); });
    return apps;
}

}  // namespace

std::string utf8Prefix(const std::string& text, int maxChars) {
    int count = 0;
    size_t i = 0;
    while (i < text.size() && count < maxChars) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > text.size()) len = 1;
        i += len;
        ++count;
    }
    return text.substr(0, i);
}

std::string channelNameFromApp(const std::string& raw) {
    std::string cleaned;
    cleaned.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c == '"' || c == '\\' || c == '{' || c == '}' || c == '\r' || c == '\n') {
            cleaned += ' ';
        } else {
            cleaned += static_cast<char>(c);
        }
    }
    return utf8Prefix(normalizeChannelName(cleaned), kMixNameMaxChars);
}

std::string normalizeChannelName(const std::string& raw) {
    std::string out;
    bool space = false;
    bool started = false;
    for (unsigned char c : raw) {
        if (c == ' ' || c == '\t') {
            if (started) space = true;
            continue;
        }
        if (space) out += ' ';
        space = false;
        started = true;
        out += static_cast<char>(c);
    }
    return out;
}

int utf8Length(const std::string& text) {
    int count = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((c & 0x80) == 0) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        }
        if (i + len > text.size()) len = 1;
        i += len;
        ++count;
    }
    return count;
}

MixNameError validateChannelName(const std::string& raw, const std::vector<MixChannel>& channels,
                                 const std::string& ignoreId, std::string& normalized) {
    normalized = normalizeChannelName(raw);
    if (normalized.empty()) return MixNameError::Empty;
    if (utf8Length(normalized) > kMixNameMaxChars) return MixNameError::TooLong;
    if (normalized.find_first_of("\"\\{}\r\n") != std::string::npos) return MixNameError::BadChar;
    const std::string folded = foldAscii(normalized);
    for (const MixChannel& channel : channels) {
        if (channel.id == ignoreId) continue;
        if (foldAscii(normalizeChannelName(channel.name)) == folded) return MixNameError::Duplicate;
    }
    return MixNameError::Ok;
}

std::string nextChannelId(const std::vector<MixChannel>& channels) {
    for (int n = 1; n <= kMixChannelMax; ++n) {
        const std::string id = "ch" + std::to_string(n);
        const bool used = std::any_of(channels.begin(), channels.end(), [&](const MixChannel& c) { return c.id == id; });
        if (!used) return id;
    }
    return "";
}

std::string channelSinkNode(const std::string& id) {
    return "frame_mix_" + id;
}

std::string channelToMicNode(const std::string& id) {
    return "frame_mix_" + id + "_virtual_sink_to_bus";
}

std::string channelToPhonesNode(const std::string& id) {
    return "frame_mix_" + id + "_virtual_sink_playback";
}

std::string channelUnit(const std::string& id) {
    return "frame-mic-mix-channel@" + id + ".service";
}

double clampMixLinear(double percent) {
    const double rounded = std::round(std::clamp(percent, 0.0, 150.0));
    return rounded / 100.0;
}

std::string mixConfigDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') return std::string(xdg) + "/pipewire/frame-mic-mix";
    const char* home = std::getenv("HOME");
    return std::string(home != nullptr ? home : ".") + "/.config/pipewire/frame-mic-mix";
}

bool writeChannelNameFile(const std::string& id, const std::string& name, std::string& error) {
    if (!validId(id)) {
        error = "bad channel id";
        return false;
    }
    const std::string dir = mixConfigDir();
    if (!makeDirectories(dir)) {
        error = "mkdir failed";
        return false;
    }
    const std::string path = dir + "/" + id + ".name";
    const std::string temp = path + ".tmp";
    {
        std::FILE* file = std::fopen(temp.c_str(), "wb");
        if (file == nullptr) {
            error = "open failed";
            return false;
        }
        const std::string line = name + "\n";
        const bool ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
        std::fclose(file);
        if (!ok) {
            ::unlink(temp.c_str());
            error = "write failed";
            return false;
        }
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        ::unlink(temp.c_str());
        error = "rename failed";
        return false;
    }
    return true;
}

void removeChannelNameFile(const std::string& id) {
    if (!validId(id)) return;
    ::unlink((mixConfigDir() + "/" + id + ".name").c_str());
}

bool setMixCoreRunning(bool on) {
    if (on) return systemctl({"enable", "--now", kMixCoreService});
    return systemctl({"disable", "--now", kMixCoreService});
}

bool startMixChannel(const std::string& id) {
    if (!validId(id)) return false;
    return systemctl({"enable", "--now", channelUnit(id)});
}

bool stopMixChannel(const std::string& id) {
    if (!validId(id)) return false;
    return systemctl({"disable", "--now", channelUnit(id)});
}

bool restartMixChannel(const std::string& id) {
    if (!validId(id)) return false;
    return systemctl({"restart", channelUnit(id)});
}

bool writeNodeVolume(int nodeId, double linear) {
    if (nodeId < 0) return false;
    linear = std::clamp(linear, 0.0, 4.0);
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", linear);
    const std::vector<std::string> argv = {"wpctl", "set-volume", std::to_string(nodeId), text};
    const CommandResult result = runCommand(argv);
    std::fprintf(stderr, "[ミックス] %s\n", describeCommand(argv, result).c_str());
    return result.ok();
}

bool writeMixVolume(int nodeId, double linear) {
    return writeNodeVolume(nodeId, std::clamp(linear, kMixVolumeMin, kMixVolumeMax));
}

bool writeMixMute(int nodeId, bool muted) {
    if (nodeId < 0) return false;
    const std::vector<std::string> argv = {"wpctl", "set-mute", std::to_string(nodeId), muted ? "1" : "0"};
    const CommandResult result = runCommand(argv);
    std::fprintf(stderr, "[ミックス] %s\n", describeCommand(argv, result).c_str());
    return result.ok();
}

void maintainVrlinkMakeup(MixGraph& graph) {
    if (graph.vrlinkNodeId < 0) return;
    // 音声チェックは同じソースを PCM でヘッドセットに出す。無線マイクだけ小さいので、このストリームを 2 倍にする。
    // ミックスを外したら 1 に戻す（ふつうのマイクまで大きくしない）。
    const double want = graph.routed ? kVrlinkMakeupLinear : 1.0;
    double current = graph.vrlinkLinear;
    if (current < 0.0) {
        const CommandResult result = runCommand({"wpctl", "get-volume", std::to_string(graph.vrlinkNodeId)});
        if (!result.ok() || !parseSourceVolume(result.out, current)) return;
    }
    if (std::abs(current - want) < 0.05) {
        graph.vrlinkLinear = current;
        return;
    }
    if (writeNodeVolume(graph.vrlinkNodeId, want)) graph.vrlinkLinear = want;
}

bool writeMixEnabled(bool on) {
    return writeSetting(kMixSettingKey, on);
}

bool waitForMixNode(const std::string& nodeName, int& nodeId) {
    for (int attempt = 0; attempt < 25; ++attempt) {
        const CommandResult result = runCommand({"pw-dump", nodeName}, 3000);
        if (result.ok()) {
            JsonValue root;
            std::string error;
            if (parseJson(result.out, root, error) && root.type == JsonValue::Type::Array) {
                for (const JsonValue& object : root.items) {
                    const JsonValue* info = object.get("info");
                    const JsonValue* props = info != nullptr ? info->get("props") : nullptr;
                    const JsonValue* name = props != nullptr ? props->get("node.name") : nullptr;
                    const JsonValue* id = object.get("id");
                    if (name != nullptr && name->isString() && name->text == nodeName && id != nullptr && id->isNumber()) {
                        nodeId = static_cast<int>(id->number);
                        return true;
                    }
                }
            }
        }
        ::usleep(100000);
    }
    return false;
}

MixGraph parseMixDump(const std::string& text, const std::vector<MixChannel>& channels) {
    MixGraph graph;
    JsonValue root;
    std::string error;
    if (!parseJson(text, root, error) || root.type != JsonValue::Type::Array) return graph;
    graph.known = true;

    struct Node {
        int id = -1;
        std::string name;
        std::string mediaClass;
        std::string app;
        double volume = 1.0;
        bool muted = false;
        bool vrlink = false;
        bool virtualNode = false;
        int serial = -1;
        std::string playback;  ///< application.name、無ければ media.name（ノード名には落とさない）
        std::string binary;    ///< application.process.binary のファイル名
    };
    std::vector<Node> nodes;
    struct Link {
        int output = -1;
        int input = -1;
    };
    std::vector<Link> links;
    for (const JsonValue& object : root.items) {
        const JsonValue* type = object.get("type");
        const std::string typeText = type != nullptr && type->isString() ? type->text : std::string();
        const JsonValue* idValue = object.get("id");
        const int id = idValue != nullptr && idValue->isNumber() ? static_cast<int>(idValue->number) : -1;
        if (typeText.find("Node") != std::string::npos) {
            Node node;
            node.id = id;
            node.name = propText(object, "node.name");
            node.mediaClass = propText(object, "media.class");
            node.app = appLabel(object);
            node.volume = volumeOf(object);
            node.muted = muteOf(object);
            node.vrlink = isVrlink(object);
            node.serial = infoInt(object, "object.serial");
            const JsonValue* virt = prop(object, "node.virtual");
            node.virtualNode = virt != nullptr && ((virt->isBool() && virt->boolean) || (virt->isString() && virt->text == "true"));
            node.playback = propText(object, "application.name");
            if (node.playback.empty()) node.playback = propText(object, "media.name");
            node.binary = baseName(propText(object, "application.process.binary"));
            nodes.push_back(node);
        } else if (typeText.find("Link") != std::string::npos) {
            links.push_back({infoInt(object, "output-node-id"), infoInt(object, "input-node-id")});
        }
    }

    const auto findNode = [&](const std::string& name) -> const Node* {
        for (const Node& node : nodes) {
            if (node.name == name) return &node;
        }
        return nullptr;
    };
    const auto fill = [](const Node* node, const std::string& name) {
        MixNodeVolume volume;
        volume.nodeName = name;
        if (node != nullptr) {
            volume.known = true;
            volume.nodeId = node->id;
            volume.linear = node->volume;
            volume.muted = node->muted;
        }
        return volume;
    };

    if (const Node* source = findNode(kMixSourceNode)) {
        graph.coreRunning = true;
        (void)source;
    }
    graph.mic = fill(findNode(kMixMicLevelNode), kMixMicLevelNode);

    int vrlinkId = -1;
    for (const Node& node : nodes) {
        if (node.vrlink && node.mediaClass.find("Stream/Input") != std::string::npos) {
            graph.vrlinkPresent = true;
            vrlinkId = node.id;
            graph.vrlinkNodeId = node.id;
            graph.vrlinkLinear = node.volume;
        }
    }
    int sourceId = -1;
    if (const Node* source = findNode(kMixSourceNode)) sourceId = source->id;
    if (vrlinkId >= 0 && sourceId >= 0) {
        for (const Link& link : links) {
            if (link.output == sourceId && link.input == vrlinkId) graph.routed = true;
        }
    }

    std::vector<std::string> ids;
    for (const MixChannel& saved : channels) ids.push_back(saved.id);
    for (const Node& node : nodes) {
        // シンクだけ（frame_mix_ch1）。_to_bus や _virtual_sink_playback は長さが違う。
        if (node.name.rfind("frame_mix_ch", 0) != 0 || node.name.size() != 13) continue;
        const std::string id = node.name.substr(10);
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    for (const std::string& id : ids) {
        MixChannelLive live;
        live.id = id;
        const Node* sink = findNode(channelSinkNode(id));
        live.running = sink != nullptr;
        if (sink != nullptr) {
            live.sinkId = sink->id;
            live.sinkSerial = sink->serial;
        }
        live.toMic = fill(findNode(channelToMicNode(id)), channelToMicNode(id));
        live.toPhones = fill(findNode(channelToPhonesNode(id)), channelToPhonesNode(id));
        if (sink != nullptr) {
            for (const Link& link : links) {
                if (link.input != sink->id) continue;
                for (const Node& node : nodes) {
                    if (node.id != link.output || node.app.empty()) continue;
                    if (node.name.rfind("frame_mix_", 0) == 0) continue;
                    if (std::find(live.apps.begin(), live.apps.end(), node.app) == live.apps.end()) live.apps.push_back(node.app);
                }
            }
        }
        graph.channels.push_back(live);
    }

    // いま音を出しているアプリ（自分のループバックと空間オーディオは入れない）
    for (const Node& node : nodes) {
        if (node.mediaClass.find("Stream/Output/Audio") == std::string::npos) continue;
        if (node.virtualNode || node.playback.empty()) continue;
        if (node.name.rfind("frame_mix_", 0) == 0 || node.name.rfind("frame_micmix_", 0) == 0) continue;
        if (node.name.find("spatialize_filter_chain") != std::string::npos) continue;
        MixPlayback play;
        play.app = node.playback;
        play.nodeId = node.id;
        play.binary = node.binary;
        for (const Link& link : links) {
            if (link.output == node.id) play.linkedInput = link.input;
        }
        graph.playbacks.push_back(play);
    }
    return graph;
}

MixGraph readMixGraph(const std::vector<MixChannel>& channels) {
    MixGraph graph;
    const CommandResult result = runCommand({"pw-dump"}, 4000);
    if (!result.ok()) {
        std::fprintf(stderr, "[ミックス] pw-dump を読めません: %s\n", describeCommand({"pw-dump"}, result).c_str());
    } else {
        graph = parseMixDump(result.out, channels);
    }
    graph.runningApps = readRunningApps();
    return graph;
}

bool sameMixGraph(const MixGraph& a, const MixGraph& b) {
    if (a.known != b.known || a.settingKnown != b.settingKnown || a.enabled != b.enabled || a.coreRunning != b.coreRunning ||
        a.vrlinkPresent != b.vrlinkPresent || a.routed != b.routed) {
        return false;
    }
    if (a.mic.known != b.mic.known || a.mic.nodeId != b.mic.nodeId) return false;
    if (a.mic.known && std::abs(a.mic.linear - b.mic.linear) >= 0.02) return false;
    return a.channels == b.channels && a.playbacks == b.playbacks && a.runningApps == b.runningApps;
}

void applyMixPins(const std::vector<MixPin>& pins, const MixGraph& graph) {
    for (const MixPin& pin : pins) {
        int serial = -1;
        int sinkId = -1;
        for (const MixChannelLive& channel : graph.channels) {
            if (channel.id != pin.id) continue;
            serial = channel.sinkSerial;
            sinkId = channel.sinkId;
        }
        if (serial < 0) continue;
        for (const MixPlayback& play : graph.playbacks) {
            if (!mixAppMatches(pin.app, play.app, play.binary) || play.nodeId < 0 || play.linkedInput == sinkId) continue;
            const std::vector<std::string> argv = {"pw-metadata", "-n", "default", std::to_string(play.nodeId), "target.object",
                                                   std::to_string(serial)};
            const CommandResult result = runCommand(argv);
            std::fprintf(stderr, "[ミックス] %s\n", describeCommand(argv, result).c_str());
        }
    }
}
