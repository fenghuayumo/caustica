import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
import os
import io

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from blender_fit.review import CRITERIA, validate_review, final_quality
from blender_fit.provider import Provider
import blender_fit_cli


class ReviewTests(unittest.TestCase):
    def setUp(self):
        self.geometry = {"parts": [{"name": "tray"}]}
        self.views = [{"id": "a", "width":128, "height":96}, {"id": "b", "width":128, "height":96}]
        self.value = {"checks": [{"part":"tray", "criterion":c, "status":"pass", "severity":0,
                                 "views":["a","b"], "reason":"visible in both views"} for c in CRITERIA]}

    def test_internal_wall_failure_changes_objective_at_identical_silhouette(self):
        good = validate_review(self.value, self.geometry, self.views)
        bad = copy.deepcopy(self.value)
        bad["checks"][2].update(status="fail", severity=0.8, reason="Inside wall too shallow")
        result = validate_review(bad, self.geometry, self.views)
        self.assertGreater(result["loss"], good["loss"])
        self.assertEqual(result["status"], "needs_geometry_review")

    def test_unknown_never_scores_as_pass(self):
        self.value["checks"][3].update(status="uncertain", views=[], reason="Support is occluded")
        self.assertGreater(validate_review(self.value, self.geometry, self.views)["loss"], 0)

    def test_rejects_incomplete_fabricated_or_single_view_pass(self):
        for mutation in (lambda x:x["checks"].pop(),
                         lambda x:x["checks"][0].update(views=["a"]),
                         lambda x:x["checks"][0].update(views=["a","invented"]),
                         lambda x:x["checks"][0].update(severity=float("nan")),
                         lambda x:x["checks"][0].update(status="fail", severity=0)):
            value = copy.deepcopy(self.value)
            mutation(value)
            with self.assertRaises(ValueError):
                validate_review(value, self.geometry, self.views)

    def test_final_gate_requires_heldout_and_training_evidence(self):
        review = validate_review(self.value, self.geometry, self.views)
        silhouette = {"status":"within_silhouette_tolerance", "failing_views":[]}
        self.assertEqual(final_quality(silhouette, review, review, ["b"])["status"], "within_geometry_tolerance")
        for training, audit, heldout in ((review, review, ["unseen"]), (review, None, ["b"]),
                                        ({"status":"needs_geometry_review"}, review, ["b"])):
            self.assertEqual(final_quality(silhouette, training, audit, heldout)["status"], "needs_geometry_review")
        silhouette["failing_views"] = [{"view":"b", "loss":0.2}]
        self.assertEqual(final_quality(silhouette, review, review, ["b"])["status"], "needs_geometry_review")

    def test_invalid_model_review_retries_are_bounded(self):
        with patch.dict(os.environ, {"BLENDER_MODEL_API_KEY":"test-only"}):
            provider = Provider({"supports_images":True, "model":"test", "base_url":"https://example.invalid"})
        with patch.object(provider, "request", side_effect=[{}, self.value]) as request:
            self.assertEqual(provider.review(self.geometry, self.views, [])["attempts"], 2)
            self.assertEqual(request.call_count, 2)
        with patch.object(provider, "request", return_value={}) as request:
            with self.assertRaisesRegex(ValueError, "three attempts"):
                provider.review(self.geometry, self.views, [])
            self.assertEqual(request.call_count, 3)

    def test_cli_pending_geometry_has_nonzero_exit(self):
        argv = ["blender_fit_cli.py", "run", "project.json", "--output-dir", "run"]
        with patch.object(sys, "argv", argv), patch.object(blender_fit_cli, "find_blender", return_value="blender"), \
             patch.object(blender_fit_cli, "run", return_value={"quality":"needs_geometry_review"}), \
             patch("builtins.print"), patch.object(sys, "stderr", io.StringIO()):
            with self.assertRaises(SystemExit) as result:
                blender_fit_cli.main()
            self.assertEqual(result.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
