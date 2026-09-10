# Caelis 具身仿真：Caustica 接口规格

本文是 **Caustica 仓库内的实现清单**。消费者是 Caelis Sim：物理真值在宿主（Stiff-GIPC 生产，Isaac PhysX 仅对照），Caustica 只做 **渲染 / 传感器 sidecar**。

要对齐的是 Isaac Sim 的相机、标注、机器人外观、域随机化、headless 出数，**不是**再做一套 PhysX。

权威副本也在 Caelis 仓库：`docs/caustica-embodied-api.md`。两边冲突时以「语义 + 本文签名」为准，合入后同步改 `caustica.md`。

C++ 与 Python 必须同名同语义，Python 为 snake_case。现有公开面：[caustica.md](../caustica.md)、[public-api.md](public-api.md)。

---

## 1. 约定（不要改）

与 Caelis `FrameState` 对齐，改任何一条都要同步改两边。

| 项 | 值 |
|---|---|
| 世界 | 右手系，米、秒、弧度，**+Y up** |
| 位姿 | `translation_xyz_m` + Hamilton 四元数 **xyzw** |
| 相机局部 | OpenGL：+X 右，+Y 上，看向 **−Z** |
| 图像原点 | **左上** |
| Depth | float32 `H×W`，线性 **\|view Z\|** 米，**0 = miss**（含环境） |
| Instance / semantic | uint32，**0 = miss / unlabeled**，禁止自动分配 0 |
| RGB LDR | uint8，与 `save_screenshot` / `get_pixels()` **同一份** |
| 物理时间 | `step_frame` 才推进；`capture_sensor_outputs` 不得推进物理 |

---

## 2. 非目标（不要做进 Caustica）

| 不要做 | 原因 |
|---|---|
| 生产物理求解 | 权威后端是 Stiff-GIPC |
| 策略 / 奖励 / Isaac `ArticulationAction` | 编排层（Caelis） |
| Replicator writer（COCO、KITTI） | Caelis `dataset.py` |
| 2D/3D bbox、遮挡率 | 可用 instance + 相机几何在 Caelis 算 |
| GPU PhysX `num_envs` 克隆 | 短期用多进程 `EngineApp` |
| 把可选 PhysX 插件做成 Python 生产 API | 编辑器实验即可 |

---

## 3. 已经够用（实现新功能时复用）

| API | 用途 |
|---|---|
| `EngineApp.create(..., headless=True)` | Headless |
| `SceneEntity.set_intrinsics` / `look_to` / `camera_pose` | 针孔相机 |
| `add_render_product` / `capture_sensor_outputs` | 同一物理时刻多相机 |
| `SensorOutput.depth` / `instance_id` / `semantic_id` / `normal` / `motion_vector` | 几何 AOV |
| `set_world_pose` / `set_mesh_vertices` | 刚体、布料外观 |
| `Material` OpenPBR 属性 | 外观随机化 |
| `spawn_from_file`（gltf/obj/urdf/usd） | 资源导入；URDF 目前只是视觉 link 树 |
| `set_realtime_mode` / `set_reference_mode` | 预览 vs 参考帧 |
| `wait_until_ready` | 场景 commit（默认 `warmup_frames=32`，见 P0-1） |

---

## 4. 实现优先级

按这个顺序合入。P0 做完，XArm + 布料的视觉闭环可以离开 Isaac RTX。

| ID | 项 | 层 | 验收 |
|---|---|---|---|
| P0-1 | 预热 + 时域重置 | 引擎 + Python | realtime 加载后非黑；姿态瞬移无 TAA 残影 |
| P0-2 | LDR RGB ≡ `get_pixels`；另加线性 HDR | 引擎 + Python | `product.rgb` 与 screenshot 一致 |
| P0-3 | 每 RenderProduct 独立分辨率 | 引擎 + Python | 腕部 640×480 + 第三人称 1280×720 同帧 |
| P0-4 | 相机 parent + local pose | 引擎 + Python | 腕部相机随 link 走，不必每帧 `look_to` |
| P0-5 | 向量化 snapshot / pose / 顶点 | 引擎 + Python | 一帧一次提交，numpy 顶点 |
| P1-1 | URDF 关节视觉 FK **或** 批量 link pose | 引擎 + Python | `set_joint_positions` 或 `set_world_poses` |
| P1-2 | 布料拓扑更新 | 引擎 + Python | `set_mesh_triangles` |
| P2-1 | GPU / dlpack tensor | 引擎 + Python | depth/rgb/instance 零拷贝到 CUDA |
| P2-2 | 渲染 seed | 引擎 + Python | 同 snapshot + seed → 同噪声 |
| P3 | LiDAR、clone env、相机噪声 | 可选 | 不挡 MVP |

---

## 5. P0 规格

### P0-1 预热与时域历史

**问题：** 程序天空约 24 帧才稳定；`wait_until_ready(warmup_frames=4)` 默认太短。Realtime 第一帧发黑。`reset_accumulation` 清不掉 TAA/NRD，episode 姿态跳变会残影。

**C++**

```cpp
bool EngineApp::warmup(int frames);                 // 不推进宿主物理 snapshot
void EngineApp::resetTemporalHistory();             // 清 NRD / TAA / per-camera motion
// waitUntilReady: 默认 warmupFrames 提到 32，或内部在 scene ready 后自动 bake 环境 LUT
```

**Python**

```python
engine.warmup(frames: int = 32) -> None
engine.reset_temporal_history() -> None
engine.wait_until_ready(..., warmup_frames: float | int = 32) -> bool
```

语义：

- `warmup` 在场景 committed、相机已注册之后调用。
- 不调用 `warmup` 时，`wait_until_ready` 也必须把环境 LUT bake 完，不能返回后第一帧是黑的。
- `reset_temporal_history` 用于宿主瞬移物体（数据集回放）。连续仿真不要每帧调。
- `warmup` / `step_n` 不得把物理权威状态往后积分；Caelis 的 pose 是外部提交的。

**验收**

1. `create(realtime=True)` → `wait_until_ready()` → `get_pixels()` 立方体+地面+天空可见，均值明显大于 0。
2. 物体 yaw 跳 25° → `reset_temporal_history()` → `warmup(16)` → RGB 无上一姿态残影。
3. `examples/python/environment_lighting.py` 的 24 帧 bake 可改为调用 `warmup`。

**主要改动位置：** `engine/EngineApp`、`engine/RenderSessionApi`、realtime denoiser 历史。

---

### P0-2 RGB：LDR 同源 + 线性 HDR

**问题：** 文档写 Sensor RGB 是 RGBA8 LDR，但 realtime 下 `SensorOutput.rgb` 与 `get_pixels()` 曾不一致。HDR readback 未实现。

**引擎**

- `Aov.rgb`：与 `get_pixels()` / `save_screenshot` **同一 LDR**（uint8 `H×W×4` 或 `H×W×3`，文档写死一种）。
- 新增 `Aov.linear_rgb`（或 `Aov.hdr`）：float32 `H×W×3`，线性 radiance，**未** tonemap。Miss 为 0。

**Python `SensorOutput`**

```python
product.rgb          # uint8, LDR, 与 get_pixels() 像素一致
product.linear_rgb   # float32 HxWx3 or None（未请求时）
```

**验收**

1. 同一帧 `np.allclose(product.rgb[..., :3], engine.get_pixels()[..., :3])`。
2. `linear_rgb` 在直射面 > 0；环境 miss = 0。
3. 单测：请求 `Aov.rgb` 不带 linear 时 `linear_rgb is None`。

**主要改动位置：** `engine/SensorApi.h`、`SensorOutput`、path tracer AOV 导出、`PythonBindingsCore.cpp`。

---

### P0-3 每相机分辨率

**问题：** `EngineApp.create(width, height)` 是全局一张。腕部与第三人称需要不同分辨率。

**C++ `RenderProductDesc` 增加** `width` / `height`（0 = 用 session 默认）。

**Python**

```python
engine.add_render_product(
    name: str,
    camera=None,
    aovs=caustica.Aov.rgb | caustica.Aov.depth,
    width: int = 0,
    height: int = 0,
) -> None
```

`SensorOutput.width/height` 描述该 product 的 RGB；几何 AOV 若因 upscaler 不同，继续用已有的 `geometry_width` 等。

**验收：** 同帧 wrist 640×480、third 1280×720，两路 depth 形状各自匹配。

---

### P0-4 相机挂到实体

**问题：** 现在只能每帧世界系 `look_to`。Isaac 把 camera prim 挂在 gripper 下。

**引擎：** 透视相机 entity 支持 parent + 局部 TRS。父实体 `set_world_pose` 后，相机世界 pose 自动更新。

**Python**

```python
cam = engine.spawn_camera(
    name="wrist",
    parent=None,                    # SceneEntity | str path | None
    local_translation=(0, 0, 0),
    local_rotation=(0, 0, 0, 1),    # xyzw
    intrinsics=(fx, fy, cx, cy, w, h),
    aovs=None,
)
cam.set_parent(entity_or_path)      # None = 世界
cam.local_pose = (t, q, scale)
```

已有 `look_to` 仍写 **世界** look-to。有 parent 时只写 `local_pose`，不要混用 `look_to`。

**验收：** 父 link 绕 Y 转 90°，不改相机 local，腕部图像跟着转；instance 仍对得上 gripper。

**主要改动位置：** `CameraApi`、场景层级、Extract。

---

### P0-5 向量化 snapshot

**问题：** 每刚体一次 `set_world_pose`、布料 `list[tuple]` 顶点，XArm+布料会成百次跨语言调用。

**Python（最小可用）**

```python
engine.set_world_poses(
    names: list[str],                 # entity name or path
    translations: np.ndarray,         # (N, 3) float32
    rotations_xyzw: np.ndarray,       # (N, 4) float32
    scales: np.ndarray | None = None, # (N, 3)
) -> None

engine.set_mesh_vertices(
    entity,
    vertices: np.ndarray,             # (V, 3) float32
    *,
    space: str = "object",            # "object" | "world"
    recompute_normals: bool = True,
    rebuild_acceleration_structure: bool = True,
) -> None
```

保留现有 list/tuple 重载。

可选合并接口：

```python
engine.apply_visual_snapshot(
    rigids: dict[str, tuple[t, q]],
    meshes: dict[str, np.ndarray] | None = None,
    cameras: dict[str, ...] | None = None,
) -> None
```

**验收：** 100 个 cube 一帧 `set_world_poses` 与 100 次 `set_world_pose` 画面一致；布料 `(V,3)` float32 与 list 路径顶点一致。

**主要改动位置：** `SceneTransforms` / `EngineApp` 批量入口 + `PythonBindingsCore.cpp`。

---

## 6. P1 规格

### P1-1 机器人视觉 FK

URDF 导入已建 link 树（`UrdfImporter.cpp`），**没有**关节驱动。二选一，文档必须写死：

**方案 A（引擎 FK，更像 Isaac，推荐，已实现）**

```python
robot = engine.spawn_from_file("xarm.urdf")
robot.joint_names -> list[str]
robot.set_joint_positions(q: np.ndarray)     # rad，与 joint_names 对齐
robot.get_link_pose(link_name) -> (t, q)
robot.attach_camera("wrist", link="link_eef", local_t=..., local_q=...)
```

接触/力矩仍不进引擎。

**方案 B（宿主 FK）**  
不做关节，只保证 P0-5 批量 link pose 足够快。Caelis 用自己的 FK 填 `FrameState.rigid_bodies`。

### P1-2 可变形拓扑

```python
engine.set_mesh_triangles(entity, triangles: np.ndarray)  # (F, 3) uint32
```

拓扑变化必须 rebuild AS。

---

## 7. P2 规格

### P2-1 GPU tensor

```python
product.rgb_dlpack()
product.depth_dlpack()
product.instance_id_dlpack()
```

不必硬依赖 torch。设备与 `EngineApp` GPU 相同。

### P2-2 Seed

```python
engine.seed = 7
```

同 scene、同 snapshot、同 seed、同 spp/mode → RGB 可复现（denoiser 误差在测试里写明）。

---

## 8. 建议的一帧宿主循环

```python
engine = caustica.EngineApp.create(
    width=1280, height=720, headless=True,
    scene=scene_json, realtime=True,
)
engine.wait_until_ready(warmup_frames=32)
engine.add_render_product("wrist", wrist_cam, aovs, width=640, height=480)
engine.add_render_product("third", third_cam, aovs, width=1280, height=720)
engine.warmup(32)

while sim.running:
    snap = physics.step()                          # Stiff-GIPC / 对照 PhysX
    engine.apply_visual_snapshot(...)              # 或 set_world_poses + set_mesh_vertices
    engine.step_frame()
    for p in engine.capture_sensor_outputs():
        write_episode(p.rgb, p.depth, p.instance_id)
```

姿态瞬移（数据集回放）：`apply_visual_snapshot` 之后 `reset_temporal_history()` 再 `warmup(16)`。

---

## 9. 合入检查单

每项 PR：

- [ ] C++ 与 Python 签名同时落地
- [ ] `caustica.md` 对应表格已更新
- [ ] `caustica/Python/Tests/` 有新测或扩现有测
- [ ] 未把物理求解、策略、数据集 writer 带进引擎
- [ ] 坐标系仍是 Y-up、xyzw、depth `|view Z|`

建议测试文件：

- `caustica/Python/Tests/sensor_warmup_test.py`
- `caustica/Python/Tests/sensor_aov_consistency_test.py`
