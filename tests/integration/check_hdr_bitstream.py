#!/usr/bin/env python3
"""Verify the synthetic HermesHdrCuda sample using an independent FFmpeg decoder."""
import argparse
from fractions import Fraction
import json
import subprocess


def check(path, ffprobe):
    result = subprocess.run(
        [ffprobe, "-v", "error", "-select_streams", "v:0", "-show_streams",
         "-show_frames", "-of", "json", path],
        check=True, capture_output=True, text=True,
    )
    data = json.loads(result.stdout)
    stream, = data["streams"]
    expected = {
        "codec_name": "hevc", "profile": "Main 10", "pix_fmt": "yuv420p10le",
        "color_range": "tv", "color_space": "bt2020nc",
        "color_transfer": "smpte2084", "color_primaries": "bt2020",
    }
    for key, value in expected.items():
        if stream.get(key) != value:
            raise ValueError(f"{key}: {stream.get(key)!r}, expected {value!r}")
    frame, = data["frames"]
    side = {item["side_data_type"]: item for item in frame.get("side_data_list", [])}
    mastering = side["Mastering display metadata"]
    expected_mastering = {
        "red_x": Fraction(34000, 50000), "red_y": Fraction(16000, 50000),
        "green_x": Fraction(13250, 50000), "green_y": Fraction(34500, 50000),
        "blue_x": Fraction(7500, 50000), "blue_y": Fraction(3000, 50000),
        "white_point_x": Fraction(15635, 50000), "white_point_y": Fraction(16450, 50000),
        "min_luminance": Fraction(1, 10000), "max_luminance": Fraction(1000),
    }
    for key, value in expected_mastering.items():
        if Fraction(mastering[key]) != value:
            raise ValueError(f"{key}: {mastering[key]}, expected {value}")
    light = side["Content light level metadata"]
    if light["max_content"] != 1000 or light["max_average"] != 400:
        raise ValueError(f"Unexpected content light metadata: {light}")
    print("PASS: decoded HEVC Main10, BT.2020/PQ/limited, mastering primaries/luminance and CLL/FALL")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bitstream")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    check(args.bitstream, args.ffprobe)
