# Agent authoring foundation

Current workflow: geometry authoring entirely in Blender, file export, then
[fixed-geometry material/lighting fitting in Caustica (Chinese)](multiview-fitting.md).
Standalone calibrated geometry runner: [COLMAP to Blender fitting (Chinese)](blender-geometry-fitting.md).
Live [Blender scene synchronization](blender-caustica-integration-plan.md) is optional future work.

Future multiview geometry/material/lighting fitting requirements and implementation
milestones: [AI multiview reconstruction plan (Chinese)](ai-multiview-reconstruction-plan.md).

Implemented reference-project evaluation and bounded parameter-search runner:
[Multiview fitting (Chinese)](multiview-fitting.md).

Caustica now exposes a CPU-only `scene::SceneDocument` / Python `SceneDocument`
for persistent authoring and `EngineApp::inspectScene` / `inspect_scene` for
observing the expanded live scene. Both are provider-independent: they need no
model SDK, MCP transport, network connection, or API key.

## Ownership and publication

`SceneDocument` owns an ordinary, resolved `entities[]` scene JSON document.
It does not borrow ECS pointers and can be used without a GPU. One thread owns
each instance, including queries. Route requests from network threads to that
owner. Each successful changed batch is one history entry and one revision.
Validation is performed on a candidate copy before committing anything.

Publishing is explicit: save the document, call `engine.set_scene(path,
force_reload=True)`, then `wait_until_ready`. Loading is asynchronous; document
`status=completed` means the document edit completed, not GPU readiness. This
initial implementation reloads the scene when publishing. It does not incrementally
patch live ECS, synchronize editor UI edits, or add simulation frames to history.
After a failed runtime load, the authoring document remains available to correct
and publish again. Do not mark it rendered merely because the document is valid.

`base` / `overrides` overlays must be resolved before constructing this document.
Unknown top-level fields and object-valued components are preserved for future
extensions. Structural validation does not prove that an asset exists, a custom
component is supported, or that every renderer setting is meaningful.

Relative resource paths keep the existing engine semantics (pack root, then
scene-directory fallback). Saving to another directory does **not** rebase or copy
resources. Use pack-relative or absolute paths when publishing to a new directory.

## Python usage

```python
import caustica

doc = caustica.SceneDocument()
result = doc.execute({
    "request_id": "create-cube",
    "expected_session_id": doc.session_id,
    "expected_revision": doc.revision,
    "operations": [{
        "op": "add_entity",
        "entity": {
            "id": "cube",
            "components": {
                "PrefabInstance": {"source": "builtin:cube"},
                "Transform": {"translation": [0, 0, 0]}
            }
        }
    }]
})
if result["status"] != "completed":
    raise RuntimeError(result["error"])
saved = doc.save("my.scene.json")
if saved["status"] != "completed":
    raise RuntimeError(saved["error"])

# A running EngineApp can publish the saved document:
# engine.set_scene(saved["path"], force_reload=True)
# if not engine.wait_until_ready(timeout_seconds=120):
#     raise RuntimeError("Scene did not become ready")
# live = engine.inspect_scene(offset=0, limit=100)
```

`from_dict`, `from_json`, and `load` construct documents; invalid initial
documents raise `ValueError` (`std::invalid_argument` in C++). File read errors
raise `RuntimeError`. `document` and `get_entity` return copies in Python, so
modifying them does not silently bypass the revision/history contract.
`get_entity(id)` returns `None` for an unknown id.

`query_entities(component="PrefabInstance", offset=0, limit=100)` filters authored
entities by component and returns the total matching count and a page. Pages
are limited to 1000 entries. `SceneDocument.capabilities()` describes protocol
version, supported operations, units and validation scope.

## Command contract (version 1)

| Operation | Fields | Behavior |
| --- | --- | --- |
| `add_entity` | `entity` | Adds a node; omitted `id` generates a random persistent id returned in `created_ids`. |
| `patch_entity` | `id`, `patch` | Recursively merges fields; the id cannot be modified. |
| `remove_entity` | `id`, optional `cascade` | Rejects parents with children unless `cascade=true`; cascade removes the authored subtree. |
| `patch_settings` | `patch` | Recursively merges scene settings. |
| `undo` / `redo` | none | Restores the previous/next document; must be the sole operation in a batch. |

Patches use JSON Merge Patch semantics: `null` removes a field, objects merge,
arrays and scalar values replace. To replace a quaternion with Euler angles,
remove `rotation` in the same patch that sets `euler`. Reparent with
`patch: {"parent": "parent-id"}`; detach with `patch: {"parent": null}`.
Parents may be added later within the same batch. IDs must be unique and parent
references must exist without cycles in the final candidate document.

Requests contain 1–1024 operations. `request_id` is echoed for correlation; it is
**not** a deduplication key. An owner must not blindly replay a successful request.
Use `expected_session_id` and `expected_revision` when adapting Agent tools.
They are optional for local scripts and checked when present. Revisions are
monotonic within one session; undo/redo also increment them, and no-op batches
do not. Reloading a file starts a new session at revision zero. Entity ids are
stored in the scene; session ids, revisions and history are not.

Failures return `status=failed` plus `error.code` / `error.message` and the
unchanged revision. Codes include `invalid_argument`, `revision_conflict`,
`session_conflict`, `duplicate_id`, `entity_not_found`, `entity_has_children`,
`hierarchy_cycle`, `unsupported_operation`, `history_empty`, and `io_error`.
Each result also includes `session_id`, `revision` and `request_id`.
Non-JSON Python inputs and NaN/Infinity raise at the Python JSON boundary.

History holds complete document snapshots (32 entries by default in C++, capped
at 128). It does not copy model/texture data. Saving uses a temporary sibling file
and atomic replacement so a failed write does not truncate the previous scene.
It does not create missing directories or save referenced material assets.

## Materials, lights and cameras

Use `PrefabInstance.materials` for named slots or `MaterialOverride.source` for
all slots. References point to existing `.material.json` assets. This initial
history tracks **bindings**, not changes to the contents of external assets.
For Agent experiments, create private material assets; editing a shared material
file would affect every scene using that file and cannot be undone by scene history.
Material authoring fields follow [OpenPBR](openpbr.md).

Lights and cameras are ordinary authored components, so their parameters and
transforms participate in the same atomic batches and save/reload. Distances are
meters, world up is +Y, quaternion order is XYZW, and colors are linear RGB.
Transform Euler angles and camera vertical FOV are radians. Spot-light cone angles
and directional-light angular size retain their existing **degree** conventions;
the underlying component docs remain authoritative for field-specific units.

## Runtime observations

`engine.inspect_scene(offset=0, limit=100)` returns a page of the expanded live
graph with `scene_generation`, `frame_index`, names, paths, parent/child runtime
ids, persistent `authored_id` when present, local/world poses, bounds, kinds,
asset source and per-geometry material-slot names/source paths. Authored
light/camera component values reuse the save serializer. The scene root is
omitted. Ordinary imported mesh nodes have no persistent authored id. Nodes
inside JSON prefabs can have IDs local to that prefab: use
`authored_scope_runtime_id` to distinguish identical IDs in different instances.
A null scope denotes the enclosing scene document. Only edit top-level document
IDs through this document's command API; internal prefab IDs belong to their
source document.

Queries run on the logic thread, do not advance physical time or render, and
bounds reflect the most recent hierarchy propagation. Runtime ids are opaque
strings scoped to **that EngineApp and scene generation**. They are not asset
ids, semantic labels, GPU indices or saved references. Pages are ordered by
runtime handle; do not combine pages across loads or concurrent structure edits.
Empty/nonfinite bounds return null. Limits are clamped to 1000.

Document revision and live scene generation are intentionally different.
Record the document session/revision, published file, live generation, and
post-warmup frame alongside a preview image. That provenance allows an Agent
runner to know which scene produced its feedback.

## Verification and next extensions

```powershell
cmake --build build-release --config Release --target causSceneDocumentTests causSceneInspectionTests caustica_py
ctest --test-dir build-release -C Release -R "causScene(Document|Inspection)Tests" --output-on-failure
python caustica/Python/Tests/scene_document_test.py
python examples/python/scene_authoring.py --output-dir build-agent-check/authoring --document-only
python examples/python/scene_authoring.py --output-dir build-agent-check/render
```

Use the installed `caustica` package, or set `PYTHONPATH` to the built `bin`
directory as in the other Python examples. The example creates a private
OpenPBR material, scene objects, lights and camera; checks batch rollback,
undo/redo and save/reload; then verifies live ids, bounds, ground contact, material binding and
a nonblack GPU preview. It writes `verification.json`, `live-scene.json` and
`preview.png` under the chosen output directory.

The next layer can adapt this command contract to MCP or provider tool calling.
Incremental live application, shared editor undo, material-asset transactions,
async job/event transport, asset search and spatial placement are subsequent
extensions; none is claimed by the current document history.
