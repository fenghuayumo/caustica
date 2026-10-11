#!/usr/bin/env python3
"""Registered multi-view texture fitting, independent of geometry and lighting fits."""
import argparse,json,subprocess,sys
from pathlib import Path


def main():
    parser=argparse.ArgumentParser(description=__doc__);commands=parser.add_subparsers(dest='command',required=True)
    validate=commands.add_parser('validate');validate.add_argument('config')
    bake=commands.add_parser('bake');bake.add_argument('config');bake.add_argument('--output-dir',required=True)
    report=commands.add_parser('report');report.add_argument('directory')
    seams=commands.add_parser('refine-seams');seams.add_argument('--textures',required=True);seams.add_argument('--output-dir',required=True)
    seams.add_argument('--strength',type=float,default=.7);seams.add_argument('--band',type=int,default=5)
    for command in ['export-meshes','apply','review']:
        p=commands.add_parser(command);p.add_argument('--model',required=True);p.add_argument('--output-dir',required=True);p.add_argument('--blender',required=True)
        if command=='apply':p.add_argument('--textures',required=True)
        if command=='review':
            p.add_argument('--config',required=True);p.add_argument('--max-dimension',type=int,default=960);p.add_argument('--samples',type=int,default=24)
    args=parser.parse_args()
    if args.command=='refine-seams':
        from texture_fit.seams import refine
        if not 0<=args.strength<=1 or not 1<=args.band<=16:parser.error('strength must be 0..1 and band 1..16')
        refine(args.textures,args.output_dir,strength=args.strength,band=args.band);return
    if args.command=='report':
        from texture_fit.report import generate
        generate(args.directory);return
    if args.command in ['validate','bake']:
        from texture_fit.pipeline import validate,bake
        if args.command=='validate':
            config,views=validate(args.config);print(json.dumps({'status':'valid','views':len(views),'objects':len(config['objects'])}));return
        report=bake(args.config,args.output_dir);print(json.dumps({'status':report['status'],'objects':len(report['objects'])}));return
    out=Path(args.output_dir).resolve()
    if out.exists():parser.error('Use a fresh export output directory')
    out.mkdir(parents=True)
    command=[args.blender,'--background','--threads','0' if args.command=='review' else '4','--python-exit-code','1','--python',str(Path(__file__).parent/'texture_fit/blender_worker.py'),
             '--','--mode',{'export-meshes':'export','apply':'apply','review':'render'}[args.command],'--model',str(Path(args.model).resolve()),'--output',str(out)]
    if args.command=='apply':command+=['--textures',str(Path(args.textures).resolve())]
    if args.command=='review':
        from texture_fit.pipeline import validate
        config,views=validate(args.config)
        command+=['--views',config['views'],'--max-dimension',str(args.max_dimension),'--samples',str(args.samples)]
    with (out/'blender.log').open('w',encoding='utf-8') as log:result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
    if result.returncode:raise RuntimeError(f'Blender operation failed: {out / "blender.log"}')
    if args.command=='review':
        from PIL import Image
        import html
        parts=['<!doctype html><meta charset="utf-8"><title>Texture multi-view review</title><style>body{background:#222;color:#eee;font:16px system-ui}img{width:48%;vertical-align:top}section{border-top:1px solid #555;padding:16px}</style><h1>原图 / 配准纹理渲染</h1>']
        for view in views:
            im=Image.open(Path(config['images'])/view['source_name']).convert('RGB');im.thumbnail((args.max_dimension,args.max_dimension));im.save(out/(view['id']+'-reference.jpg'),quality=95)
            name=html.escape(view['id']);parts.append(f'<section><p>{name} · {html.escape(view["source_name"])}</p><img src="{name}-reference.jpg"><img src="{name}-render.png"></section>')
        (out/'index.html').write_text(''.join(parts),encoding='utf-8')
        (out/'manifest.json').write_text(json.dumps({'views':views,'model':str(Path(args.model).resolve())},indent=2),encoding='utf-8')
    print(json.dumps({'status':'complete','output':str(out)}))


if __name__=='__main__':main()
