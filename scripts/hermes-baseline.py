#!/usr/bin/env python3

"""Capture and summarize deterministic Hermes H0 telemetry windows."""

from __future__ import annotations

import argparse
import json
import math
import ssl
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


NETWORK_STAGES = (
    "send_queue",
    "packetization",
    "fec",
    "pacer",
    "send",
    "capture_to_last_send",
)

DEFAULT_LAN_PROFILES = ("clean", "lan")
DEFAULT_CONSTRAINED_PROFILES = (
    "wifi",
    "wan",
    "burst-loss",
    "bufferbloat",
    "reordering",
)
GATE_CONSTRAINED_STAGES = ("send_queue",)
HESTIA_TRACE_MARKER = "HESTIA_FRAME_TRACE "
HESTIA_TRACE_STAGES = (
    "reassembly",
    "decode",
    "presentation_queue",
    "render",
    "receive_to_present",
)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def finite_number(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{field} must be a number")
    result = float(value)
    if not math.isfinite(result):
        raise ValueError(f"{field} must be finite")
    return result


def positive_integer(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{field} must be a positive integer")
    return value


def nonnegative_integer(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{field} must be a non-negative integer")
    return value


def extract_pipeline(document: Any) -> dict[str, Any] | None:
    if not isinstance(document, dict):
        raise ValueError("diagnostics response must be a JSON object")
    runtime = document.get("runtime")
    if not isinstance(runtime, dict):
        raise ValueError("diagnostics response is missing runtime")
    pipeline = runtime.get("pipeline")
    if pipeline is None:
        return None
    if not isinstance(pipeline, dict):
        raise ValueError("runtime.pipeline must be an object or null")
    positive_integer(pipeline.get("window_sequence"), "pipeline.window_sequence")
    return pipeline


def capture_identity(pipeline: dict[str, Any]) -> tuple[int, int]:
    encode_sequence = positive_integer(
        pipeline.get("window_sequence"), "pipeline.window_sequence"
    )
    network = pipeline.get("network")
    network_sequence = 0
    if network is not None:
        if not isinstance(network, dict):
            raise ValueError("pipeline.network must be an object or null")
        network_sequence = positive_integer(
            network.get("window_sequence"), "pipeline.network.window_sequence"
        )
    return encode_sequence, network_sequence


def read_hestia_identity(
    settings_path: str | None = None,
) -> tuple[bytes, bytes]:
    try:
        from PyQt6.QtCore import (  # type: ignore[import-not-found]
            QByteArray,
            QCoreApplication,
            QSettings,
        )
    except ImportError:
        try:
            from PyQt5.QtCore import (  # type: ignore[import-not-found,no-redef]
                QByteArray,
                QCoreApplication,
                QSettings,
            )
        except ImportError as error:
            raise ValueError(
                "--hestia-identity requires PyQt5 or PyQt6"
            ) from error

    if settings_path:
        ini_format = (
            QSettings.Format.IniFormat
            if hasattr(QSettings, "Format")
            else QSettings.IniFormat
        )
        settings = QSettings(settings_path, ini_format)
    else:
        QCoreApplication.setOrganizationName(
            "Moonlight Game Streaming Project"
        )
        QCoreApplication.setOrganizationDomain("moonlight-stream.com")
        QCoreApplication.setApplicationName("Moonlight")
        settings = QSettings()

    def byte_value(name: str) -> bytes:
        value = settings.value(name, QByteArray())
        if isinstance(value, QByteArray):
            return bytes(value)
        if isinstance(value, bytes):
            return value
        if isinstance(value, str):
            return value.encode()
        return b""

    certificate = byte_value("certificate")
    private_key = byte_value("key")
    if not certificate or not private_key:
        location = settings.fileName()
        raise ValueError(
            "Hestia identity is missing; launch/pair Hestia first "
            f"(checked {location})"
        )
    return certificate, private_key


def build_ssl_context(args: argparse.Namespace) -> ssl.SSLContext:
    if args.hestia_settings and not args.hestia_identity:
        raise ValueError("--hestia-settings requires --hestia-identity")
    if args.hestia_identity and (args.cert or args.key):
        raise ValueError(
            "--hestia-identity cannot be combined with --cert or --key"
        )

    ca_file = args.ca
    if args.hestia_identity and not ca_file:
        default_ca = (
            Path.home() / ".config" / "sunshine" / "credentials" / "cacert.pem"
        )
        if default_ca.is_file():
            ca_file = str(default_ca)

    if args.insecure:
        context = ssl._create_unverified_context()  # noqa: SLF001
    else:
        context = ssl.create_default_context(cafile=ca_file)

    if args.key and not args.cert:
        raise ValueError("--key requires --cert")
    if args.cert:
        context.load_cert_chain(args.cert, args.key)
    elif args.hestia_identity:
        certificate, private_key = read_hestia_identity(
            args.hestia_settings
        )
        with tempfile.TemporaryDirectory(prefix="hermes-hestia-identity-") as root:
            certificate_path = Path(root) / "client-cert.pem"
            private_key_path = Path(root) / "client-key.pem"
            certificate_path.write_bytes(certificate)
            private_key_path.write_bytes(private_key)
            certificate_path.chmod(0o600)
            private_key_path.chmod(0o600)
            context.load_cert_chain(certificate_path, private_key_path)
    return context


def fetch_diagnostics(
    url: str, context: ssl.SSLContext, timeout: float
) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        headers={"Accept": "application/json", "User-Agent": "hermes-baseline/1"},
    )
    with urllib.request.urlopen(request, context=context, timeout=timeout) as response:
        payload = response.read()
    return json.loads(payload)


def capture(args: argparse.Namespace) -> int:
    if args.duration <= 0:
        raise ValueError("--duration must be greater than zero")
    if args.interval <= 0:
        raise ValueError("--interval must be greater than zero")
    if args.timeout <= 0:
        raise ValueError("--timeout must be greater than zero")

    context = build_ssl_context(args)
    output = Path(args.output)
    mode = "w" if args.overwrite else "x"
    run_id = uuid.uuid4().hex
    deadline = time.monotonic() + args.duration
    last_identity: tuple[int, int] | None = None
    last_error: str | None = None
    samples = 0
    errors = 0

    with output.open(mode, encoding="utf-8") as destination:
        while True:
            poll_started = time.monotonic()
            try:
                document = fetch_diagnostics(args.url, context, args.timeout)
                pipeline = extract_pipeline(document)
                if pipeline is not None:
                    identity = capture_identity(pipeline)
                    if identity != last_identity:
                        record = {
                            "schema": 1,
                            "run_id": run_id,
                            "profile": args.profile,
                            "captured_at_utc": utc_now(),
                            "pipeline": pipeline,
                        }
                        destination.write(
                            json.dumps(record, separators=(",", ":"), sort_keys=True)
                            + "\n"
                        )
                        destination.flush()
                        last_identity = identity
                        samples += 1
                last_error = None
            except (
                OSError,
                ValueError,
                json.JSONDecodeError,
                urllib.error.URLError,
            ) as error:
                errors += 1
                message = str(error)
                if message != last_error:
                    print(f"capture warning: {message}", file=sys.stderr)
                    last_error = message

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            sleep_for = max(0.0, args.interval - (time.monotonic() - poll_started))
            time.sleep(min(sleep_for, remaining))

    print(
        f"Captured {samples} unique telemetry states for profile "
        f"'{args.profile}' ({errors} poll errors) into {output}"
    )
    return 0 if samples > 0 else 1


def load_records(paths: Iterable[Path]) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for path in paths:
        with path.open(encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                if not line.strip():
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as error:
                    raise ValueError(f"{path}:{line_number}: {error}") from error
                if (
                    not isinstance(record, dict)
                    or isinstance(record.get("schema"), bool)
                    or record.get("schema") != 1
                ):
                    raise ValueError(
                        f"{path}:{line_number}: unsupported or missing schema"
                    )
                if not isinstance(record.get("pipeline"), dict):
                    raise ValueError(
                        f"{path}:{line_number}: pipeline must be an object"
                    )
                record["_source"] = str(path)
                records.append(record)
    return records


def validate_hestia_trace(trace: Any, source: str) -> dict[str, Any]:
    if not isinstance(trace, dict):
        raise ValueError(f"{source}: Hestia trace must be a JSON object")
    if (
        isinstance(trace.get("schema"), bool)
        or trace.get("schema") != 1
    ):
        raise ValueError(f"{source}: unsupported Hestia trace schema")
    frame_id = trace.get("frame_id")
    if isinstance(frame_id, bool) or not isinstance(frame_id, int) or frame_id < 0:
        raise ValueError(f"{source}: frame_id must be a non-negative integer")
    for field in (
        "receive_us",
        "assemble_us",
        "decode_us",
        "present_start_us",
        "terminal_us",
    ):
        nonnegative_integer(trace.get(field), f"{source}.{field}")
    for field in ("outcome", "reason"):
        if not isinstance(trace.get(field), str) or not trace[field]:
            raise ValueError(f"{source}: {field} must be a non-empty string")
    return trace


def extract_hestia_trace_line(line: str, source: str) -> dict[str, Any] | None:
    marker_offset = line.find(HESTIA_TRACE_MARKER)
    if marker_offset >= 0:
        payload = line[marker_offset + len(HESTIA_TRACE_MARKER):].strip()
    elif line.lstrip().startswith("{"):
        payload = line.strip()
    else:
        return None
    try:
        trace = json.loads(payload)
    except json.JSONDecodeError as error:
        raise ValueError(f"{source}: invalid Hestia trace JSON: {error}") from error
    if not isinstance(trace, dict) or "frame_id" not in trace:
        return None
    return validate_hestia_trace(trace, source)


def import_hestia(args: argparse.Namespace) -> int:
    output = Path(args.output)
    mode = "w" if args.overwrite else "x"
    run_id = uuid.uuid4().hex
    imported = 0
    seen_frames: set[int] = set()

    with output.open(mode, encoding="utf-8") as destination:
        for input_name in args.inputs:
            input_path = Path(input_name)
            with input_path.open(encoding="utf-8", errors="replace") as source:
                for line_number, line in enumerate(source, 1):
                    trace = extract_hestia_trace_line(
                        line, f"{input_path}:{line_number}"
                    )
                    if trace is None:
                        continue
                    frame_id = trace["frame_id"]
                    if frame_id in seen_frames:
                        continue
                    seen_frames.add(frame_id)
                    record = {
                        "schema": 1,
                        "kind": "hestia_frame_trace",
                        "run_id": run_id,
                        "profile": args.profile,
                        "imported_at_utc": utc_now(),
                        "trace": trace,
                    }
                    destination.write(
                        json.dumps(record, separators=(",", ":"), sort_keys=True)
                        + "\n"
                    )
                    imported += 1

    print(
        f"Imported {imported} unique Hestia frame traces for profile "
        f"'{args.profile}' into {output}"
    )
    return 0 if imported > 0 else 1


def load_hestia_records(paths: Iterable[Path]) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    for path in paths:
        with path.open(encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                if not line.strip():
                    continue
                location = f"{path}:{line_number}"
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as error:
                    raise ValueError(f"{location}: {error}") from error
                if (
                    not isinstance(record, dict)
                    or record.get("schema") != 1
                    or record.get("kind") != "hestia_frame_trace"
                ):
                    raise ValueError(
                        f"{location}: expected an imported Hestia frame trace"
                    )
                validate_hestia_trace(record.get("trace"), location)
                record["_source"] = str(path)
                records.append(record)
    return records


def weighted_average(values: list[float], weights: list[float]) -> float:
    total_weight = sum(weights)
    if not values or total_weight <= 0:
        return 0.0
    return sum(value * weight for value, weight in zip(values, weights)) / total_weight


def top_level_stage(
    pipeline: dict[str, Any], prefix: str
) -> dict[str, float]:
    return {
        "mean_ms": finite_number(pipeline.get(f"{prefix}_ms"), f"{prefix}_ms"),
        "p50_ms": finite_number(
            pipeline.get(f"{prefix}_p50_ms"), f"{prefix}_p50_ms"
        ),
        "p95_ms": finite_number(
            pipeline.get(f"{prefix}_p95_ms"), f"{prefix}_p95_ms"
        ),
        "p99_ms": finite_number(
            pipeline.get(f"{prefix}_p99_ms"), f"{prefix}_p99_ms"
        ),
    }


def network_stage(network: dict[str, Any], name: str) -> dict[str, float]:
    stage = network.get(name)
    if not isinstance(stage, dict):
        raise ValueError(f"network.{name} must be an object")
    return {
        field: finite_number(stage.get(field), f"network.{name}.{field}")
        for field in ("mean_ms", "p50_ms", "p95_ms", "p99_ms")
    }


def stage_summary(
    windows: list[tuple[dict[str, float], int]]
) -> dict[str, float | int]:
    weights = [float(frame_count) for _, frame_count in windows]
    summary: dict[str, float | int] = {
        "windows": len(windows),
        "frames": sum(frame_count for _, frame_count in windows),
    }
    for field in ("mean_ms", "p50_ms", "p95_ms", "p99_ms"):
        summary[field] = weighted_average(
            [stage[field] for stage, _ in windows], weights
        )
    summary["worst_p99_ms"] = max(
        (stage["p99_ms"] for stage, _ in windows), default=0.0
    )
    return summary


def summarize_records(
    records: Iterable[dict[str, Any]]
) -> dict[str, dict[str, Any]]:
    grouped: dict[str, dict[str, Any]] = defaultdict(
        lambda: {
            "encode_seen": set(),
            "network_seen": set(),
            "encode_windows": [],
            "network_windows": [],
        }
    )

    for record in records:
        profile = str(record.get("profile") or "unlabelled")
        run_id = str(record.get("run_id") or record.get("_source") or "unknown")
        pipeline = record["pipeline"]
        encode_sequence = positive_integer(
            pipeline.get("window_sequence"), "pipeline.window_sequence"
        )
        encode_key = (run_id, encode_sequence)
        group = grouped[profile]
        if encode_key not in group["encode_seen"]:
            frames = positive_integer(
                pipeline.get("window_frames"), "pipeline.window_frames"
            )
            group["encode_seen"].add(encode_key)
            group["encode_windows"].append((pipeline, frames))

        network = pipeline.get("network")
        if isinstance(network, dict):
            sequence = positive_integer(
                network.get("window_sequence"), "pipeline.network.window_sequence"
            )
            network_key = (run_id, sequence)
            if network_key not in group["network_seen"]:
                frames = positive_integer(
                    network.get("window_frames"), "pipeline.network.window_frames"
                )
                group["network_seen"].add(network_key)
                group["network_windows"].append((network, frames))

    result: dict[str, dict[str, Any]] = {}
    for profile, group in grouped.items():
        stages: dict[str, dict[str, float | int]] = {}
        encode_windows = group["encode_windows"]
        stages["capture_to_encode"] = stage_summary(
            [
                (top_level_stage(window, "capture_to_encode"), frames)
                for window, frames in encode_windows
            ]
        )
        stages["encode"] = stage_summary(
            [
                (top_level_stage(window, "encode"), frames)
                for window, frames in encode_windows
            ]
        )

        network_windows = group["network_windows"]
        for name in NETWORK_STAGES:
            stages[name] = stage_summary(
                [
                    (network_stage(window, name), frames)
                    for window, frames in network_windows
                ]
            )

        duration_weights = [
            finite_number(window.get("window_duration_ms"), "window_duration_ms")
            for window, _ in encode_windows
        ]
        fps = weighted_average(
            [finite_number(window.get("fps"), "fps") for window, _ in encode_windows],
            duration_weights,
        )
        bitrate = weighted_average(
            [
                finite_number(window.get("bitrate_kbps"), "bitrate_kbps")
                for window, _ in encode_windows
            ],
            duration_weights,
        )

        network_duration_weights = [
            finite_number(
                window.get("window_duration_ms"), "network.window_duration_ms"
            )
            for window, _ in network_windows
        ]
        wire_bitrate = weighted_average(
            [
                finite_number(
                    window.get("wire_bitrate_kbps"), "network.wire_bitrate_kbps"
                )
                for window, _ in network_windows
            ],
            network_duration_weights,
        )
        data_shards = sum(
            nonnegative_integer(window.get("data_shards"), "network.data_shards")
            for window, _ in network_windows
        )
        fec_shards = sum(
            nonnegative_integer(window.get("fec_shards"), "network.fec_shards")
            for window, _ in network_windows
        )

        result[profile] = {
            "stages": stages,
            "encode_windows": len(encode_windows),
            "network_windows": len(network_windows),
            "fps": fps,
            "bitrate_kbps": bitrate,
            "wire_bitrate_kbps": wire_bitrate,
            "fec_overhead_percent": (
                fec_shards * 100.0 / data_shards if data_shards else 0.0
            ),
        }
    return result


def nearest_rank_percentile(values: list[float], percentile: int) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = max(1, math.ceil(percentile * len(ordered) / 100))
    return ordered[rank - 1]


def summarize_hestia_records(
    records: Iterable[dict[str, Any]]
) -> dict[str, dict[str, Any]]:
    grouped: dict[str, dict[str, Any]] = defaultdict(
        lambda: {
            "seen": set(),
            "run_frames": defaultdict(set),
            "terminal_frames": 0,
            "presented_frames": 0,
            "dropped_frames": 0,
            "samples": {stage: [] for stage in HESTIA_TRACE_STAGES},
        }
    )

    for record in records:
        profile = str(record.get("profile") or "unlabelled")
        run_id = str(record.get("run_id") or record.get("_source") or "unknown")
        trace = validate_hestia_trace(
            record.get("trace"), str(record.get("_source") or "Hestia trace")
        )
        key = (run_id, trace["frame_id"])
        group = grouped[profile]
        if key in group["seen"]:
            continue
        group["seen"].add(key)
        group["run_frames"][run_id].add(trace["frame_id"])
        group["terminal_frames"] += 1

        if trace["outcome"] != "presented":
            group["dropped_frames"] += 1
            continue

        timestamps = [
            trace["receive_us"],
            trace["assemble_us"],
            trace["decode_us"],
            trace["present_start_us"],
            trace["terminal_us"],
        ]
        if any(timestamp <= 0 for timestamp in timestamps):
            raise ValueError(
                f"Hestia presented frame {trace['frame_id']} has a missing timestamp"
            )
        if timestamps != sorted(timestamps):
            raise ValueError(
                f"Hestia presented frame {trace['frame_id']} has "
                "non-monotonic timestamps"
            )

        group["presented_frames"] += 1
        durations_us = {
            "reassembly": timestamps[1] - timestamps[0],
            "decode": timestamps[2] - timestamps[1],
            "presentation_queue": timestamps[3] - timestamps[2],
            "render": timestamps[4] - timestamps[3],
            "receive_to_present": timestamps[4] - timestamps[0],
        }
        for stage, duration_us in durations_us.items():
            group["samples"][stage].append(duration_us / 1000.0)

    result: dict[str, dict[str, Any]] = {}
    for profile, group in grouped.items():
        terminal_frames = group["terminal_frames"]
        expected_frames = sum(
            max(frame_ids) - min(frame_ids) + 1
            for frame_ids in group["run_frames"].values()
            if frame_ids
        )
        missing_frames = max(0, expected_frames - terminal_frames)
        result[profile] = {
            "terminal_frames": terminal_frames,
            "expected_frames": expected_frames,
            "network_missing_frames": missing_frames,
            "network_loss_percent": (
                missing_frames * 100.0 / expected_frames
                if expected_frames
                else 0.0
            ),
            "presented_frames": group["presented_frames"],
            "dropped_frames": group["dropped_frames"],
            "drop_percent": (
                group["dropped_frames"] * 100.0 / terminal_frames
                if terminal_frames
                else 0.0
            ),
            "stages": {
                stage: {
                    "frames": len(values),
                    "p50_ms": nearest_rank_percentile(values, 50),
                    "p95_ms": nearest_rank_percentile(values, 95),
                    "p99_ms": nearest_rank_percentile(values, 99),
                }
                for stage, values in group["samples"].items()
            },
        }
    return result


def render_markdown(summary: dict[str, dict[str, Any]]) -> str:
    lines = [
        "# Hermes H0 baseline report",
        "",
        "Percentile columns are frame-count-weighted means of each published "
        "one-second window; `worst p99` is the largest window p99. They are not "
        "reconstructed global percentiles.",
        "",
    ]
    for profile in sorted(summary):
        data = summary[profile]
        lines.extend(
            [
                f"## {profile}",
                "",
                f"- Encode windows: {data['encode_windows']}",
                f"- Network windows: {data['network_windows']}",
                f"- Mean FPS: {data['fps']:.2f}",
                f"- Mean payload bitrate: {data['bitrate_kbps']:.2f} kbit/s",
                f"- Mean wire bitrate: {data['wire_bitrate_kbps']:.2f} kbit/s",
                f"- FEC shard overhead: {data['fec_overhead_percent']:.2f}%",
                "",
                "| Stage | Windows | Frames | Mean ms | p50 ms | p95 ms | p99 ms | Worst p99 ms |",
                "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for stage_name, stage in data["stages"].items():
            lines.append(
                f"| {stage_name} | {stage['windows']} | {stage['frames']} | "
                f"{stage['mean_ms']:.3f} | {stage['p50_ms']:.3f} | "
                f"{stage['p95_ms']:.3f} | {stage['p99_ms']:.3f} | "
                f"{stage['worst_p99_ms']:.3f} |"
            )
        lines.append("")
    return "\n".join(lines)


def report(args: argparse.Namespace) -> int:
    records = load_records(Path(path) for path in args.inputs)
    if not records:
        raise ValueError("no telemetry records found")
    markdown = render_markdown(summarize_records(records))
    if args.output:
        mode = "w" if args.overwrite else "x"
        with Path(args.output).open(mode, encoding="utf-8") as destination:
            destination.write(markdown + "\n")
    else:
        print(markdown)
    return 0


def evaluate_h2_gate(
    reference: dict[str, dict[str, Any]],
    candidate: dict[str, dict[str, Any]],
    *,
    client_reference: dict[str, dict[str, Any]] | None = None,
    client_candidate: dict[str, dict[str, Any]] | None = None,
    require_client: bool = False,
    lan_profiles: Iterable[str] = DEFAULT_LAN_PROFILES,
    constrained_profiles: Iterable[str] = DEFAULT_CONSTRAINED_PROFILES,
    min_windows: int = 30,
    min_client_frames: int = 300,
    max_lan_regression_ms: float = 2.0,
    max_lan_fps_regression_percent: float = 2.0,
    max_lan_client_loss_regression_points: float = 0.5,
    min_constrained_improvement_percent: float = 1.0,
    constrained_tolerance_ms: float = 0.0,
    max_constrained_send_queue_p99_ms: float = 100.0,
    max_client_loss_percent: float = 10.0,
) -> dict[str, Any]:
    """Evaluate paired Hermes/Hestia H2 acceptance metrics."""

    if min_windows <= 0:
        raise ValueError("--min-windows must be greater than zero")
    if min_client_frames <= 0:
        raise ValueError("--min-client-frames must be greater than zero")
    thresholds = {
        "--max-lan-regression-ms": max_lan_regression_ms,
        "--max-lan-fps-regression-percent": max_lan_fps_regression_percent,
        "--max-lan-client-loss-regression-points": (
            max_lan_client_loss_regression_points
        ),
        "--min-constrained-improvement-percent": (
            min_constrained_improvement_percent
        ),
        "--constrained-tolerance-ms": constrained_tolerance_ms,
        "--max-constrained-send-queue-p99-ms": (
            max_constrained_send_queue_p99_ms
        ),
        "--max-client-loss-percent": max_client_loss_percent,
    }
    for field, value in thresholds.items():
        checked = finite_number(value, field)
        if checked < 0:
            raise ValueError(f"{field} must be non-negative")
    if min_constrained_improvement_percent > 100:
        raise ValueError(
            "--min-constrained-improvement-percent cannot exceed 100"
        )

    lan_profiles = tuple(dict.fromkeys(lan_profiles))
    constrained_profiles = tuple(dict.fromkeys(constrained_profiles))
    if not lan_profiles:
        raise ValueError("at least one LAN profile is required")
    if not constrained_profiles:
        raise ValueError("at least one constrained profile is required")

    failures: list[str] = []
    checks: list[dict[str, Any]] = []
    comparable_profiles: set[str] = set()
    comparable_client_profiles: set[str] = set()

    for profile in (*lan_profiles, *constrained_profiles):
        reference_data = reference.get(profile)
        candidate_data = candidate.get(profile)
        if reference_data is None:
            failures.append(f"reference is missing profile '{profile}'")
        if candidate_data is None:
            failures.append(f"candidate is missing profile '{profile}'")
        if reference_data is None or candidate_data is None:
            continue

        sample_counts_ok = True
        for role, data in (
            ("reference", reference_data),
            ("candidate", candidate_data),
        ):
            for field in ("encode_windows", "network_windows"):
                count = nonnegative_integer(
                    data.get(field), f"{role}.{profile}.{field}"
                )
                if count < min_windows:
                    failures.append(
                        f"{role} profile '{profile}' has {count} {field}; "
                        f"requires at least {min_windows}"
                    )
                    sample_counts_ok = False
        if sample_counts_ok:
            comparable_profiles.add(profile)

    if (client_reference is None) != (client_candidate is None):
        failures.append(
            "both client reference and candidate traces must be provided together"
        )
    elif client_reference is None or client_candidate is None:
        if require_client:
            failures.append(
                "paired Hestia reference and candidate traces are required"
            )
    else:
        for profile in (*lan_profiles, *constrained_profiles):
            reference_data = client_reference.get(profile)
            candidate_data = client_candidate.get(profile)
            if reference_data is None:
                failures.append(
                    f"Hestia reference is missing profile '{profile}'"
                )
            if candidate_data is None:
                failures.append(
                    f"Hestia candidate is missing profile '{profile}'"
                )
            if reference_data is None or candidate_data is None:
                continue

            sample_counts_ok = True
            for role, data in (
                ("reference", reference_data),
                ("candidate", candidate_data),
            ):
                count = nonnegative_integer(
                    data.get("presented_frames"),
                    f"Hestia {role}.{profile}.presented_frames",
                )
                if count < min_client_frames:
                    failures.append(
                        f"Hestia {role} profile '{profile}' has {count} "
                        f"presented frames; requires at least {min_client_frames}"
                    )
                    sample_counts_ok = False
            if sample_counts_ok:
                comparable_client_profiles.add(profile)

    for profile in lan_profiles:
        if profile not in comparable_profiles:
            continue
        reference_data = reference[profile]
        candidate_data = candidate[profile]
        stage_name = "capture_to_last_send"
        reference_stage = reference_data["stages"][stage_name]
        candidate_stage = candidate_data["stages"][stage_name]
        for percentile in ("p95_ms", "p99_ms"):
            reference_value = finite_number(
                reference_stage.get(percentile),
                f"reference.{profile}.{stage_name}.{percentile}",
            )
            candidate_value = finite_number(
                candidate_stage.get(percentile),
                f"candidate.{profile}.{stage_name}.{percentile}",
            )
            limit = reference_value + max_lan_regression_ms
            checks.append(
                {
                    "profile": profile,
                    "metric": f"{stage_name}.{percentile}",
                    "reference": reference_value,
                    "candidate": candidate_value,
                    "limit": (
                        f"≤ reference + {max_lan_regression_ms:.3f} ms"
                    ),
                    "passed": candidate_value <= limit,
                }
            )

        reference_fps = finite_number(
            reference_data.get("fps"), f"reference.{profile}.fps"
        )
        candidate_fps = finite_number(
            candidate_data.get("fps"), f"candidate.{profile}.fps"
        )
        fps_factor = 1.0 - max_lan_fps_regression_percent / 100.0
        checks.append(
            {
                "profile": profile,
                "metric": "fps",
                "reference": reference_fps,
                "candidate": candidate_fps,
                "limit": f"≥ reference × {fps_factor:.3f}",
                "passed": candidate_fps >= reference_fps * fps_factor,
            }
        )

    improvement_factor = 1.0 - min_constrained_improvement_percent / 100.0
    for profile in constrained_profiles:
        if profile not in comparable_profiles:
            continue
        for stage_name in GATE_CONSTRAINED_STAGES:
            reference_stage = reference[profile]["stages"][stage_name]
            candidate_stage = candidate[profile]["stages"][stage_name]
            for percentile in ("p95_ms", "p99_ms"):
                reference_value = finite_number(
                    reference_stage.get(percentile),
                    f"reference.{profile}.{stage_name}.{percentile}",
                )
                candidate_value = finite_number(
                    candidate_stage.get(percentile),
                    f"candidate.{profile}.{stage_name}.{percentile}",
                )
                limit = (
                    reference_value * improvement_factor
                    + constrained_tolerance_ms
                )
                checks.append(
                    {
                        "profile": profile,
                        "metric": f"{stage_name}.{percentile}",
                        "reference": reference_value,
                        "candidate": candidate_value,
                        "limit": (
                            "≤ reference × "
                            f"{improvement_factor:.3f} + "
                            f"{constrained_tolerance_ms:.3f} ms"
                        ),
                        "passed": candidate_value <= limit,
                    }
                )
        reference_total = finite_number(
            reference[profile]["stages"]["send_queue"].get("worst_p99_ms"),
            f"reference.{profile}.send_queue.worst_p99_ms",
        )
        candidate_total = finite_number(
            candidate[profile]["stages"]["send_queue"].get("worst_p99_ms"),
            f"candidate.{profile}.send_queue.worst_p99_ms",
        )
        checks.append(
            {
                "profile": profile,
                "metric": "send_queue.worst_p99_ms",
                "reference": reference_total,
                "candidate": candidate_total,
                "limit": (
                    f"≤ {max_constrained_send_queue_p99_ms:.3f} ms"
                ),
                "passed": (
                    candidate_total <= max_constrained_send_queue_p99_ms
                ),
            }
        )

    if client_reference is not None and client_candidate is not None:
        for profile in lan_profiles:
            if profile not in comparable_client_profiles:
                continue
            reference_stage = client_reference[profile]["stages"][
                "receive_to_present"
            ]
            candidate_stage = client_candidate[profile]["stages"][
                "receive_to_present"
            ]
            for percentile in ("p95_ms", "p99_ms"):
                reference_value = finite_number(
                    reference_stage.get(percentile),
                    f"Hestia reference.{profile}.{percentile}",
                )
                candidate_value = finite_number(
                    candidate_stage.get(percentile),
                    f"Hestia candidate.{profile}.{percentile}",
                )
                checks.append(
                    {
                        "profile": profile,
                        "metric": f"Hestia.receive_to_present.{percentile}",
                        "reference": reference_value,
                        "candidate": candidate_value,
                        "limit": (
                            f"≤ reference + {max_lan_regression_ms:.3f} ms"
                        ),
                        "passed": (
                            candidate_value
                            <= reference_value + max_lan_regression_ms
                        ),
                    }
                )
            reference_loss = finite_number(
                client_reference[profile].get("network_loss_percent"),
                f"Hestia reference.{profile}.network_loss_percent",
            )
            candidate_loss = finite_number(
                client_candidate[profile].get("network_loss_percent"),
                f"Hestia candidate.{profile}.network_loss_percent",
            )
            checks.append(
                {
                    "profile": profile,
                    "metric": "Hestia.network_loss_percent",
                    "reference": reference_loss,
                    "candidate": candidate_loss,
                    "limit": (
                        "≤ reference + "
                        f"{max_lan_client_loss_regression_points:.3f} points"
                    ),
                    "passed": (
                        candidate_loss
                        <= reference_loss
                        + max_lan_client_loss_regression_points
                    ),
                }
            )

        for profile in constrained_profiles:
            if profile not in comparable_client_profiles:
                continue
            reference_stage = client_reference[profile]["stages"][
                "receive_to_present"
            ]
            candidate_stage = client_candidate[profile]["stages"][
                "receive_to_present"
            ]
            for percentile in ("p95_ms", "p99_ms"):
                reference_value = finite_number(
                    reference_stage.get(percentile),
                    f"Hestia reference.{profile}.{percentile}",
                )
                candidate_value = finite_number(
                    candidate_stage.get(percentile),
                    f"Hestia candidate.{profile}.{percentile}",
                )
                limit = (
                    reference_value * improvement_factor
                    + constrained_tolerance_ms
                )
                checks.append(
                    {
                        "profile": profile,
                        "metric": f"Hestia.receive_to_present.{percentile}",
                        "reference": reference_value,
                        "candidate": candidate_value,
                        "limit": (
                            "≤ reference × "
                            f"{improvement_factor:.3f} + "
                            f"{constrained_tolerance_ms:.3f} ms"
                        ),
                        "passed": candidate_value <= limit,
                    }
                )
            reference_loss = finite_number(
                client_reference[profile].get("network_loss_percent"),
                f"Hestia reference.{profile}.network_loss_percent",
            )
            candidate_loss = finite_number(
                client_candidate[profile].get("network_loss_percent"),
                f"Hestia candidate.{profile}.network_loss_percent",
            )
            checks.append(
                {
                    "profile": profile,
                    "metric": "Hestia.network_loss_percent",
                    "reference": reference_loss,
                    "candidate": candidate_loss,
                    "limit": f"≤ {max_client_loss_percent:.3f}%",
                    "passed": candidate_loss <= max_client_loss_percent,
                }
            )

    failed_checks = [check for check in checks if not check["passed"]]
    return {
        "passed": not failures and not failed_checks,
        "failures": failures,
        "checks": checks,
        "min_windows": min_windows,
        "min_client_frames": min_client_frames,
        "client_required": require_client,
        "lan_profiles": lan_profiles,
        "constrained_profiles": constrained_profiles,
    }


def render_gate_markdown(result: dict[str, Any]) -> str:
    status = "PASS" if result["passed"] else "FAIL"
    lines = [
        "# Hermes H2 acceptance gate",
        "",
        f"**Result: {status}**",
        "",
        "This gate compares frame-count-weighted Hermes windows with exact "
        "nearest-rank percentiles from paired Hestia terminal frame traces. "
        "LAN profiles limit host/client p95/p99 regression, FPS loss, and client "
        "loss; constrained profiles require lower queue/presentation tails while "
        "enforcing the host send-queue budget.",
        "",
        "Minimum encode and network windows per side/profile: "
        f"{result['min_windows']}",
        "Minimum presented Hestia frames per side/profile: "
        f"{result['min_client_frames']}",
        "",
    ]
    if result["failures"]:
        lines.extend(["## Input failures", ""])
        lines.extend(f"- {failure}" for failure in result["failures"])
        lines.append("")

    lines.extend(
        [
            "## Checks",
            "",
            "| Profile | Metric | Reference | Candidate | Limit | Result |",
            "| --- | --- | ---: | ---: | --- | --- |",
        ]
    )
    for check in result["checks"]:
        check_status = "PASS" if check["passed"] else "FAIL"
        lines.append(
            f"| {check['profile']} | {check['metric']} | "
            f"{check['reference']:.3f} | {check['candidate']:.3f} | "
            f"{check['limit']} | {check_status} |"
        )
    lines.append("")
    return "\n".join(lines)


def gate(args: argparse.Namespace) -> int:
    reference_records = load_records(Path(path) for path in args.reference)
    candidate_records = load_records(Path(path) for path in args.candidate)
    client_reference_records = load_hestia_records(
        Path(path) for path in args.client_reference
    )
    client_candidate_records = load_hestia_records(
        Path(path) for path in args.client_candidate
    )
    if not reference_records:
        raise ValueError("no reference telemetry records found")
    if not candidate_records:
        raise ValueError("no candidate telemetry records found")
    if not client_reference_records:
        raise ValueError("no Hestia reference frame traces found")
    if not client_candidate_records:
        raise ValueError("no Hestia candidate frame traces found")

    result = evaluate_h2_gate(
        summarize_records(reference_records),
        summarize_records(candidate_records),
        client_reference=summarize_hestia_records(client_reference_records),
        client_candidate=summarize_hestia_records(client_candidate_records),
        require_client=True,
        lan_profiles=args.lan_profile or DEFAULT_LAN_PROFILES,
        constrained_profiles=(
            args.constrained_profile or DEFAULT_CONSTRAINED_PROFILES
        ),
        min_windows=args.min_windows,
        min_client_frames=args.min_client_frames,
        max_lan_regression_ms=args.max_lan_regression_ms,
        max_lan_fps_regression_percent=(
            args.max_lan_fps_regression_percent
        ),
        max_lan_client_loss_regression_points=(
            args.max_lan_client_loss_regression_points
        ),
        min_constrained_improvement_percent=(
            args.min_constrained_improvement_percent
        ),
        constrained_tolerance_ms=args.constrained_tolerance_ms,
        max_constrained_send_queue_p99_ms=(
            args.max_constrained_send_queue_p99_ms
        ),
        max_client_loss_percent=args.max_client_loss_percent,
    )
    markdown = render_gate_markdown(result)
    if args.output:
        mode = "w" if args.overwrite else "x"
        with Path(args.output).open(mode, encoding="utf-8") as destination:
            destination.write(markdown + "\n")
        print(f"H2 gate {'passed' if result['passed'] else 'failed'}: {args.output}")
    else:
        print(markdown)
    return 0 if result["passed"] else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    capture_parser = subparsers.add_parser(
        "capture", help="poll Hestia diagnostics into a deduplicated JSONL run"
    )
    capture_parser.add_argument("--url", required=True)
    capture_parser.add_argument("--output", required=True)
    capture_parser.add_argument("--profile", default="clean")
    capture_parser.add_argument("--duration", type=float, default=60.0)
    capture_parser.add_argument("--interval", type=float, default=0.25)
    capture_parser.add_argument("--timeout", type=float, default=5.0)
    capture_parser.add_argument("--cert")
    capture_parser.add_argument("--key")
    capture_parser.add_argument("--ca")
    capture_parser.add_argument(
        "--hestia-identity",
        action="store_true",
        help="load the client certificate/key directly from Hestia QSettings",
    )
    capture_parser.add_argument(
        "--hestia-settings",
        help="explicit Hestia INI file (defaults to the normal QSettings path)",
    )
    capture_parser.add_argument("--insecure", action="store_true")
    capture_parser.add_argument("--overwrite", action="store_true")
    capture_parser.set_defaults(handler=capture)

    report_parser = subparsers.add_parser(
        "report", help="create a Markdown baseline from one or more JSONL runs"
    )
    report_parser.add_argument("inputs", nargs="+")
    report_parser.add_argument("--output")
    report_parser.add_argument("--overwrite", action="store_true")
    report_parser.set_defaults(handler=report)

    import_parser = subparsers.add_parser(
        "import-hestia",
        help="extract structured Hestia frame traces from one or more client logs",
    )
    import_parser.add_argument("inputs", nargs="+")
    import_parser.add_argument("--profile", required=True)
    import_parser.add_argument("--output", required=True)
    import_parser.add_argument("--overwrite", action="store_true")
    import_parser.set_defaults(handler=import_hestia)

    gate_parser = subparsers.add_parser(
        "gate",
        help="compare reference and candidate runs against the H2 acceptance gate",
    )
    gate_parser.add_argument("--reference", nargs="+", required=True)
    gate_parser.add_argument("--candidate", nargs="+", required=True)
    gate_parser.add_argument("--client-reference", nargs="+", required=True)
    gate_parser.add_argument("--client-candidate", nargs="+", required=True)
    gate_parser.add_argument(
        "--lan-profile",
        action="append",
        help="LAN profile to check (repeatable; defaults to clean and lan)",
    )
    gate_parser.add_argument(
        "--constrained-profile",
        action="append",
        help="limited-link profile to check (repeatable; defaults to all profiles)",
    )
    gate_parser.add_argument("--min-windows", type=int, default=30)
    gate_parser.add_argument("--min-client-frames", type=int, default=300)
    gate_parser.add_argument("--max-lan-regression-ms", type=float, default=2.0)
    gate_parser.add_argument(
        "--max-lan-fps-regression-percent", type=float, default=2.0
    )
    gate_parser.add_argument(
        "--max-lan-client-loss-regression-points", type=float, default=0.5
    )
    gate_parser.add_argument(
        "--min-constrained-improvement-percent", type=float, default=1.0
    )
    gate_parser.add_argument(
        "--constrained-tolerance-ms", type=float, default=0.0
    )
    gate_parser.add_argument(
        "--max-constrained-send-queue-p99-ms", type=float, default=100.0
    )
    gate_parser.add_argument(
        "--max-client-loss-percent", type=float, default=10.0
    )
    gate_parser.add_argument("--output")
    gate_parser.add_argument("--overwrite", action="store_true")
    gate_parser.set_defaults(handler=gate)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.handler(args)
    except (OSError, ValueError) as error:
        print(f"{parser.prog}: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
