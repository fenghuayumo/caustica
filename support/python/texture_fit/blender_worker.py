"""Blender-only world mesh export and texture application; never moves geometry."""
import argparse,json,sys
from pathlib import Path
import bpy
import numpy as np

parser=argparse.ArgumentParser();parser.add_argument('--mode',choices=['export','apply','render'],required=True)
parser.add_argument('--model',required=True);parser.add_argument('--output',required=True);parser.add_argument('--textures')
parser.add_argument('--views');parser.add_argument('--max-dimension',type=int,default=960);parser.add_argument('--samples',type=int,default=24)
args=parser.parse_args(sys.argv[sys.argv.index('--')+1:])
out=Path(args.output);out.mkdir(parents=True,exist_ok=True)
bpy.ops.wm.open_mainfile(filepath=str(Path(args.model).resolve()))
objects=[obj for obj in bpy.context.scene.objects if obj.type=='MESH']

def arrays(obj):
        mesh=obj.data;mesh.calc_loop_triangles();normal_matrix=obj.matrix_world.to_3x3().inverted().transposed()
        positions=[];normals=[];uv=[];indices=[];layer=mesh.uv_layers.active
        for tri in mesh.loop_triangles:
            row=[]
            for loop_index in tri.loops:
                loop=mesh.loops[loop_index];positions.append(tuple(obj.matrix_world@mesh.vertices[loop.vertex_index].co))
                normal=(normal_matrix@mesh.corner_normals[loop_index].vector).normalized();normals.append(tuple(normal))
                v=layer.data[loop_index].uv if layer is not None else (0,0);uv.append((v[0],1-v[1]));row.append(len(positions)-1)
            indices.append(row)
        return dict(positions=np.array(positions,np.float32),normals=np.array(normals,np.float32),uv=np.array(uv,np.float32),indices=np.array(indices,np.uint32))

if args.mode=='render':
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
    from blender_fit.worker import camera
    views=json.loads(Path(args.views).read_text(encoding='utf-8'))['views']
    scene=bpy.context.scene;scene.cycles.samples=args.samples
    for i,view in enumerate(views):
        camera(view);scene.render.resolution_percentage=max(1,min(100,round(100*args.max_dimension/max(view['width'],view['height']))))
        scene.render.filepath=str(out/(view['id']+'-render.png'));bpy.ops.render.render(write_still=True)
        print('REVIEW',i+1,'/',len(views),flush=True)
elif args.mode=='export':
    for obj in objects:np.savez(out/(obj.name+'.npz'),**arrays(obj))
else:
    textures=Path(args.textures);summary=json.loads((textures/'summary.json').read_text(encoding='utf-8'))
    if summary['status']!='complete':raise ValueError('Texture fitting did not complete')
    missing=set(summary['objects'])-{obj.name for obj in objects}
    if missing:raise ValueError('Texture objects missing from blend: '+str(missing))
    for obj in objects:
        if obj.name not in summary['objects']:continue
        expected_path=textures/'meshes'/(obj.name+'.npz')
        if not expected_path.is_file():raise ValueError('Missing baked geometry snapshot for '+obj.name)
        expected=np.load(expected_path);current=arrays(obj)
        for key in ['positions','normals','uv','indices']:
            if expected[key].shape!=current[key].shape or not np.allclose(expected[key],current[key],atol=2e-6,rtol=1e-6):
                raise ValueError('Texture geometry / UV mismatch: '+obj.name+' '+key)
        mat=obj.data.materials[0];nodes=mat.node_tree.nodes
        for node in list(nodes):
            if node.type=='TEX_IMAGE':nodes.remove(node)
        image=bpy.data.images.load(str(textures/'assets'/(obj.name+'-color.png')),check_existing=False)
        image.colorspace_settings.name='sRGB';node=nodes.new('ShaderNodeTexImage');node.image=image
        bsdf=next(n for n in nodes if n.type=='BSDF_PRINCIPLED');mat.node_tree.links.new(node.outputs['Color'],bsdf.inputs['Base Color'])
    assets=out/'assets';assets.mkdir(exist_ok=True)
    for obj in objects:
        bpy.ops.object.select_all(action='DESELECT');obj.select_set(True);bpy.context.view_layer.objects.active=obj
        bpy.ops.export_scene.gltf(filepath=str(assets/(obj.name+'.glb')),export_format='GLB',use_selection=True,export_yup=True)
    bpy.ops.object.select_all(action='DESELECT')
    for obj in objects:obj.select_set(True)
    bpy.ops.export_scene.gltf(filepath=str(out/'model.glb'),export_format='GLB',use_selection=True,export_yup=True)
    bpy.ops.file.pack_all();bpy.ops.wm.save_as_mainfile(filepath=str(out/'model.blend'))
