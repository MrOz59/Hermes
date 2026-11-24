#!/usr/bin/env bash
# Verify the synthetic HermesHdrCuda sample using an independent FFmpeg decoder.
set -euo pipefail
usage() {
    printf 'usage: %s BITSTREAM [--ffprobe PATH]\n' "$0"
}
ffprobe=ffprobe
bitstreams=()
while (( $# )); do
    case $1 in
        --help|-h) usage; exit 0 ;;
        --ffprobe)
            if (( $# < 2 )); then usage >&2; exit 2; fi
            ffprobe=$2
            shift
            ;;
        --ffprobe=*) ffprobe=${1#*=} ;;
        --) shift; bitstreams+=("$@"); break ;;
        -*) usage >&2; exit 2 ;;
        *) bitstreams+=("$1") ;;
    esac
    shift
done
if (( ${#bitstreams[@]} != 1 )); then
    echo "pass exactly one bitstream" >&2
    exit 2
fi

"$ffprobe" -v error -select_streams v:0 -show_streams -show_frames -of json "${bitstreams[0]}" |
    jq -er '
        def require($field; $expected):
            if .[$field] == $expected then .
            else error("\($field): \(.[$field] | @json), expected \($expected | @json)") end;
        # FFmpeg rational components are signed 32-bit integers. Their cross-products
        # with the fixture denominators fit exactly in jq numbers.
        def rational($field; $num; $den):
            (.[$field] | tostring) as $actual |
            ([$actual | capture("^(?<num>-?[0-9]{1,10})(?:/(?<den>-?[0-9]{1,10}))?$")][0]) as $parts |
            if $parts != null and
               ($parts.num | tonumber) * $den == $num * ($parts.den // "1" | tonumber) and
               ($parts.den // "1" | tonumber) != 0 then .
            else error("\($field): \($actual), expected \($num)/\($den)") end;
        if (.streams | length) != 1 or (.frames | length) != 1 then
            error("expected exactly one video stream and frame")
        else . end |
        . as $decoded |
        (.streams[0] |
            require("codec_name"; "hevc") | require("profile"; "Main 10") |
            require("pix_fmt"; "yuv420p10le") | require("color_range"; "tv") |
            require("color_space"; "bt2020nc") | require("color_transfer"; "smpte2084") |
            require("color_primaries"; "bt2020")) as $stream |
        ($decoded.frames[0].side_data_list // []) |
        (first(.[] | select(.side_data_type == "Mastering display metadata")) // null) as $mastering |
        (first(.[] | select(.side_data_type == "Content light level metadata")) // null) as $light |
        if $mastering == null or $light == null then error("missing HDR mastering or content light metadata")
        else
            ($mastering |
                rational("red_x"; 34000; 50000) | rational("red_y"; 16000; 50000) |
                rational("green_x"; 13250; 50000) | rational("green_y"; 34500; 50000) |
                rational("blue_x"; 7500; 50000) | rational("blue_y"; 3000; 50000) |
                rational("white_point_x"; 15635; 50000) | rational("white_point_y"; 16450; 50000) |
                rational("min_luminance"; 1; 10000) | rational("max_luminance"; 1000; 1)) as $verified |
            ($light | require("max_content"; 1000) | require("max_average"; 400)) as $verified_light |
            "PASS: decoded HEVC Main10, BT.2020/PQ/limited, mastering primaries/luminance and CLL/FALL"
        end
    '
