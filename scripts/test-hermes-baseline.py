#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import io
import json
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path


SCRIPT_PATH = Path(__file__).with_name("hermes-baseline.py")
SPEC = importlib.util.spec_from_file_location("hermes_baseline", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
BASELINE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BASELINE)


def latency(value: float) -> dict[str, float]:
    return {
        "mean_ms": value,
        "p50_ms": value,
        "p95_ms": value * 2,
        "p99_ms": value * 3,
    }


def record(
    sequence: int,
    frames: int,
    value: float,
    *,
    profile: str = "wifi",
    run_id: str = "run-a",
    fps: float = 60.0,
) -> dict:
    network = {
        "window_sequence": sequence,
        "window_duration_ms": 1000.0,
        "window_frames": frames,
        "wire_bitrate_kbps": value * 1000,
        "data_shards": frames * 10,
        "fec_shards": frames * 2,
    }
    for stage in BASELINE.NETWORK_STAGES:
        network[stage] = latency(value)

    return {
        "schema": 1,
        "run_id": run_id,
        "profile": profile,
        "pipeline": {
            "window_sequence": sequence,
            "window_duration_ms": 1000.0,
            "window_frames": frames,
            "fps": fps,
            "bitrate_kbps": value * 500,
            "encode_ms": value,
            "encode_p50_ms": value,
            "encode_p95_ms": value * 2,
            "encode_p99_ms": value * 3,
            "capture_to_encode_ms": value,
            "capture_to_encode_p50_ms": value,
            "capture_to_encode_p95_ms": value * 2,
            "capture_to_encode_p99_ms": value * 3,
            "network": network,
        },
    }


def hestia_trace(frame_id: int, total_ms: float) -> dict:
    receive_us = 1_000_000 + frame_id * 100_000
    return {
        "schema": 1,
        "frame_id": frame_id,
        "receive_us": receive_us,
        "assemble_us": receive_us + 1_000,
        "decode_us": receive_us + 2_000,
        "present_start_us": receive_us + 3_000,
        "terminal_us": receive_us + int(total_ms * 1_000),
        "outcome": "presented",
        "reason": "presented",
    }


def hestia_record(
    frame_id: int,
    total_ms: float,
    *,
    profile: str,
    run_id: str,
) -> dict:
    return {
        "schema": 1,
        "kind": "hestia_frame_trace",
        "run_id": run_id,
        "profile": profile,
        "trace": hestia_trace(frame_id, total_ms),
    }


class BaselineReportTests(unittest.TestCase):
    def test_extract_pipeline_rejects_unscoped_shape(self) -> None:
        with self.assertRaises(ValueError):
            BASELINE.extract_pipeline({"runtime": {"pipeline": []}})

    def test_reads_hestia_identity_from_explicit_qsettings_file(self) -> None:
        try:
            from PyQt6.QtCore import QByteArray, QSettings
        except ImportError:
            try:
                from PyQt5.QtCore import QByteArray, QSettings
            except ImportError:
                self.skipTest("PyQt5 and PyQt6 are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            settings_path = Path(directory) / "Moonlight.conf"
            ini_format = (
                QSettings.Format.IniFormat
                if hasattr(QSettings, "Format")
                else QSettings.IniFormat
            )
            settings = QSettings(
                str(settings_path), ini_format
            )
            settings.setValue(
                "certificate", QByteArray(b"test-certificate")
            )
            settings.setValue("key", QByteArray(b"test-private-key"))
            settings.sync()

            certificate, private_key = BASELINE.read_hestia_identity(
                str(settings_path)
            )

        self.assertEqual(certificate, b"test-certificate")
        self.assertEqual(private_key, b"test-private-key")

    def test_summary_deduplicates_and_weights_windows(self) -> None:
        first = record(1, 10, 1.0)
        duplicate = json.loads(json.dumps(first))
        second = record(2, 30, 3.0)

        summary = BASELINE.summarize_records([first, duplicate, second])["wifi"]
        encode = summary["stages"]["encode"]
        send_queue = summary["stages"]["send_queue"]

        self.assertEqual(summary["encode_windows"], 2)
        self.assertEqual(summary["network_windows"], 2)
        self.assertEqual(encode["frames"], 40)
        self.assertAlmostEqual(encode["mean_ms"], 2.5)
        self.assertAlmostEqual(encode["p99_ms"], 7.5)
        self.assertAlmostEqual(encode["worst_p99_ms"], 9.0)
        self.assertAlmostEqual(send_queue["mean_ms"], 2.5)
        self.assertAlmostEqual(summary["fec_overhead_percent"], 20.0)

    def test_jsonl_loader_and_markdown_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "run.jsonl"
            path.write_text(
                json.dumps(record(1, 60, 2.0)) + "\n", encoding="utf-8"
            )
            records = BASELINE.load_records([path])
            markdown = BASELINE.render_markdown(
                BASELINE.summarize_records(records)
            )

        self.assertIn("# Hermes H0 baseline report", markdown)
        self.assertIn("## wifi", markdown)
        self.assertIn("| encode |", markdown)
        self.assertIn("FEC shard overhead: 20.00%", markdown)

    def test_report_refuses_to_overwrite_without_opt_in(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / "run.jsonl"
            output_path = Path(directory) / "report.md"
            input_path.write_text(
                json.dumps(record(1, 60, 2.0)) + "\n", encoding="utf-8"
            )
            output_path.write_text("keep me", encoding="utf-8")

            with redirect_stderr(io.StringIO()):
                result = BASELINE.main(
                    ["report", str(input_path), "--output", str(output_path)]
                )

            self.assertEqual(result, 2)
            self.assertEqual(output_path.read_text(encoding="utf-8"), "keep me")

    def gate_summary(
        self,
        *,
        lan_value: float,
        constrained_value: float,
        fps: float = 60.0,
        windows: int = 1,
        run_id: str,
    ) -> dict:
        records = []
        for profile in BASELINE.DEFAULT_LAN_PROFILES:
            for sequence in range(1, windows + 1):
                records.append(
                    record(
                        sequence,
                        60,
                        lan_value,
                        profile=profile,
                        run_id=run_id,
                        fps=fps,
                    )
                )
        for profile in BASELINE.DEFAULT_CONSTRAINED_PROFILES:
            for sequence in range(1, windows + 1):
                records.append(
                    record(
                        sequence,
                        60,
                        constrained_value,
                        profile=profile,
                        run_id=run_id,
                        fps=fps,
                    )
                )
        return BASELINE.summarize_records(records)

    def client_gate_summary(
        self,
        *,
        lan_value: float,
        constrained_value: float,
        frames: int = 2,
        run_id: str,
    ) -> dict:
        records = []
        for profile in BASELINE.DEFAULT_LAN_PROFILES:
            for frame_id in range(1, frames + 1):
                records.append(
                    hestia_record(
                        frame_id,
                        lan_value,
                        profile=profile,
                        run_id=run_id,
                    )
                )
        for profile in BASELINE.DEFAULT_CONSTRAINED_PROFILES:
            for frame_id in range(1, frames + 1):
                records.append(
                    hestia_record(
                        frame_id,
                        constrained_value,
                        profile=profile,
                        run_id=run_id,
                    )
                )
        return BASELINE.summarize_hestia_records(records)

    def test_h2_gate_passes_lan_budget_and_constrained_improvement(self) -> None:
        reference = self.gate_summary(
            lan_value=2.0,
            constrained_value=4.0,
            run_id="reference",
        )
        candidate = self.gate_summary(
            lan_value=2.5,
            constrained_value=3.0,
            fps=59.0,
            run_id="candidate",
        )

        result = BASELINE.evaluate_h2_gate(
            reference, candidate, min_windows=1
        )

        self.assertTrue(result["passed"])
        self.assertTrue(result["checks"])
        self.assertIn("**Result: PASS**", BASELINE.render_gate_markdown(result))

    def test_h2_gate_fails_lan_p99_regression(self) -> None:
        reference = self.gate_summary(
            lan_value=1.0,
            constrained_value=4.0,
            run_id="reference",
        )
        candidate = self.gate_summary(
            lan_value=2.0,
            constrained_value=3.0,
            run_id="candidate",
        )

        result = BASELINE.evaluate_h2_gate(
            reference, candidate, min_windows=1
        )

        self.assertFalse(result["passed"])
        self.assertTrue(
            any(
                not check["passed"]
                and check["metric"] == "capture_to_last_send.p99_ms"
                for check in result["checks"]
            )
        )

    def test_h2_gate_fails_constrained_regression(self) -> None:
        reference = self.gate_summary(
            lan_value=1.0,
            constrained_value=2.0,
            run_id="reference",
        )
        candidate = self.gate_summary(
            lan_value=1.0,
            constrained_value=3.0,
            run_id="candidate",
        )

        result = BASELINE.evaluate_h2_gate(
            reference, candidate, min_windows=1
        )

        self.assertFalse(result["passed"])
        self.assertTrue(
            any(
                not check["passed"] and check["profile"] == "wifi"
                for check in result["checks"]
            )
        )

    def test_h2_gate_rejects_missing_or_short_profiles(self) -> None:
        reference = self.gate_summary(
            lan_value=1.0,
            constrained_value=2.0,
            run_id="reference",
        )
        candidate = self.gate_summary(
            lan_value=1.0,
            constrained_value=1.0,
            run_id="candidate",
        )
        del candidate["wan"]

        result = BASELINE.evaluate_h2_gate(
            reference, candidate, min_windows=2
        )

        self.assertFalse(result["passed"])
        self.assertTrue(
            any("missing profile 'wan'" in failure for failure in result["failures"])
        )
        self.assertTrue(
            any("requires at least 2" in failure for failure in result["failures"])
        )

    def test_hestia_import_and_exact_trace_summary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "hestia.log"
            output_path = Path(directory) / "hestia.jsonl"
            first = hestia_trace(10, 12.0)
            second = hestia_trace(11, 20.0)
            log_path.write_text(
                "unrelated line\n"
                f"INFO: {BASELINE.HESTIA_TRACE_MARKER}{json.dumps(first)}\n"
                f"{json.dumps(second)}\n",
                encoding="utf-8",
            )

            result = BASELINE.main(
                [
                    "import-hestia",
                    str(log_path),
                    "--profile",
                    "wifi",
                    "--output",
                    str(output_path),
                ]
            )
            summary = BASELINE.summarize_hestia_records(
                BASELINE.load_hestia_records([output_path])
            )["wifi"]

        self.assertEqual(result, 0)
        self.assertEqual(summary["presented_frames"], 2)
        self.assertEqual(summary["network_missing_frames"], 0)
        self.assertAlmostEqual(
            summary["stages"]["receive_to_present"]["p50_ms"], 12.0
        )
        self.assertAlmostEqual(
            summary["stages"]["receive_to_present"]["p99_ms"], 20.0
        )

    def test_h2_gate_uses_paired_hestia_traces(self) -> None:
        reference = self.gate_summary(
            lan_value=2.0,
            constrained_value=4.0,
            run_id="reference",
        )
        candidate = self.gate_summary(
            lan_value=2.2,
            constrained_value=3.0,
            run_id="candidate",
        )
        client_reference = self.client_gate_summary(
            lan_value=10.0,
            constrained_value=30.0,
            run_id="client-reference",
        )
        client_candidate = self.client_gate_summary(
            lan_value=11.0,
            constrained_value=20.0,
            run_id="client-candidate",
        )

        result = BASELINE.evaluate_h2_gate(
            reference,
            candidate,
            client_reference=client_reference,
            client_candidate=client_candidate,
            require_client=True,
            min_windows=1,
            min_client_frames=2,
        )

        self.assertTrue(result["passed"])
        self.assertTrue(
            any(
                check["metric"] == "Hestia.receive_to_present.p99_ms"
                for check in result["checks"]
            )
        )

    def test_h2_gate_requires_client_traces_when_requested(self) -> None:
        reference = self.gate_summary(
            lan_value=1.0,
            constrained_value=2.0,
            run_id="reference",
        )
        candidate = self.gate_summary(
            lan_value=1.0,
            constrained_value=1.0,
            run_id="candidate",
        )

        result = BASELINE.evaluate_h2_gate(
            reference,
            candidate,
            require_client=True,
            min_windows=1,
        )

        self.assertFalse(result["passed"])
        self.assertTrue(
            any("Hestia" in failure for failure in result["failures"])
        )

    def test_h2_gate_cli_writes_passing_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            host_reference_path = root / "host-reference.jsonl"
            host_candidate_path = root / "host-candidate.jsonl"
            client_reference_path = root / "client-reference.jsonl"
            client_candidate_path = root / "client-candidate.jsonl"
            output_path = root / "gate.md"

            host_reference = []
            host_candidate = []
            client_reference = []
            client_candidate = []
            profiles = (
                *BASELINE.DEFAULT_LAN_PROFILES,
                *BASELINE.DEFAULT_CONSTRAINED_PROFILES,
            )
            for profile in profiles:
                constrained = profile in BASELINE.DEFAULT_CONSTRAINED_PROFILES
                host_reference.append(
                    record(
                        1,
                        60,
                        4.0 if constrained else 2.0,
                        profile=profile,
                        run_id="host-reference",
                    )
                )
                host_candidate.append(
                    record(
                        1,
                        60,
                        3.0 if constrained else 2.2,
                        profile=profile,
                        run_id="host-candidate",
                    )
                )
                for frame_id in (1, 2):
                    client_reference.append(
                        hestia_record(
                            frame_id,
                            30.0 if constrained else 10.0,
                            profile=profile,
                            run_id="client-reference",
                        )
                    )
                    client_candidate.append(
                        hestia_record(
                            frame_id,
                            20.0 if constrained else 11.0,
                            profile=profile,
                            run_id="client-candidate",
                        )
                    )

            for path, records in (
                (host_reference_path, host_reference),
                (host_candidate_path, host_candidate),
                (client_reference_path, client_reference),
                (client_candidate_path, client_candidate),
            ):
                path.write_text(
                    "".join(json.dumps(item) + "\n" for item in records),
                    encoding="utf-8",
                )

            result = BASELINE.main(
                [
                    "gate",
                    "--reference",
                    str(host_reference_path),
                    "--candidate",
                    str(host_candidate_path),
                    "--client-reference",
                    str(client_reference_path),
                    "--client-candidate",
                    str(client_candidate_path),
                    "--min-windows",
                    "1",
                    "--min-client-frames",
                    "2",
                    "--output",
                    str(output_path),
                ]
            )

            self.assertEqual(result, 0)
            self.assertIn(
                "**Result: PASS**", output_path.read_text(encoding="utf-8")
            )

    def test_hestia_summary_detects_frame_id_gaps(self) -> None:
        summary = BASELINE.summarize_hestia_records(
            [
                hestia_record(1, 10.0, profile="wifi", run_id="run"),
                hestia_record(3, 10.0, profile="wifi", run_id="run"),
            ]
        )["wifi"]

        self.assertEqual(summary["network_missing_frames"], 1)
        self.assertAlmostEqual(summary["network_loss_percent"], 100.0 / 3.0)


if __name__ == "__main__":
    unittest.main()
