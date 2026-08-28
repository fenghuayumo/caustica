# Editor SampleGame -- DEMO ONLY

This folder is **not** an embedding API and **not** a second ECS.

| Layer | Owns |
| --- | --- |
| Engine (`SceneEntityWorld`, `EntityWorld`, `SceneSpawn`) | Hierarchy, meshes, lights, transforms |
| `demo::*` in this folder | Editor SampleGame scripts that Tick / drive UI over that ECS |

Contents (editor-linked only; **not** in `causScene` / thin_client):

- `GameScene` -- SampleGame stage loader (`GameSettings` + `SampleGame/` media)
- `PropBase` / `PropComponentBase` -- OO script objects (`PoliceLightingOnRX6`, `BasicInteractableUI`)
- `GameModel` / `GameTypes` -- model instance + `LightController` sidecars

## For new applications

Use the official sample instead:

- [`examples/cpp/thin_client`](../../../examples/cpp/thin_client/Main.cpp)
- `EntityWorld` / `Query<>` / `SceneSpawn` / `SceneTransform`

Do **not** copy `PropComponentBase`, `ModelInstance`, or `LightController` into engine or host code.
Do not grow new engine features in this folder.
