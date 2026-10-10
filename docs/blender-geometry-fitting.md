# 独立 Blender 多视角建模工作流

入口：`support/python/blender_fit_cli.py`。整个几何阶段只使用 Python、Blender 和模型服务，不加载 Caustica。
相机和稀疏点云来自 COLMAP，几何确认后通过文件交给 Scene Fit 调材质与灯光。

## 当前实现

```text
COLMAP cameras/images/points3D + 参考图 + 可选前景 mask
  → 固定标定相机，复制参考输入，缩放图像与内参
  → 视觉模型提出参数化几何与物体框
  → Blender 生成 mesh，按每个标定相机渲染
  → 训练视角 silhouette IoU 评分，尺寸搜索与模型结构修改
  → 保留视角独立验证
  → model.blend + model.glb + 相机侧车 + Scene Fit 项目
```

已实现供应商可配置的多模态 Chat Completions HTTP 适配器，不需要模型 SDK。
模型接收训练参考图、相机、点云摘要以及当前训练渲染和误差，返回 JSON 几何。
Blender 执行允许的几何操作；模型不能返回任意 Python 脚本。
支持组合盒体、椭球、圆柱、旋转剖面实体和开放薄壳；每次最多 32 个部件，剖面最多 64 个采样点。
部件有位置、XYZ 欧拉角和尺寸；旋转体还有剖面与壁厚。模型可以增删部件、修改这些字段。
程序另外执行尺寸坐标搜索，只按训练误差保留更好的候选；预算为模型修改预留评估次数。

点云通过训练 mask 的多视角投影过滤背景，然后提供给模型作为空间证据。
导出的 `.blend` 保存最多 256 个前景点作为不参与渲染的辅助 mesh，便于检查。
稀疏点云目前不是点到曲面的损失项；反光、透明物体的稀疏点尤其不应直接视为可靠表面。

## COLMAP 输入约定

传入包含 `cameras.bin/images.bin/points3D.bin` 或对应 `.txt` 的目录，也支持其父目录下的 `0/`。
同时存在时优先 binary；支持非连续 ID、图片名中的空格和空 observations 行。
使用 **去畸变后的 PINHOLE 或 SIMPLE_PINHOLE 模型及其对应图像**。
畸变相机明确报错，应先运行 COLMAP `image_undistorter`，再传入输出的 `sparse/` 和 `images/`。
输入帧定义和数据结构依据 [COLMAP 官方格式说明](https://colmap.github.io/format.html)。

COLMAP 世界坐标直接用作 Blender 世界 XYZ，不根据相机朝向猜测世界上轴。
正确反转 world-to-camera 位姿，将相机 right/down/front 转为 Blender right/up/back。
Blender 使用对应 fx/fy、主点和像素长宽比，因此支持非中心主点和 fx≠fy。
GLB 导出开启 Y-up，交接相机同步使用 `(x,y,z) → (x,z,-y)`。

COLMAP 的坐标尺度通常不能仅从照片确定。用 `--meters-per-unit` 声明一个重建单位对应多少米，
程序同时缩放点云与相机。几何始终在这个缩放后的世界坐标中生成。
若不提供，允许独立 Blender 建模并导出，但不生成可直接运行的米制 Scene Fit 项目，
`export/handoff.json` 会标记 `needs_metric_scale`。

## 环境与单命令运行

需要 Python 3.9+、NumPy、Pillow、Blender（本机验证版本 5.2.1 LTS）。
自动前景分割额外需要 `opencv-python`，直接提供 mask/透明背景 PNG 则不需要。
可使用 `--blender` 或 `BLENDER_EXECUTABLE` 指定 Blender；否则查 PATH 和常见安装目录。
Blender 使用 CPU Cycles 进行可复现的中性轮廓渲染，几何阶段不需要 Caustica GPU。

先复制 `support/python/blender_fit/provider.example.json`，填写你实际使用的视觉模型与服务地址。
API key 从配置的环境变量读取，不写入项目或日志。
适配器使用 [Chat Completions 官方图像消息与 JSON 输出接口](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create)。
兼容服务须支持图像消息与 `response_format: json_object`；`supports_images:true` 是明确声明，不能令文本模型获得视觉能力。
GPT 或其他服务使用你已有的实际视觉模型名；DeepSeek 接入也必须满足同样的图像接口条件。

```powershell
# 在自己的终端设置密钥，provider.json 里只保存 api_key_env 的变量名。
$env:BLENDER_MODEL_API_KEY = '<你的模型服务密钥>'

python support/python/blender_fit_cli.py auto `
  --colmap D:/capture/undistorted/sparse `
  --images D:/capture/undistorted/images `
  --masks D:/capture/masks `
  --provider-config D:/capture/provider.json `
  --target '桌面上的玻璃杯，包含杯壁和杯底，不包含桌面' `
  --meters-per-unit 0.0125 `
  --blender D:/SoftWare/Blender/blender.exe `
  --output-dir build-fit/blender-cup `
  --max-evaluations 30 --model-rounds 3
```

`--meters-per-unit` 示例不是默认尺度，需要替换为实际标尺结果。
`--masks` 可省略。mask 优先查 `原图文件名.png`（例如 `cup.jpg.png`），再查替换扩展名后的 `cup.png`，
保留原图子目录结构。mask 为与原图同尺寸的灰度图，>127 为前景；透明背景参考图可直接使用 alpha。
没有 mask 时，模型定位框后执行 GrabCut，输出分割预览。复杂背景、透明杯口或折射区域可能分割错误，
真实数据应优先提供人工核对的物体轮廓 mask。

默认最多均匀取 8 张图，长边缩到 512 像素，并同步缩放 K。
按原图文件名排序后最后一张作为保留视角，其余作为训练视角；至少 3 张图。
保留视角的照片不进入几何模型提示，误差不参与候选选择。
如果它缺少 mask，只用于一次独立的物体框预处理请求，结果不会反馈给几何建模模型。
需要自定义划分时先 `prepare`，再修改 `project.json` 中各 view 的 `split`，最后运行 `run`。

所有输出目录须尚不存在，输入快照与运行目录不能相互嵌套。
单次调用预算结束就导出最佳候选；`state.json` 的 `quality` 明确标记是否达到训练/验证平均轮廓误差 ≤0.05。
运行完成不等于任意真实物体已经重建准确。通过创建运行目录下的 `CANCEL` 文件可以取消 Blender 迭代，
失败和取消保留已经评估的候选、最佳几何与日志。首版不提供原地 resume；可将 `best-geometry.json`
作为新运行的 `--geometry` 初值继续拟合。

## 分步运行与无模型调试

```powershell
python support/python/blender_fit_cli.py prepare `
  --colmap D:/capture/sparse/0 --images D:/capture/images --masks D:/capture/masks `
  --meters-per-unit 0.0125 --output-dir build-fit/cup-input

python support/python/blender_fit_cli.py validate build-fit/cup-input/project.json

python support/python/blender_fit_cli.py run build-fit/cup-input/project.json `
  --provider-config D:/capture/provider.json --output-dir build-fit/cup-run

# 有现成参数化初值时，无 API key 也能验证 Blender 渲染与尺寸搜索。
python support/python/blender_fit_cli.py run build-fit/cup-input/project.json `
  --geometry D:/capture/initial-geometry.json --output-dir build-fit/cup-offline
```

`initial-geometry.json` 示例（坐标、尺寸须与你的点云对应）：

```json
{"parts":[{"name":"cup","kind":"lathe","position":[0,0,0],"rotation":[0,0,0],
"size":[1,1,1],"profile":[[0,0.035],[0.09,0.04]],"thickness":0.002}]}
```

旋转体沿局部 Z 轴。profile 的 height/radius 和 thickness 为局部长度，size 为 XYZ 乘法缩放；
其他形状 size 是完整尺寸。非均匀缩放也会影响实际壁厚；有精确壁厚要求应保持旋转体 size=[1,1,1] 并改剖面。
开放薄壳有 Solidify 形成壁厚与端部环，实心旋转体封闭两端。
任意自由曲面、布料、扫描拓扑和复杂 CAD 约束不在首版操作集合中。

## Caustica 交接与验收

主要结果在 `export/model.blend`、`export/model.glb`、`export/cameras.json`。
`report.html` 显示参考图、前景 mask、最佳 Blender 渲染；每个候选的 `request.json` 和 `blender.log` 可复核。
满足米制尺度且参考图尺寸相同时，生成 `export/scene-fit-project.json` 及私有 assets/reference 文件。
该项目只有材质颜色、粗糙度和灯光强度参数，没有几何参数；默认只是中性起点，可扩展透明材质、灯光位置等外观参数。
仍应核对投影、UV、法线、部件关系以及尺度，再进行外观拟合：

```powershell
$env:PYTHONPATH = (Resolve-Path bin).Path
python support/python/scene_fit_cli.py validate build-fit/cup-run/export/scene-fit-project.json
python support/python/scene_fit_cli.py optimize build-fit/cup-run/export/scene-fit-project.json `
  --run-dir build-fit/cup-appearance --max-evaluations 30
```

几何轮廓评分没有原照片 RGB 光照项，避免把照片中的材质、反射和灯光当成几何误差。
轮廓一致无法唯一确定遮挡面、内部结构和透明物体壁厚；此类部位仍需结构先验、额外测量或用户检查。
模型调用和预算是有限的；没有承诺输入任意图片就一定恢复真实几何。

验证命令：

```powershell
python -m unittest discover -s support/python/tests -p test_blender_fit.py -v
python support/python/tests/blender_fit_acceptance.py --output-dir build-fit/blender-acceptance

# 额外验证 Caustica 实际导入后的多视角轮廓，需要本机扩展与 GPU。
$env:PYTHONPATH = (Resolve-Path bin).Path
python support/python/tests/blender_fit_handoff_test.py `
  build-fit/blender-acceptance/run/export/scene-fit-project.json --output-dir build-fit/blender-handoff-check
```

验收实际启动 Blender，从 COLMAP 生成多视角合成参考，验证非中心主点和非等焦距的投影，
执行 HTTP 模型适配器（明确使用本地 mock 服务）、真实 mesh 尺寸修改、较差模型建议回退、保留视角验证与 GLB 导出。
真实外部视觉模型质量需要你配置 API key 后用真实数据验证；本机没有密钥，因此不声称已验证在线建模效果。

本机合成验收：Blender 5.2.1 LTS，COLMAP 投影最大误差 0.0000082 像素，训练误差 0.0769 → 0，
保留视角误差 0。独立 Caustica GPU 重新载入 GLB，三个视角轮廓 IoU 分别为 0.9744、0.9744、0.9718。
Blender alpha 与 Caustica 光线采样的边缘像素存在差异，这些数字只证明测试场景的交接投影一致。
