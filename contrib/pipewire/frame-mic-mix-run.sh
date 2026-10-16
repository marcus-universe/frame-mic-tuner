#!/bin/bash
# Frame Mic Mix graph, as session clients of the already running PipeWire.
# A second `pipewire -c` process would be its own graph and could not see the
# headset mic or the speakers, so this runs pw-loopback instead.
#
#   frame-mic-mix-run.sh core
#   frame-mic-mix-run.sh channel ch3
#
# The mic is taken after Valve's filters (the default loopback source). Every
# capture stream is node.virtual, so it does not turn those filters on by itself.
set -uo pipefail

mode="${1:-}"
id="${2:-}"
name_dir="${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/frame-mic-mix"
mic_source="alsa_loopback_device.alsa_input.platform-sound.HiFi__Mic__source"

pids=()
cleanup() {
    local pid
    for pid in "${pids[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

start() {
    pw-loopback "$@" &
    pids+=("$!")
}

if [[ "$mode" == "core" ]]; then
    # Bus sink + the source VRLink records from. Low priority so it never becomes the default output.
    start \
        --capture-props="{ media.class = Audio/Sink node.name = frame_micmix_sink node.description = \"Frame Mix Bus\" node.virtual = true audio.position = [ FL FR ] priority.session = 1 media.name = \"Frame Mix Bus\" }" \
        --playback-props="{ media.class = Audio/Source node.name = frame_micmix_source node.description = \"Frame Mic + Mix\" audio.position = [ FL FR ] priority.session = 2000 media.name = \"Frame Mic + Mix\" }"
    # Filtered built-in mic into the bus at unity. The Microphone slider sets the
    # default source volume, which this capture already hears.
    start \
        --capture-props="{ node.name = frame_micmix_from_mic target.object = \"$mic_source\" node.passive = true node.virtual = true audio.position = [ FL FR ] stream.dont-remix = true media.name = \"Frame Mix from mic\" }" \
        --playback-props="{ node.name = frame_micmix_mic_virtual_sink target.object = frame_micmix_sink node.passive = true node.virtual = true media.name = \"Frame Mix mic level\" }"
elif [[ "$mode" == "channel" ]]; then
    if [[ ! "$id" =~ ^ch[1-8]$ ]]; then
        echo "frame-mic-mix: bad channel id: $id" >&2
        exit 1
    fi
    name_file="$name_dir/$id.name"
    if [[ ! -f "$name_file" ]]; then
        echo "frame-mic-mix: missing $name_file" >&2
        exit 1
    fi
    name="$(head -n 1 "$name_file" | tr -d '\r')"
    esc="${name//\\/\\\\}"
    esc="${esc//\"/\\\"}"
    # Sink apps select ("Frame Mix: Music"). No target.object: playback follows the default output.
    # The playback node name contains "virtual" and "sink" so SteamOS does not spatialize it.
    start \
        --capture-props="{ media.class = Audio/Sink node.name = frame_mix_${id} node.description = \"Frame Mix: ${esc}\" audio.position = [ FL FR ] priority.session = 100 media.name = \"Frame Mix ${id}\" }" \
        --playback-props="{ node.name = frame_mix_${id}_virtual_sink_playback node.passive = true node.virtual = true audio.position = [ FL FR ] media.name = \"Frame Mix ${id} headphones\" }"
    # Monitor of that sink into the mix bus. Volume of this playback node is the "to mic" slider.
    start \
        --capture-props="{ node.name = frame_mix_${id}_from_monitor stream.capture.sink = true target.object = frame_mix_${id} node.passive = true node.virtual = true audio.position = [ FL FR ] media.name = \"Frame Mix ${id} monitor\" }" \
        --playback-props="{ node.name = frame_mix_${id}_virtual_sink_to_bus target.object = frame_micmix_sink node.passive = true node.virtual = true audio.position = [ FL FR ] media.name = \"Frame Mix ${id} mic\" }"
else
    echo "usage: frame-mic-mix-run.sh core | channel chN" >&2
    exit 2
fi

# If either loopback exits, stop the other and let systemd restart the unit.
wait -n
exit 1
