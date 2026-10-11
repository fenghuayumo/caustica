# 配准后的多视角纹理工作流

入口是 `support/python/texture_fit_cli.py`。它独立处理纹理，不重新拟合几何，
也不将照片当成已经分离照明的物理 base color。几何拟合和 Scene Fit 仍分别负责形状、材质与灯光。

```text
几何检查 → 世界坐标 mesh + 原始 COLMAP 相机 + 训练照片
        → 全场景深度 / 逐物体可见性
        → 扩大相邻物体和细线遮挡排除范围
        → 照片投到物体独立 UV（只是初始对应）
        → 图像配准与一致性验收
        → 单视角区域拼接（不做全视角颜色平均）
        → 单独记录观测、来源视角、补全
        → Blender 应用 → 多视角审查 / 保留视角验证 → Scene Fit
```

## 输入与运行

依赖 NumPy、SciPy（接缝校正）、Pillow、OpenCV（SIFT/ECC）以及用户已有的 photara_drender Python 运行时。
`photara_runtime` 指向包含 `photara_drender` 包的目录；其原生扩展需要与 Python ABI 匹配。
photara 提供 Vulkan 光栅化和属性插值。本流程使用全场景深度光栅化判断最前方物体，
**没有调用 ray-query 纹理融合**。它不会回退到无遮挡投影。

先导出 Blender 世界坐标网格。相机和网格必须使用相同坐标系、尺度；
`export-meshes` 不使用 GLB 的 Y-up 坐标转换，也不移动物体。

```powershell
python support/python/texture_fit_cli.py export-meshes --model model.blend --blender blender.exe --output-dir meshes
python support/python/texture_fit_cli.py validate texture-config.json
python support/python/texture_fit_cli.py bake texture-config.json --output-dir texture-run
python support/python/texture_fit_cli.py refine-seams --textures texture-run --output-dir texture-refined --strength 0.7 --band 5
python support/python/texture_fit_cli.py apply --model model.blend --textures texture-refined --blender blender.exe --output-dir textured-model
python support/python/texture_fit_cli.py review --model textured-model/model.blend --config texture-config.json --blender blender.exe --output-dir texture-review
```

输出目录必须不存在，保证新一次拟合不覆盖旧结果。异常时 `summary.json` 保留 `running`，
`apply` 会拒绝尚未完成的纹理结果。修正参数后使用新的输出目录。
应用时还会核对烘焙网格快照与目标 Blender 的世界位置、法线、UV 和三角形，
几何迭代后不能把旧纹理直接套到变化的模型上。

配置中的路径相对于配置文件。`views` 为 `{"views": [...]}` JSON，
沿用 blender_fit 的相机格式：`id`、`source_name`、`width`、`height`、
`camera.position/direction/up/intrinsics`，内参包含 `fx/fy/cx/cy`。
照片分辨率必须与相机的 width/height 一致；不要把缩小后的相机与原尺寸照片混用。
必须显式提供 `heldout` 文件名列表，任何保留照片出现在纹理来源中都会失败。

```json
{
  "images": "original-images",
  "meshes": "meshes",
  "views": "training-views.json",
  "photara_runtime": "photara-python-runtime",
  "heldout": ["validation.jpg"],
  "objects": [
    {
      "name": "fruit",
      "mode": "registered_patches",
      "resolution": [1024, 2048],
      "color_guard": "green",
      "occlusion_guard_px": 24,
      "min_facing": 0.4,
      "max_registration_shift": 32,
      "min_patch_texels": 8192,
      "max_source_views": 8,
      "min_patch_extent": 48
    }
  ]
}
```

分辨率为 `[height, width]`。颜色先验 `green/red` 只作粗过滤，不能取代实例分割。
没有颜色先验的物体可以省略 `color_guard`，此时更需要核对实际照片物体边界。

## 水果与书的不同策略

`registered_patches` 保留原网格 UV。初始采样使用全场景物体归属、法线朝向、
照片颜色先验；向外扩张的遮挡保护包含所有相邻网格，包括其他水果。
选安全采样面积、朝向与清晰度较好的主视角，再尝试其他视角。
另外在原照片上检测长直线梯度并保守扩张排除，用于捕捉与近似网格错位的篮丝 / 线状阴影；
`visibility/*-photo-lines.png` 保存诊断 mask。这是启发式，可能排除真实的水果长纹路，
也可能漏掉弯曲篮丝，因此必须核对实际贴图；`exclude_photo_lines: false` 可关闭。
SIFT/RANSAC 相似变换优先；特征不足时尝试受限的 masked ECC 平移。
最大位移、尺度、配准后重叠相关性必须通过检查。
高频细纹的相关性也必须通过，避免仅因为两张图明暗相近就允许拼接。
每个物体最多保留 `max_source_views` 个贡献来源；新区域必须满足最小连续面积和长宽，
不把几十张图中的小碎片逐一补进贴图。
失败的视角不得补入新区域；通过的视角只填缺口，同一观测像素保留一个照片来源。
不会因一个视角配准失败而退回全视角 RGB 平均。

`planar_faces` 用于书等分面物体。配置 `faces`，每面给出按 UV 顺序排列的
四个世界坐标 `corners`、外侧 `normal` 和 `corner_ids`。
`annotations[view_id][corner_id]` 是原尺寸照片中的角点坐标，优先于几何投影角点。
`atlas_columns` 与网格现有 UV 排布一致，`face_resolution` 控制每个方形面块的大小。
`solid_occluders` 指定整块排除的遮挡物；书面在篮子下不能以篮丝之间的孔洞作安全来源。
`inferred: "paper" / "binding"` 对应推断书页或不可见装订面。
缺少安全来源的照片面直接失败，不自动套用无关封面。

## 审查与已知限制

每个物体输出 `*-color.png`、`*-observed.png`、`*-provenance.npz`。
NPZ 包含原 UV 覆盖、真实观测 mask、每像素来源视角索引；来源为 `-1` 表示补全。
`summary.json` 记录主视角、接受 / 拒绝的配准、实际覆盖率和限制。
覆盖率按 UV 图集有效像素计算，并非按世界坐标表面积加权；球面两极和书的不同面块需注意这种差别。
`inputs.json` 保存配置、相机、网格和每张照片的 SHA256，防止把旧相机或旧几何结果误用。
补全前清除所有被拒绝照片像素，避免污染低分辨率修补。
补全不改变观测标记，模型交付必须同时带覆盖记录。

此方法减轻颜色混合重影，但不保证准确恢复纹理：近似几何仍可能造成系统性错位，
遮挡 guard 是投影近似，颜色过滤也可能接受同色的错误物体。
全局相似变换 / 平移不能表达大幅非刚性形变；拒绝这些来源会降低覆盖。
低纹理表面、缺少重叠的照片和完全不可见区域不能凭更多照片恢复。
照片高光与阴影尚未通过逆渲染分离；单来源区域之间仍可能出现接缝和色差。
`refine-seams` 可作为独立微调阶段：屏蔽来源边界的跳变梯度，用 screened Poisson
校正低频色差，保留原图细纹，再修补窄边带。书只处理封面，书脊文字和纸页保持原状。
窄边带记录为推断像素；`*-seams.png` 与 provenance 内的低频修正场可复核。
这不能修复错误的文字 / 建筑对应，也不会恢复被遮挡的封面内容。
水果 UV 经线两端尚未做专门的周期性配准，需检查背面接缝。

验收应同时看原图、渲染、物体边界和来源 / 补全图，
不能仅凭整体 RGB MAE 或轮廓 IoU 认定材质正确。
`blender_fit_cli auto` 尚未自动调用该纹理阶段：几何确认后显式执行本入口，
确认纹理再执行 Scene Fit，避免几何迭代后继续使用旧纹理。
