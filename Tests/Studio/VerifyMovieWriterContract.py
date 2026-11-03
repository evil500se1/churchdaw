#!/usr/bin/env python3
"""Independently verify media emitted by MovieWriterContractTests.h.

Requires Python 3.9+, NumPy, ffprobe and ffmpeg. No pytest is needed.

    python3 Tests/Studio/VerifyMovieWriterContract.py /path/movie-writer-contract.json
    python3 Tests/Studio/VerifyMovieWriterContract.py copied.json --media-root ./windows-media

FFPROBE/FFMPEG may name executable paths. Otherwise PATH, ~/miniforge3/bin and
/opt/miniforge3/bin are checked. --media-root remaps absolute Windows paths by
basename; ambiguous names fail. The JSON report defaults to the manifest's
directory, named movie-writer-verification.json; override with --report.

Video is decoded one RGB frame at a time, with no rotation, frame-rate conversion
or resizing. Only tiny probe patches and seven frame-code bits are retained.
Audio is decoded as float PCM at its native sample rate/channels; ffmpeg applies
AAC skip-sample/edit-list information. Sine/cosine least squares separates the
three tones, including short onset/offset windows. Metadata alone cannot pass.
"""

from __future__ import annotations

import argparse
from fractions import Fraction
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


class VerificationError(RuntimeError):
    pass


def find_tool(name: str) -> str:
    explicit = os.environ.get(name.upper())
    candidates = ([explicit] if explicit else []) + [
        shutil.which(name),
        str(Path.home() / "miniforge3" / "bin" / name),
        str(Path("/opt/miniforge3/bin") / name),
        str(Path("/opt/homebrew/bin") / name),
    ]
    # An explicitly supplied but invalid path must not silently choose another
    # encoder/prober from PATH.
    if explicit:
        candidates = [explicit, shutil.which(explicit)]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(Path(candidate).resolve())
    raise VerificationError(f"Cannot find {name}; put it on PATH or set {name.upper()}.")


def command_json(command: list[str]) -> dict:
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=90)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise VerificationError(str(error)) from error
    if completed.returncode:
        raise VerificationError(completed.stderr[-4000:].strip() or f"Command exited {completed.returncode}")
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise VerificationError(f"Invalid ffprobe JSON: {error}") from error


def finite_number(value, fallback=None):
    try:
        result = float(value)
        return result if math.isfinite(result) else fallback
    except (TypeError, ValueError):
        return fallback


def rate(value) -> float | None:
    try:
        return float(Fraction(value))
    except (ValueError, TypeError, ZeroDivisionError):
        return None


def basename(value: str) -> str:
    # JUCE manifests can contain either slash convention after copying from a
    # Windows host. pathlib on macOS does not interpret backslashes as separators.
    return value.replace("\\", "/").rsplit("/", 1)[-1]


class Verifier:
    def __init__(self, manifest: dict, media_root: Path | None, ffprobe: str, ffmpeg: str, np):
        self.manifest = manifest
        self.media_root = media_root
        self.ffprobe = ffprobe
        self.ffmpeg = ffmpeg
        self.np = np
        self.checks: list[dict] = []
        self.outputs: list[dict] = []

    def check(self, case: str, name: str, ok: bool, expected=None, observed=None):
        entry = {"case": case, "check": name, "passed": bool(ok)}
        if expected is not None:
            entry["expected"] = expected
        if observed is not None:
            entry["observed"] = observed
        self.checks.append(entry)
        if not ok:
            print(f"FAIL {case}: {name}; observed={observed!r}, expected={expected!r}", file=sys.stderr)

    def near(self, case, name, actual, expected, tolerance):
        self.check(case, name, actual is not None and abs(actual - expected) <= tolerance,
                   {"value": expected, "absolute_tolerance": tolerance}, actual)

    def resolve(self, declared_path: str) -> Path:
        if self.media_root is None:
            path = Path(declared_path)
            if path.is_file():
                return path
            raise VerificationError(f"Missing media {declared_path}; copied Windows files need --media-root.")
        name = basename(declared_path)
        direct = self.media_root / name
        if direct.is_file():
            return direct
        matches = [p for p in self.media_root.rglob("*") if p.is_file() and p.name == name]
        if len(matches) != 1:
            raise VerificationError(f"Expected one file named {name!r} under {self.media_root}, found {len(matches)}.")
        return matches[0]

    def probe(self, path: Path) -> dict:
        return command_json([self.ffprobe, "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)])

    def verify_video(self, path: Path, stream: dict, expected: dict, case: str) -> dict:
        width, height = int(expected["width"]), int(expected["height"])
        count, fps = int(expected["frame_count"]), float(expected["fps"])
        self.check(case, "video codec", stream.get("codec_name") == expected["codec"], expected["codec"], stream.get("codec_name"))
        dimensions_ok = stream.get("width") == width and stream.get("height") == height
        self.check(case, "video dimensions", dimensions_ok, [width, height], [stream.get("width"), stream.get("height")])
        self.near(case, "average frame rate", rate(stream.get("avg_frame_rate")), fps, 0.001)
        rotation = finite_number(stream.get("tags", {}).get("rotate"), 0.0)
        for side_data in stream.get("side_data_list", []):
            if "rotation" in side_data:
                rotation = finite_number(side_data["rotation"])
        normal_rotation = None if rotation is None else (rotation + 180) % 360 - 180
        self.near(case, "rotation metadata", normal_rotation, float(expected["rotation_degrees"]), 0.01)

        frame_info = command_json([
            self.ffprobe, "-v", "error", "-select_streams", "v:0", "-show_frames",
            "-show_entries", "frame=pts_time,best_effort_timestamp_time,pkt_duration_time,duration_time",
            "-of", "json", str(path),
        ]).get("frames", [])
        self.check(case, "ffprobe decoded frame count", len(frame_info) == count, count, len(frame_info))
        pts = [finite_number(frame.get("pts_time", frame.get("best_effort_timestamp_time"))) for frame in frame_info]
        errors = [abs(timestamp - index / fps) for index, timestamp in enumerate(pts) if timestamp is not None]
        valid_pts = len(errors) == len(pts) and len(pts) == count
        tolerance = float(expected["frame_timestamp_tolerance_seconds"])
        self.check(case, "presentation timestamps", valid_pts and max(errors, default=math.inf) <= tolerance,
                   {"sequence": "index / fps", "absolute_tolerance_seconds": tolerance},
                   {"first": pts[0] if pts else None, "last": pts[-1] if pts else None,
                    "maximum_error_seconds": max(errors) if errors else None})
        last_duration = finite_number(frame_info[-1].get("duration_time", frame_info[-1].get("pkt_duration_time")), 1 / fps) if frame_info else 0
        timeline_end = pts[-1] + last_duration if pts and pts[-1] is not None else None
        self.near(case, "decoded video timeline end", timeline_end, float(expected["duration_seconds"]), float(expected["duration_tolerance_seconds"]))
        if stream.get("duration") is not None:
            self.near(case, "video stream duration", finite_number(stream["duration"]), float(expected["duration_seconds"]), float(expected["duration_tolerance_seconds"]))
        if not dimensions_ok:
            raise VerificationError("Cannot evaluate fixed-coordinate probes on unexpected video dimensions.")

        # rawvideo has no framing; read_exact loops because pipe reads may be
        # short. At most one 1920x1080 RGB frame (~6 MB) is resident at a time.
        frame_bytes = width * height * 3
        codes, colour_failures, bit_failures = [], [], []
        maximum_colour_error = 0.0
        extra_frames = False
        with tempfile.TemporaryFile() as errors_file:
            process = subprocess.Popen([
                self.ffmpeg, "-v", "error", "-nostdin", "-noautorotate", "-i", str(path),
                "-map", "0:v:0", "-an", "-fps_mode", "passthrough", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1",
            ], stdout=subprocess.PIPE, stderr=errors_file)
            try:
                assert process.stdout is not None
                while True:
                    data = bytearray(frame_bytes)
                    view = memoryview(data)
                    received = 0
                    while received < frame_bytes:
                        part = process.stdout.readinto(view[received:])
                        if not part:
                            break
                        received += part
                    if received == 0:
                        break
                    if received != frame_bytes:
                        raise VerificationError(f"Truncated RGB frame ({received}/{frame_bytes} bytes).")
                    rgb = self.np.frombuffer(data, dtype=self.np.uint8).reshape(height, width, 3)
                    index = len(codes)
                    for probe in expected["orientation_colour_probes"]:
                        x = min(width - 1, max(0, round(probe["normalised_xy"][0] * width)))
                        y = min(height - 1, max(0, round(probe["normalised_xy"][1] * height)))
                        observed = rgb[max(0, y - 3):y + 4, max(0, x - 3):x + 4].mean(axis=(0, 1))
                        error = float(self.np.max(self.np.abs(observed - self.np.asarray(probe["rgb"]))))
                        maximum_colour_error = max(maximum_colour_error, error)
                        if error > float(expected["rgb_absolute_tolerance"]) and len(colour_failures) < 8:
                            colour_failures.append({"frame": index, "xy": [x, y], "rgb": observed.tolist(), "maximum_error": error})
                    code = 0
                    threshold = float(expected["frame_number_black_white_threshold"])
                    for bit, centre in enumerate(expected["frame_number_bit_centres_pixels_lsb_first"]):
                        x, y = (round(coordinate) for coordinate in centre)
                        if not (2 <= x < width - 2 and 2 <= y < height - 2):
                            raise VerificationError(f"Frame-code probe outside image: {centre}.")
                        intensity = float(rgb[y - 2:y + 3, x - 2:x + 3].mean())
                        one = intensity >= threshold
                        code |= int(one) << bit
                        if abs(intensity - (255 if one else 0)) > float(expected["rgb_absolute_tolerance"]) and len(bit_failures) < 8:
                            bit_failures.append({"frame": index, "bit": bit, "intensity": intensity})
                    codes.append(code)
                    if len(codes) > count:
                        extra_frames = True
                        process.terminate()
                        break
                return_code = process.wait(timeout=30)
                errors_file.seek(0)
                error_text = errors_file.read().decode("utf-8", "replace")[-4000:]
                if return_code and not extra_frames:
                    raise VerificationError(f"RGB decode exited {return_code}: {error_text}")
            finally:
                if process.stdout:
                    process.stdout.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()
        self.check(case, "streamed RGB frame count", len(codes) == count, count, len(codes))
        self.check(case, "quadrant orientation and RGB order on every frame", not colour_failures,
                   {"absolute_rgb_tolerance": expected["rgb_absolute_tolerance"]},
                   {"maximum_error": maximum_colour_error, "examples": colour_failures})
        self.check(case, "frame-code black/white integrity", not bit_failures, "black or white within colour tolerance", bit_failures)
        self.check(case, "decoded frame numbers and first/last frames", codes == list(range(count)),
                   f"0..{count - 1}, once each", codes)
        return {"frame_count": len(codes), "first_code": codes[0] if codes else None,
                "last_code": codes[-1] if codes else None, "timeline_end_seconds": timeline_end}

    def fit_tones(self, samples, sample_rate: int, frequencies: list[float]):
        np = self.np
        times = np.arange(len(samples), dtype=np.float64) / sample_rate
        basis = [np.ones(len(samples))]
        for frequency in frequencies:
            basis.extend((np.sin(2 * np.pi * frequency * times), np.cos(2 * np.pi * frequency * times)))
        coefficients = np.linalg.lstsq(np.column_stack(basis), samples, rcond=None)[0]
        return {frequency: np.hypot(coefficients[1 + 2 * index], coefficients[2 + 2 * index])
                for index, frequency in enumerate(frequencies)}

    def measured_frequency(self, signal, sample_rate: int, target: float) -> float:
        np = self.np
        size = 1 << (max(2, len(signal) * 2) - 1).bit_length()
        spectrum = np.abs(np.fft.rfft(signal * np.hanning(len(signal)), n=size))
        frequencies = np.fft.rfftfreq(size, 1 / sample_rate)
        candidates = np.flatnonzero((frequencies >= target - 25) & (frequencies <= target + 25))
        if not len(candidates):
            return 0.0
        peak = int(candidates[np.argmax(spectrum[candidates])])
        fraction = 0.0
        if 0 < peak < len(spectrum) - 1:
            a, b, c = np.log(np.maximum(spectrum[peak - 1:peak + 2], 1e-30))
            denominator = a - 2 * b + c
            if abs(denominator) > 1e-15:
                fraction = float(np.clip(0.5 * (a - c) / denominator, -0.5, 0.5))
        return (peak + fraction) * sample_rate / size

    def verify_audio(self, path: Path, stream: dict, expected: dict, video: dict, case: str) -> dict:
        np = self.np
        channels = int(stream.get("channels", 0))
        sample_rate = int(stream.get("sample_rate", 0))
        self.check(case, "audio codec", stream.get("codec_name") == expected["codec"], expected["codec"], stream.get("codec_name"))
        self.check(case, "audio channel count", channels == int(expected["channels"]), expected["channels"], channels)
        if channels != 2 or not (8000 <= sample_rate <= 192000):
            raise VerificationError("Cannot evaluate stereo audio at an invalid channel count or sample rate.")
        duration = float(video["duration_seconds"])
        # Bound PCM memory even if a broken exporter made a very long stream.
        # The extra half-second still exposes excessive duration/padding.
        decoded = subprocess.run([
            self.ffmpeg, "-v", "error", "-nostdin", "-i", str(path), "-map", "0:a:0",
            "-vn", "-t", str(duration + 0.5), "-c:a", "pcm_f32le", "-f", "f32le", "pipe:1",
        ], capture_output=True, timeout=90)
        if decoded.returncode:
            raise VerificationError(f"PCM decode failed: {decoded.stderr.decode('utf-8', 'replace')[-4000:]}")
        if len(decoded.stdout) % (4 * channels):
            raise VerificationError("PCM decode returned an incomplete sample frame.")
        samples = np.frombuffer(decoded.stdout, dtype="<f4").reshape(-1, channels)
        if len(samples) == 0 or not np.isfinite(samples).all():
            raise VerificationError("Audio is empty or contains NaN/infinity.")
        start = finite_number(stream.get("start_time"), 0.0)
        end = start + len(samples) / sample_rate
        duration_tolerance = float(video["duration_tolerance_seconds"])
        self.near(case, "decoded audio start", start, 0.0, duration_tolerance)
        self.near(case, "decoded audio/video end alignment", end, duration, duration_tolerance)
        if stream.get("duration") is not None:
            self.near(case, "audio stream end", start + float(stream["duration"]), duration, duration_tolerance)
        peak = float(np.max(np.abs(samples)))
        peak_limit = float(expected["maximum_linear_peak_before_encoding"]) + float(expected["aac_decoded_peak_tolerance"])
        self.check(case, "decoded clipping/protection peak bound", peak <= peak_limit, {"maximum": peak_limit}, peak)

        sources = expected["sources"]
        frequencies = sorted({float(source[key]) for source in sources for key in ("left_frequency_hz", "right_frequency_hz")})
        windows = [("DAW-only", interval) for interval in expected["daw_only_analysis_windows_seconds"]]
        windows.append(("DAW+mic", expected["daw_plus_mic_analysis_window_seconds"]))
        observed_windows = []
        for label, interval in windows:
            begin, finish = map(float, interval)
            first, last = round((begin - start) * sample_rate), round((finish - start) * sample_rate)
            if first < 0 or last > len(samples) or last <= first:
                self.check(case, f"{label} analysis window available", False, interval, [start, end])
                continue
            segment = samples[first:last]
            measured = self.fit_tones(segment, sample_rate, frequencies)
            observed_windows.append({"interval_seconds": interval, "amplitudes_by_frequency_lr":
                                     {str(frequency): amplitude.tolist() for frequency, amplitude in measured.items()}})
            for channel, frequency_key in enumerate(("left_frequency_hz", "right_frequency_hz")):
                active = [source for source in sources if float(source["offset_seconds"]) <= begin
                          and float(source["audible_until_seconds"]) >= finish]
                target = {frequency: sum(float(source["expected_sine_peak_per_routed_channel"])
                                        for source in active if float(source[frequency_key]) == frequency)
                          for frequency in frequencies}
                reference = max(target.values(), default=0.0)
                for frequency in frequencies:
                    name = f"{label} {begin:g}-{finish:g}s channel {channel} {frequency:g}Hz"
                    amplitude = float(measured[frequency][channel])
                    if target[frequency] > 0:
                        self.near(case, name + " amplitude/gain", amplitude, target[frequency],
                                  target[frequency] * float(expected["amplitude_relative_tolerance"]))
                        detected = self.measured_frequency(segment[:, channel], sample_rate, frequency)
                        self.near(case, name + " frequency", detected, frequency, float(expected["frequency_tolerance_hz"]))
                    else:
                        limit = reference * float(expected["excluded_frequency_max_relative_amplitude"])
                        self.check(case, name + " absent/correct routing", amplitude <= limit, {"maximum_amplitude": limit}, amplitude)

        # Estimate onset/offset globally, not just silence in two distant
        # windows. 20ms least-squares windows with 10ms hops reject the DAW tones
        # independently of microphone phase or non-integral periods.
        window_size, hop = round(sample_rate * 0.020), round(sample_rate * 0.010)
        if len(samples) < window_size:
            raise VerificationError("Decoded audio is too short for a 20ms timing measurement.")
        times = np.arange(window_size, dtype=np.float64) / sample_rate
        basis = [np.ones(window_size)]
        for frequency in frequencies:
            basis.extend((np.sin(2 * np.pi * frequency * times), np.cos(2 * np.pi * frequency * times)))
        inverse = np.linalg.pinv(np.column_stack(basis))
        centres, envelopes = [], []
        for position in range(0, len(samples) - window_size + 1, hop):
            coefficients = inverse @ samples[position:position + window_size]
            centres.append(start + (position + window_size / 2) / sample_rate)
            envelopes.append(np.stack([np.hypot(coefficients[1 + 2 * index], coefficients[2 + 2 * index])
                                       for index in range(len(frequencies))]))
        envelope = np.asarray(envelopes)
        onset_results = []
        for source in sources:
            if source["role"] != "microphone":
                continue
            for channel, key in enumerate(("left_frequency_hz", "right_frequency_hz")):
                frequency = float(source[key])
                selected = envelope[:, frequencies.index(frequency), channel]
                threshold = float(source["expected_sine_peak_per_routed_channel"]) * 0.5
                active = np.flatnonzero(selected >= threshold)
                onset = centres[int(active[0])] if len(active) else None
                offset = centres[int(active[-1])] if len(active) else None
                tolerance = float(expected["onset_offset_tolerance_seconds"])
                self.near(case, f"microphone channel {channel} onset", onset, float(source["offset_seconds"]), tolerance)
                self.near(case, f"microphone channel {channel} end", offset, float(source["audible_until_seconds"]), tolerance)
                onset_results.append({"channel": channel, "frequency_hz": frequency, "onset_seconds": onset,
                                      "end_seconds": offset, "measurement_threshold_amplitude": threshold})
        return {"sample_rate": sample_rate, "channels": channels, "decoded_samples": len(samples),
                "start_seconds": start, "end_seconds": end, "peak": peak,
                "analysis_windows": observed_windows, "microphone_timing": onset_results}

    def run(self):
        self.check("manifest", "schema", self.manifest.get("schema_version") == 1, 1, self.manifest.get("schema_version"))
        self.check("manifest", "native backend supported", self.manifest.get("supported") is True, True, self.manifest.get("supported"))
        self.check("manifest", "native API contract failures", self.manifest.get("native_failures") == 0, 0, self.manifest.get("native_failures"))
        expected_cases = {"raw-720p", "video-only-720p", "offset-gain-mix-720p", "raw-1080p", "protected-mix-1080p"}
        cases = [output.get("case") for output in self.manifest.get("outputs", [])]
        self.check("manifest", "all required outputs present once", set(cases) == expected_cases and len(cases) == len(expected_cases), sorted(expected_cases), cases)
        for output in self.manifest.get("outputs", []):
            case = str(output.get("case", "unnamed"))
            observation = {"case": case, "declared_path": output.get("path")}
            self.outputs.append(observation)
            self.check(case, "native output success", output.get("native_api_success") is True, True, output.get("native_api_success"))
            try:
                path = self.resolve(output["path"])
                observation["resolved_path"] = str(path)
                metadata = self.probe(path)
                video_streams = [stream for stream in metadata.get("streams", []) if stream.get("codec_type") == "video"]
                audio_streams = [stream for stream in metadata.get("streams", []) if stream.get("codec_type") == "audio"]
                self.check(case, "video stream count", len(video_streams) == 1, 1, len(video_streams))
                self.check(case, "audio stream count", len(audio_streams) == output["expected_audio_streams"], output["expected_audio_streams"], len(audio_streams))
                format_names = metadata.get("format", {}).get("format_name", "").split(",")
                self.check(case, "ISO media container", bool({"mov", "mp4"}.intersection(format_names)), "mov/mp4 family", format_names)
                expected_video = output["video"]
                self.near(case, "container duration", finite_number(metadata.get("format", {}).get("duration")),
                          float(expected_video["duration_seconds"]), float(expected_video["duration_tolerance_seconds"]))
                if len(video_streams) == 1:
                    observation["video"] = self.verify_video(path, video_streams[0], expected_video, case)
                if output["expected_audio_streams"] and len(audio_streams) == 1:
                    observation["audio"] = self.verify_audio(path, audio_streams[0], output["audio"], expected_video, case)
            except (VerificationError, OSError, KeyError, TypeError, ValueError, subprocess.TimeoutExpired) as error:
                self.check(case, "complete independent decode verification", False, "all media checks executable", str(error))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--media-root", type=Path, help="Directory containing copied media, resolved by unique basename")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    report_path = args.report or args.manifest.parent / "movie-writer-verification.json"
    report = {"schema_version": 1, "manifest": str(args.manifest), "passed": False}
    exit_code = 2
    try:
        try:
            import numpy as np
        except ImportError as error:
            raise VerificationError("NumPy is required. Run with a Python environment containing numpy (for example miniforge).") from error
        manifest = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
        if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
            raise VerificationError("Expected schema_version 1 MovieWriter contract manifest.")
        if args.media_root is not None and not args.media_root.is_dir():
            raise VerificationError(f"Not a media directory: {args.media_root}")
        ffprobe, ffmpeg = find_tool("ffprobe"), find_tool("ffmpeg")
        report["tools"] = {"ffprobe": ffprobe, "ffmpeg": ffmpeg, "numpy": np.__version__}
        verifier = Verifier(manifest, args.media_root, ffprobe, ffmpeg, np)
        verifier.run()
        report["checks"] = verifier.checks
        report["outputs"] = verifier.outputs
        report["failed_checks"] = sum(not check["passed"] for check in verifier.checks)
        report["passed"] = report["failed_checks"] == 0
        exit_code = 0 if report["passed"] else 1
    except (VerificationError, OSError, ValueError) as error:
        report["error"] = str(error)
        print(f"MovieWriter verification error: {error}", file=sys.stderr)
    try:
        report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
    except OSError as error:
        print(f"Cannot write verification report {report_path}: {error}", file=sys.stderr)
        return 2
    print(f"MOVIE_WRITER_VERIFICATION_REPORT={report_path.resolve()}")
    print(f"MovieWriter decoded media: {'PASS' if report['passed'] else 'FAIL'}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
