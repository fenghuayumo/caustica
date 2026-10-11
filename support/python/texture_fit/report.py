"""Portable inspection of sampled / inferred texture and camera provenance."""
import html,json
from pathlib import Path
import numpy as np
from PIL import Image


def generate(directory):
    root=Path(directory);summary=json.loads((root/'summary.json').read_text(encoding='utf-8'))
    parts=['<!doctype html><meta charset="utf-8"><title>Registered texture review</title><style>body{background:#181c23;color:#eee;font:16px system-ui;margin:24px}img{max-width:100%;max-height:500px}a{color:#9edaff}section{border-top:1px solid #555;padding:20px 0}</style>',
           '<h1>配准纹理：观测与补全</h1><p>白色 mask 是照片观测，黑色是补全；来源图中灰色是补全，其他颜色区分照片。纹理仍包含原图照明，不能视作准确 base color。</p>',
           '<p><a href="summary.json">配准结果</a> · <a href="inputs.json">输入指纹</a> · <a href="views.json">来源相机</a></p>']
    for name,result in summary['objects'].items():
        data=np.load(root/'assets'/(name+'-provenance.npz'));labels=data['source_view'];rng=np.random.default_rng(19)
        palette=rng.integers(35,240,(summary['source_view_count'],3),dtype=np.uint8);source=np.full((*labels.shape,3),65,np.uint8)
        valid=labels>=0;source[valid]=palette[labels[valid]];Image.fromarray(source).save(root/'assets'/(name+'-sources.png'))
        label=html.escape(name)
        parts.append(f'<section><h2>{label}</h2><p>照片观测 {result["observed_surface_fraction"]:.1%} · 补全 {result["inferred_surface_fraction"]:.1%}</p><img src="assets/{label}-color.png"><p>照片观测 / 补全</p><img src="assets/{label}-observed.png"><p>单像素照片来源</p><img src="assets/{label}-sources.png"></section>')
    (root/'report.html').write_text(''.join(parts),encoding='utf-8')
