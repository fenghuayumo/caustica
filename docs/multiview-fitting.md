# 多视角拟合工具（首版）

日期：2026-10-10。该实现对应多视角方案中可复现评估和低维参数优化的首个开发切片。
入口是 `support/python/scene_fit_cli.py`，核心是同目录的 `scene_fit/`。
它使用现有 Caustica Python 公共 API，不修改渲染核心，不需要模型 API key。

## 当前工作流：Blender 完成几何，Caustica 拟合外观

```text
多视角参考图与标定
  → Blender 建模、几何比较与迭代
  → 导出 glTF/GLB、纹理、材质槽以及相机/尺度信息
  → Caustica 导入，确认投影与几何一致
  → Scene Fit 固定几何和相机，仅调整材质、灯光
  → 保留视角验证，导出 Caustica 场景和 OpenPBR 材质
```

Blender 阶段可以完全独立进行，不需要 Caustica 同步插件、IPC 或往返渲染。
独立的 [COLMAP → Blender 自动几何工作流](blender-geometry-fitting.md) 已提供，入口为 `support/python/blender_fit_cli.py`。
几何修改继续在原 `.blend` 中完成；导出模型是两个阶段的交接文件。
几何建模 Agent 和外观拟合 Agent 可以独立运行，共享参考图与相机约定。
Blender 中应核对多视角轮廓、尺寸、遮挡关系，以及透明物体的实际壁厚/内部结构；仅外轮廓相同不保证折射正确。

交接时保留 UV、法线、材质槽和自包含纹理；明确米制尺度、坐标变换、相机位姿与像素内参。
模型导出并不自动包含 Scene Fit 的参考相机标定，必须在 views 中填写转换后相机。
Caustica 导入后先检查参考投影；若轮廓不对，检查相机/坐标转换，必要时回 Blender 修正并重新导出。
外观拟合不使用灯光或材质补偿几何错误。

`optimize` 默认只搜索 material、lighting；geometry 必须通过 `--groups geometry ...` 显式启用。
对 Blender 已完成的模型，项目 parameters 应仅声明材质和灯光绑定，保持对象变换与 Mesh 不变。
相机固定在 views 中，不参与当前参数搜索。材质组只允许绑定材质资产，灯光组只允许绑定灯光及其变换。
已有运行中曾经调整的几何不会在 resume 时自动恢复；外观阶段应从确认好的导出模型新建 run。

## 已实现与边界

- 校验版本化参考项目、相机 look-to/内参、图像尺寸、前景 mask、参数范围和资源依赖。
- 固定相机、曝光、AgX、无降噪参考模式，串行渲染多个视角，保存 RGB、实例前景 mask、深度和线性 RGB。
- 使用训练视角的前景区域 RGB MAE、轮廓 IoU 和可选深度误差选择最佳候选。
- 默认按 material/lighting 分组运行受约束坐标搜索，geometry 可显式启用；支持预算、取消与从最佳候选继续。
- 每个候选拥有私有场景、材质和资产目录；恢复/导出覆盖外部材质文件，不依赖 SceneDocument.undo。
- 输入与候选哈希检查、失败记录、运行互斥、HTML 对照报告，以及供应商无关的 JSON 工具请求入口。

Scene Fit 的几何参数修改的是已导入对象的 Transform 数值，例如尺寸、位置或欧拉角；Blender 参数化建模与视觉模型调用由独立的 `blender_fit` 执行，Scene Fit 本身没有拓扑操作、模型供应商 SDK 或 MCP 服务。
半透明和透明对象的 first-hit mask/depth 不保证符合真实参考标注，需后续定义专门指标。
3DGS 可以由场景声明并渲染，但尚未建立透明物体/3DGS 混合场景的专项基准。

## 环境与快速运行

需要 Python 3.9+、NumPy、Pillow 和已构建/安装的 Caustica 扩展。校验及 CPU 单元测试无需 GPU。
`import caustica` 成功不一定代表加载了扩展：项目源码目录可能被识别为同名 namespace package；必须有 EngineApp 和 SceneDocument。

从仓库根目录执行 PowerShell：

```powershell
# 使用当前本地构建；如果已经安装可用 wheel，则不需要设置这行。
$env:PYTHONPATH = (Resolve-Path bin).Path

# 新建真实 GPU 渲染的合成参考：两个训练视角，一个保留验证视角。
# 输出目录必须尚不存在；参考生成失败不会发布 project.json。
python support/python/scene_fit_cli.py demo --output-dir build-fit/demo
python support/python/scene_fit_cli.py validate build-fit/demo/project.json

# 几何与参考一致，仅材质红色分量和灯光强度从扰动值开始。
python support/python/scene_fit_cli.py optimize build-fit/demo/project.json `
  --run-dir build-fit/run --max-evaluations 25

# 中断后从冻结输入和最佳候选继续，不读取已经改变的原始项目。
python support/python/scene_fit_cli.py optimize --run-dir build-fit/run `
  --resume --groups material lighting --max-evaluations 12

# 将最佳候选导出到一个新目录；场景资源引用会随目录迁移。
python support/python/scene_fit_cli.py restore --run-dir build-fit/run --resume

python -m unittest discover -s support/python/tests -p test_scene_fit.py -v

# 可选 GPU 验收：固定几何、19 次外观候选、保留视角和独立引擎导出重载。
python support/python/tests/scene_fit_gpu_test.py --output-dir build-fit/gpu-acceptance

# 原有几何搜索作为可选回归基准保留。
python support/python/tests/scene_fit_gpu_test.py --include-geometry `
  --output-dir build-fit/joint-acceptance
```

`--vulkan`、`--adapter` 是全局选项，放在子命令前。
所有输出位于指定目录；引擎自身可能在工作目录生成 `scene_switch.log`。

## 参考项目格式

`demo` 会生成一个完整可编辑的 project.json，建议以它为起点替换真实数据。
根字段为：

| 字段 | 约定 |
| --- | --- |
| schema_version | 1 |
| coordinates | caustica_y_up_meters：右手、Y-up、米 |
| comparison_space | display_rgb：参考照片 RGB 与固定显示变换后的渲染 RGB 比较 |
| asset_root | 项目私有资源目录，禁止指向整个大型共享 Assets 库 |
| assets | alias → asset_root 内的相对文件路径 |
| foreground_entities | 场景顶层 authored ID 列表，包含这些节点下展开的所有 mesh leaves |
| scene | 已解析的普通 entities[] 场景，不接受 base/overrides |
| render | width、height、spp、bounces、timeout_seconds、exposure_compensation |
| parameters | 名称、组、单位、初始值、边界、搜索步长和 binding |
| views | 相机、参考图、mask、可选 depth，以及 train/validation 分组 |
| loss_weights | rgb、silhouette、depth；非负，至少一个正值 |

场景资源只使用 `asset:alias`、`builtin:`、`procedural:` 引用；材质槽的资源引用也应使用 alias。
每个候选复制完整 asset_root，再将引用解析到候选自己的目录。
当前 alias 支持 glTF/GLB/PLY、`.material.json`、纹理和环境图片；不支持 OBJ/URDF/USD 的依赖收集。
glTF/GLB buffers/images 的外部 URI 必须指向 bundle 内的本地文件（允许 data URI）。
材质 texture/path/source 字段的文件引用相对于材质文件，并在发布前改写为候选绝对路径。
整个 bundle 需自包含；嵌套 JSON prefab 和任意导入器扩展的依赖暂不支持。

参数示例：

```json
{
  "name": "object_width",
  "group": "geometry",
  "unit": "scale",
  "initial": 0.6,
  "bounds": [0.4, 1.5],
  "step": 0.2,
  "binding": {
    "target": "entity",
    "id": "object",
    "path": "/components/Transform/scale/0"
  }
}
```

材质使用 `target=material`、`id=assets 中的 alias`，例如 `/OpenPBR/specular_roughness`。
灯光使用实体 binding，例如 `/components/PointLight/intensity`。
binding 必须指向已存在的数值字段；不允许增加结构、修改字符串或未声明的参数。
组名不改变字段单位：Caustica 的角度字段和材质长度字段仍按各自 API 文档解释。

相机示例：

```json
{
  "id": "front",
  "split": "train",
  "image": "references/front.png",
  "mask": "references/front-mask.png",
  "camera": {
    "position": [0, 1.2, 4],
    "direction": [0, -0.7, -4],
    "up": [0, 1, 0],
    "intrinsics": {"fx": 950, "fy": 950, "cx": 480, "cy": 360}
  }
}
```

direction 是世界空间 look-to 方向，不是欧拉角或外参矩阵。
fx/fy/cx/cy 是当前 render 分辨率中的像素内参；图像原点在左上。
所有视角使用相同分辨率。照片必须事先去畸变；本工具不估计相机或补偿照片裁剪。
mask 为与图像同尺寸的 PNG，灰度 >127 表示前景。深度为可选 H×W `.npy`，单位米，轴向 |view Z|，0/NaN 无效。
实例 ID 需要在 Mesh leaves 间唯一，否则拒绝捕获，避免把背景错误计入前景。

## 误差、选择与完成语义

RGB 误差在参考与渲染前景 mask 的并集上计算，避免忽略错误扩大的物体。
轮廓误差为 1−IoU。各可用误差项按配置权重归一化，然后对视角取平均。
深度仅在双方有效的参考前景像素上计算，同时报告有效像素数；缺失深度不会被记作零误差。
启用深度时必须同时启用轮廓误差，以惩罚缺失几何。深度项以米计，RGB/轮廓无量纲，权重需要按任务设定。
普通相机照片带有未知响应/曝光，display_rgb 拟合并不保证恢复真实辐射度或唯一物理材质。

加载后选择自由相机并设置参考位姿/内参；每个视角清除历史与累积。
step_frame 是异步提交，本实现通过 sensor readback 等待渲染线程，再检查实际累积样本数。
每张图记录文档 session/revision、scene generation、frame、采样、相机、后端、设备与配置。
线性 RGB 保存用于诊断，首版不使用它直接与 PNG 参考计算误差。

优化只使用 train。结束后自动导出最佳候选并单独评估 validation；验证结果永不替换 best.json。
`max_evaluations` 限定本次训练候选数量，不包括最后一次验证捕获。
`max_seconds` 是训练阶段的软时间预算，在引擎调用边界检查；正在进行的 GPU/加载调用不能被抢占。
Ctrl+C 或在 run 目录创建 `CANCEL` 文件会停止后续工作并保留 best；继续前移除 CANCEL。
恢复从最佳参数重新启动步长搜索，不承诺逐条重放中断前的搜索调度。

每个 run 由 run.lock 独占。异常退出遗留锁时，确认原 worker 已退出后再删除锁。
failed/cancelled 候选有独立 result.json 和日志，不覆盖最佳有效候选。
输入/资产篡改检查失败时拒绝继续或恢复；不把成功收到命令等同于成功渲染。

## 大模型工具接入

宿主可以导入 Project/FitRunner，或通过 execute 的 JSON 文件接入任意工具调用框架。
该入口不绑定模型供应商，不运行网络服务器、不读取 API key，不是 MCP 协议实现。

```json
{
  "request_id": "fit-001",
  "operation": "optimize",
  "project": "E:/my-project/project.json",
  "run_dir": "E:/my-project/runs/run-001",
  "options": {"groups": ["material", "lighting"], "max_evaluations": 25}
}
```

```powershell
python support/python/scene_fit_cli.py execute request.json --response response.json
python support/python/scene_fit_cli.py capabilities
```

可用 operation：capabilities、validate、evaluate、optimize、restore。
evaluate 接受 `parameters` 数值覆盖与 split；已有 run 需要 `resume=true`。
restore 接受可选 candidate_id，默认最佳训练候选；同样需要 resume=true。
response.json 是权威响应，原生渲染日志可能输出到 stdout。
request_id 仅用于关联，不提供去重；宿主不能盲目重试成功命令。
重复 optimize 请求会继续搜索，重复 evaluate 请求会创建新的候选。

## 输出与下一步

```text
run/
  input/                  冻结参考、项目和原始私有资产
  run.json                输入哈希和来源
  iterations.jsonl        每个成功、失败或取消候选
  candidates/<id>/        场景、完整资产、参数、捕获、逐视角指标
  best.json               最佳训练候选
  optimization.json       停止原因、预算、步长
  exports/<id>-<time>/     可独立加载的导出场景与资源
  report.html             参考/渲染/叠加/差异×4
```

后续接入独立的 Blender 建模 Agent、导出项目准备、外观拟合模型适配器和 3DGS 专项验证。
Blender 实时同步是可选交互功能，不作为建模或外观拟合的前置条件。
首版合成 demo 是验证渲染反馈与搜索闭环的基准，不代表真实透明物体已经重建成功。
