"""Metadata-only observer tests; synthetic records are never game evidence."""
import copy
from pathlib import Path
import runpy
import unittest
import numpy as np

analysis = runpy.run_path(str(Path(__file__).resolve().parents[2] /
                             "scripts/analyze-target-writer-captures.py"))


def pair(draw=100, writer=1):
    operation = dict(frame="7", draw=str(draw), writer=str(writer), vs="VS", ps="PS",
                     surface="S", color="C", mask="8", depth="D", blend="B",
                     textures="3", window_offset="0", scissor="SC", scale="2x2",
                     submission="8", open="1", deferred="1")
    resource = dict(resource="R", size="2560x2048", format="10", source_samples="2")
    return [dict(draw=dict(operation, phase=phase), resource=copy.copy(resource))
            for phase in ("before", "after")]


class WriterAnalysisTest(unittest.TestCase):
    def test_exact_zero_rows_not_threshold_or_fidelity(self):
        values = np.ones((720, 8), dtype=np.float32)
        values[256:384] = 0
        values[640:720] = 0
        self.assertEqual(analysis["zero_row_ranges"](values), [[256, 384], [640, 720]])
        self.assertEqual(analysis["zero_row_ranges"](np.zeros((3, 2))), [[0, 3]])
        values = np.array([[0, -0.0], [0, 1e-30], [0, np.nan], [0, np.inf]])
        self.assertEqual(analysis["zero_row_ranges"](values), [[0, 1]])
        for bad in (np.zeros(2), np.zeros((0, 2)), np.zeros((2, 2, 2))):
            with self.assertRaises(ValueError):
                analysis["zero_row_ranges"](bad)

    def test_automatic_scope_is_partial_and_exact(self):
        arm = ("REX_EMBEDDED_TARGET_WRITER_ARM frame=7 first_observed_draw=99 "
               "scope=exact_writer_partial_frame ps=PS surface=S color=C\n")
        scope = analysis["capture_scope"](arm, pair())
        self.assertEqual(scope["earlier_draw_states"], "NOT_CAPTURED")
        self.assertTrue(scope["complete_pair_observed"])
        self.assertFalse(analysis["capture_scope"](arm, [])["complete_pair_observed"])
        for bad in (arm + arm, arm.replace("frame=7", "frame=8"),
                    arm.replace("draw=99", "draw=101"), arm.replace("ps=PS", "ps=OTHER"),
                    arm.replace("color=C", "color=OTHER"),
                    arm.replace("partial_frame", "whole_frame")):
            with self.subTest(arm=bad), self.assertRaises(ValueError):
                analysis["capture_scope"](bad, pair())

    def test_empty_is_unknown(self):
        self.assertFalse(analysis["validate_pairs"]([]))
        self.assertEqual(analysis["records"]("an unrelated log\n"), [])

    def test_four_valid_sparse_pairs(self):
        sequence = sum((pair(100 + i * 20, 2 + i * 3) for i in range(4)), [])
        self.assertTrue(analysis["validate_pairs"](sequence))

    def test_unpaired_and_budget(self):
        for sequence in (pair()[:1], sum((pair(i + 1, i + 1) for i in range(5)), [])):
            with self.assertRaises(ValueError):
                analysis["validate_pairs"](sequence)

    def test_operation_mismatch(self):
        for key in pair()[0]["draw"]:
            sequence = pair()
            sequence[1]["draw"][key] += "wrong"
            with self.subTest(key=key), self.assertRaises(ValueError):
                analysis["validate_pairs"](sequence)

    def test_resource_mismatch(self):
        for key in pair()[0]["resource"]:
            sequence = pair()
            sequence[1]["resource"][key] += "wrong"
            with self.subTest(key=key), self.assertRaises(ValueError):
                analysis["validate_pairs"](sequence)

    def test_duplicate_and_reordered(self):
        for sequence in (pair() + pair(), pair(200, 2) + pair()):
            with self.assertRaises(ValueError):
                analysis["validate_pairs"](sequence)

    def test_cross_frame_and_shader(self):
        for key in ("frame", "ps"):
            sequence = pair() + pair(200, 2)
            for entry in sequence[2:]:
                entry["draw"][key] += "other"
            with self.subTest(key=key), self.assertRaises(ValueError):
                analysis["validate_pairs"](sequence)

    def test_missing_readback_and_failure(self):
        for log in ("REX_EMBEDDED_TARGET_WRITER result=1 path=missing.bin\n",
                    "REX_EMBEDDED_TARGET_WRITER result=0 path=failed.bin\n"):
            with self.assertRaises(ValueError):
                analysis["records"](log)

    def test_truncated_capture(self):
        log = ("REX_EMBEDDED_RTV_CAPTURE label=target_writer_before dump=x.bin dumped=8 logical_bytes=16\n"
               "REX_EMBEDDED_PROMPT_RTV_READBACK resource=R\n"
               "REX_EMBEDDED_TARGET_WRITER result=1 path=x.bin frame=7\n")
        with self.assertRaises(ValueError):
            analysis["records"](log)

    def deferred_log(self, completed=8):
        return (
            "REX_EMBEDDED_TARGET_WRITER result=1 path=x.bin frame=7 submission=8 open=1 deferred=1\n"
            "REX_EMBEDDED_RTV_CAPTURE label=target_writer_before dump=x.bin dumped=16 logical_bytes=16 "
            f"deferred=1 submission=8 completed={completed}\n"
            "REX_EMBEDDED_PROMPT_RTV_READBACK result=ok resource=R\n")

    def test_completion_after_queue_is_required(self):
        self.assertEqual(len(analysis["records"](self.deferred_log())), 1)
        for log in (self.deferred_log(7), self.deferred_log().replace("open=1", "open=0"),
                    self.deferred_log().replace("dumped=16", "dumped=8"),
                    self.deferred_log().replace("result=ok", "result=failed"),
                    self.deferred_log().splitlines()[0]):
            with self.subTest(log=log), self.assertRaises(ValueError):
                analysis["records"](log)

    def test_legacy_synchronous_observer_is_rejected(self):
        log = self.deferred_log().replace(" deferred=1", "")
        with self.assertRaisesRegex(ValueError, "INVALIDATED"):
            analysis["records"](log)


if __name__ == "__main__":
    unittest.main()
