#!/usr/bin/env python3
"""CPU-only Python contract tests; run against the installed/built module."""
from __future__ import annotations

import math
import tempfile
import unittest
from pathlib import Path

import caustica


class SceneDocumentTests(unittest.TestCase):
    def setUp(self):
        self.document = caustica.SceneDocument()

    def execute(self, operations):
        return self.document.execute({
            "request_id": "python-test",
            "expected_session_id": self.document.session_id,
            "expected_revision": self.document.revision,
            "operations": operations,
        })

    def test_generated_ids_and_copy_isolation(self):
        result = self.execute([{"op": "add_entity", "entity": {"name": "Cube", "components": {
            "PrefabInstance": {"source": "builtin:cube"},
        }}}])
        self.assertEqual(result["status"], "completed")
        self.assertEqual(result["request_id"], "python-test")
        entity_id = result["created_ids"][0]
        self.assertTrue(entity_id)
        snapshot = self.document.document
        snapshot["entities"][0]["id"] = "tampered"
        entity = self.document.get_entity(entity_id)
        entity["name"] = "tampered"
        self.assertEqual(self.document.get_entity(entity_id)["name"], "Cube")
        self.assertIsNone(self.document.get_entity("tampered"))
        self.assertEqual(self.document.query_entities(component="PrefabInstance")["total"], 1)

    def test_history_and_conflicts(self):
        self.execute([{"op": "add_entity", "entity": {"id": "a"}}])
        original = self.document.document
        result = self.execute([
            {"op": "patch_entity", "id": "a", "patch": {"name": "must rollback"}},
            {"op": "patch_entity", "id": "missing", "patch": {}},
        ])
        self.assertEqual(result["error"]["code"], "entity_not_found")
        self.assertEqual(self.document.document, original)
        self.assertEqual(self.document.revision, 1)
        self.assertEqual(self.execute([{"op": "undo"}])["status"], "completed")
        self.assertEqual(self.document.revision, 2)
        self.assertEqual(self.document.document["entities"], [])
        self.assertEqual(self.execute([{"op": "redo"}])["status"], "completed")
        self.assertEqual(self.document.document, original)
        conflict = self.document.execute({"expected_revision": 1, "operations": [{"op": "undo"}]})
        self.assertEqual(conflict["error"]["code"], "revision_conflict")

    def test_json_boundary_and_invalid_initial_documents(self):
        with self.assertRaises(ValueError):
            caustica.SceneDocument.from_json('{"entities":[{"id":"a"},{"id":"a"}]}')
        with self.assertRaises(ValueError):
            caustica.SceneDocument.from_dict({"entities": [], "value": math.nan})
        result = self.document.execute(["not a request object"])
        self.assertEqual(result["status"], "failed")
        self.assertEqual(self.document.revision, 0)

    def test_save_reload_and_failed_replace(self):
        self.execute([{"op": "add_entity", "entity": {"id": "a"}}])
        with tempfile.TemporaryDirectory(prefix="caustica-document-") as directory:
            path = Path(directory) / "scene.json"
            self.assertEqual(self.document.save(str(path))["status"], "completed")
            loaded = caustica.SceneDocument.load(str(path))
            self.assertEqual(loaded.document, self.document.document)
            self.assertNotEqual(loaded.session_id, self.document.session_id)
            self.assertEqual(loaded.revision, 0)
            self.assertEqual(self.document.save(str(path))["status"], "completed")
            self.assertEqual(self.document.save(str(path / "invalid.json"))["error"]["code"], "io_error")
            self.assertEqual(caustica.SceneDocument.load(str(path)).document, self.document.document)
            self.assertFalse(list(Path(directory).glob("*.tmp")))


if __name__ == "__main__":
    unittest.main()
