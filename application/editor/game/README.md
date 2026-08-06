# Editor game props (sample scripts) — DEMO ONLY

`PropBase` / `PropComponentBase` / `GameScene` are **demo gameplay scripts** over the
engine ECS — not a second component system and not a supported embedding API.

| Layer | Owns |
| --- | --- |
| `SceneEntityWorld` | Hierarchy, meshes, lights, transforms (engine truth) |
| `game::Prop*` / `ModelInstance` / `LightController` | Sample Tick / UI that reads and writes that ECS |

Also demo-only (still linked from `causScene` for historical reasons):

- `scene/GameModel.h` — prototype import worlds for props
- `scene/GameTypes.h` — `Pose`, `KeyframeAnimation`, `LightController`

Prefer engine APIs for real applications:

- `EntityWorld` / `Query<>` system parameters
- `caustica::load` / `spawn` / `despawn` (`SceneSpawn.h`)
- `caustica::setEntityLocalTransform` (`SceneTransform.h`)
- `caustica::setMeshVertices` (`SceneMeshEdit.h`)

Do not grow new engine components or render digs inside this folder.
SampleGame only activates when a scene carries `GameSettings` and prop/model JSON.
