# Editor game props (sample scripts)

`PropBase` / `PropComponentBase` are **demo gameplay scripts** over the engine ECS - not a second component system.

| Layer | Owns |
| --- | --- |
| `SceneEntityWorld` | Hierarchy, meshes, lights, transforms (engine truth) |
| `game::Prop*` | Sample Tick / UI that reads and writes that ECS |

Prefer engine APIs for scene edits:

- `caustica::load` / `spawn` / `despawn` (`SceneSpawn.h`)
- `caustica::setEntityLocalTransform` (`SceneTransform.h`)
- `caustica::setMeshVertices` (`SceneMeshEdit.h`)

Do not grow new "engine components" inside this folder.
