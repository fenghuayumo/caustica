"""Reusable camera/mesh input, conservative visibility, UV registration and provenance."""
from __future__ import annotations
import hashlib,json,sys,time,shutil
from pathlib import Path
import cv2
import numpy as np
from PIL import Image
from .registration import patch_composite,fill_missing


def read(path):return json.loads(Path(path).read_text(encoding='utf-8'))
def write(path,value):Path(path).write_text(json.dumps(value,ensure_ascii=False,indent=2),encoding='utf-8')
def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def camera_basis(view):
    cam=view['camera'];f=np.array(cam['direction'],float);f/=np.linalg.norm(f)
    x=np.cross(f,cam['up']);x/=np.linalg.norm(x);up=np.cross(x,f)
    return np.array([x,-up,f]),np.array(cam['position'],float)


def project(points,view):
    R,C=camera_basis(view);q=(points-C)@R.T;k=view['camera']['intrinsics']
    z=q[...,2];safe=np.where(z>1e-6,z,1e-6)
    return (q[...,:2]/safe[...,None]*[k['fx'],k['fy']]+[k['cx'],k['cy']]).astype(np.float32),z


def validate(config_path):
    config_path=Path(config_path).resolve();config=read(config_path);base=config_path.parent
    for key in ['images','meshes','views','photara_runtime']:
        config[key]=str((base/config[key]).resolve())
        if not Path(config[key]).exists():raise ValueError(f'Missing {key}: {config[key]}')
    manifest=read(config['views']);views=manifest['views']
    heldout=set(config.get('heldout',[]))
    if not heldout:raise ValueError('Declare heldout filenames explicitly before texture fitting')
    if any(v['source_name'] in heldout for v in views):raise ValueError('Heldout view appears in texture sources')
    if len({v['id'] for v in views})!=len(views):raise ValueError('Duplicate source view ids')
    for v in views:
        if Path(v['source_name']).name!=v['source_name']:raise ValueError('Photo source_name must be a filename')
        if not (Path(config['images'])/v['source_name']).is_file():raise ValueError(f'Missing photo {v["source_name"]}')
        camera_basis(v)
        k=v['camera']['intrinsics']
        if min(k['fx'],k['fy'])<=0:raise ValueError('Invalid intrinsics')
    objects=config['objects']
    if len({o['name'] for o in objects})!=len(objects):raise ValueError('Duplicate texture object names')
    for o in objects:
        if Path(o['name']).name!=o['name'] or o['name'] in ['.','..']:raise ValueError('Invalid object name')
        if o['mode'] not in ['registered_patches','planar_faces']:raise ValueError('Unknown texture mode')
        if not (Path(config['meshes'])/(o['name']+'.npz')).is_file():raise ValueError('Missing object mesh')
        resolution=o.get('resolution',[1024,2048])
        if len(resolution)!=2 or any(not isinstance(n,int) or not 32<=n<=8192 for n in resolution):raise ValueError('Invalid atlas resolution')
        if not 0<=o.get('occlusion_guard_px',24)<=256:raise ValueError('Invalid occlusion guard')
        if not 0<o.get('min_facing',.40)<1:raise ValueError('Invalid minimum facing')
        if o['mode']=='planar_faces':
            if not o.get('faces') or not o.get('solid_occluders'):raise ValueError('Planar faces require corners and solid occluders')
            if any(not (Path(config['meshes'])/(name+'.npz')).is_file() for name in o['solid_occluders']):raise ValueError('Missing planar occluder')
    for path in Path(config['meshes']).glob('*.npz'):
        with np.load(path) as m:
            positions=m['positions'];normals=m['normals'];uv=m['uv'];indices=m['indices']
            if positions.ndim!=2 or positions.shape[1]!=3 or normals.shape!=positions.shape or uv.shape!=(len(positions),2):raise ValueError(f'Invalid mesh attributes: {path.name}')
            if indices.ndim!=2 or indices.shape[1]!=3 or not np.issubdtype(indices.dtype,np.integer) or indices.min()<0 or indices.max()>=len(positions):raise ValueError(f'Invalid mesh indices: {path.name}')
            if not all(np.isfinite(a).all() for a in [positions,normals,uv]):raise ValueError(f'Nonfinite mesh: {path.name}')
    config['_source_config']=str(config_path)
    return config,views


def photo_line_guard(photo,minimum_length=24,radius=8):
    """Reject photographed thin wires independently of their approximate mesh.

    Fruit pores are short; long straight gradients can be basket wires or their
    shadows. This is a conservative heuristic, saved for visual inspection.
    """
    gray=cv2.cvtColor(photo,cv2.COLOR_RGB2GRAY)
    lines=cv2.createLineSegmentDetector(cv2.LSD_REFINE_STD).detect(gray)[0]
    mask=np.zeros(gray.shape,np.uint8)
    if lines is not None:
        for x0,y0,x1,y1 in lines[:,0]:
            if np.hypot(x1-x0,y1-y0)<minimum_length:continue
            cv2.line(mask,(round(float(x0)),round(float(y0))),(round(float(x1)),round(float(y1))),255,2*radius+1)
    return mask>0


def safe_mask(owner,target,photo,kind,guard,line_guard=None):
    target_visible=(owner==target).astype(np.uint8)
    target_visible=cv2.erode(target_visible,np.ones((9,9),np.uint8))>0
    # Guard every foreground competitor, including other fruits, not just wire.
    other=((owner!=target)&(owner!=0)).astype(np.uint8)
    other=cv2.dilate(other,cv2.getStructuringElement(cv2.MORPH_ELLIPSE,(2*guard+1,2*guard+1)))>0
    rgb=photo.astype(np.float32)/255;high=rgb.max(2);low=rgb.min(2);sat=(high-low)/np.maximum(high,.001)
    color=(sat>.18)&(high>.20)
    if kind=='green':color&=(rgb[:,:,1]>rgb[:,:,2]*1.12)&(rgb[:,:,1]>rgb[:,:,0]*.72)
    elif kind=='red':color&=(rgb[:,:,0]>rgb[:,:,2]*1.08)&(rgb[:,:,0]>rgb[:,:,1]*.92)
    elif kind is not None:raise ValueError('Unknown color guard')
    else:color=np.ones(owner.shape,bool)
    result=target_visible&~other&color
    if line_guard is not None:result&=~line_guard
    return result


class Projection:
    def __init__(self,config,views,out):
        self.config,self.views,self.out=config,views,out
        sys.path.insert(0,config['photara_runtime'])
        import photara_drender as p
        self.p=p;self.rasterizer=p.Rasterizer(device_index=config.get('device_index',0))
        self.meshes={f.stem:dict(np.load(f)) for f in sorted(Path(config['meshes']).glob('*.npz'))}
        self.owner_ids={name:i+1 for i,name in enumerate(self.meshes)}
        if len(self.meshes)>254:raise ValueError('Too many mesh ids')
        positions=[];indices=[];ids=[];offset=0
        for name,m in self.meshes.items():
            positions.append(m['positions']);indices.append(m['indices']+offset)
            ids.append(np.full((len(m['positions']),1),self.owner_ids[name],np.float32));offset+=len(m['positions'])
        self.positions=np.concatenate(positions).astype(np.float32)
        self.indices=np.ascontiguousarray(np.concatenate(indices),np.uint32)
        self.ids=np.ascontiguousarray(np.concatenate(ids),np.float32)
        self.photos=[];self.owners=[];self.line_guards=[]

    def visibility(self):
        from photara_drender.colmap import ColmapCamera,ColmapImage,_world_to_clip
        cache=self.out/'visibility';cache.mkdir()
        homogeneous=np.c_[self.positions,np.ones(len(self.positions))].astype(np.float32)
        for i,v in enumerate(self.views):
            photo=np.array(Image.open(Path(self.config['images'])/v['source_name']).convert('RGB'))
            if photo.shape[:2]!=(v['height'],v['width']):raise ValueError('Photo dimensions do not match intrinsics')
            R,C=camera_basis(v);k=v['camera']['intrinsics'];z=(self.positions-C)@R[2];positive=z[z>0]
            if not positive.size:raise ValueError('Scene behind camera')
            cam=ColmapCamera(1,'PINHOLE',v['width'],v['height'],np.array([k['fx'],k['fy'],k['cx'],k['cy']]))
            record=ColmapImage(1,1,v['source_name'],R,-R@C)
            matrix=_world_to_clip(cam,record,max(.0001,float(positive.min())*.5),float(positive.max())*1.5)
            raster,_=self.rasterizer.forward(np.ascontiguousarray(homogeneous@matrix.T),self.indices,photo.shape[:2],output_barycentric_derivatives=False)
            owner=np.rint(self.rasterizer.interpolate_forward(self.ids,self.indices,raster)[:,:,0]).astype(np.uint8)
            line_guard=photo_line_guard(photo)
            Image.fromarray(line_guard.astype(np.uint8)*255).save(cache/(v['id']+'-photo-lines.png'))
            Image.fromarray(owner).save(cache/(v['id']+'-owner.png'));self.photos.append(photo);self.owners.append(owner);self.line_guards.append(line_guard)
            if i%16==15:print(f'VISIBILITY {i+1}/{len(self.views)}',flush=True)

    def surface(self,name,resolution):
        mesh=self.meshes[name];h,w=resolution
        clip=np.c_[mesh['uv']*2-1,np.zeros(len(mesh['uv'])),np.ones(len(mesh['uv']))].astype(np.float32)
        idx=np.ascontiguousarray(mesh['indices'],np.uint32)
        raster,_=self.rasterizer.forward(clip,idx,(h,w),output_barycentric_derivatives=False)
        values=self.rasterizer.interpolate_forward(np.ascontiguousarray(np.c_[mesh['positions'],mesh['normals']],np.float32),idx,raster)
        coverage=raster[:,:,3]>0
        normals=values[:,:,3:];normals/=np.maximum(np.linalg.norm(normals,axis=2,keepdims=True),1e-8)
        return values[:,:,:3],normals,coverage

    def candidates(self,obj):
        positions,normals,coverage=self.surface(obj['name'],tuple(obj.get('resolution',[1024,2048])))
        candidates=[];guard=obj.get('occlusion_guard_px',24)
        for i,(v,photo,owner) in enumerate(zip(self.views,self.photos,self.owners)):
            uv,z=project(positions,v);direction=np.array(v['camera']['position'])-positions
            direction/=np.maximum(np.linalg.norm(direction,axis=2,keepdims=True),1e-8)
            facing=np.sum(direction*normals,axis=2)
            mask=safe_mask(owner,self.owner_ids[obj['name']],photo,obj.get('color_guard'),guard,
                           self.line_guards[i] if obj.get('exclude_photo_lines',True) else None)
            mx=np.ascontiguousarray(uv[:,:,0]);my=np.ascontiguousarray(uv[:,:,1])
            image=cv2.remap(photo,mx,my,cv2.INTER_CUBIC,borderMode=cv2.BORDER_CONSTANT)
            valid=cv2.remap(mask.astype(np.uint8),mx,my,cv2.INTER_NEAREST,borderMode=cv2.BORDER_CONSTANT)>0
            valid&=coverage&(z>0)&(facing>obj.get('min_facing',.40))
            if valid.sum()<256:continue
            score=float(np.sum(facing[valid]**2))*np.sqrt(max(v.get('sharpness',1),1))
            candidates.append({'image':image,'valid':valid,'score':score,'view':v['source_name'],'view_index':i})
        return candidates,coverage


def bake(config_path,output):
    config,views=validate(config_path);out=Path(output).resolve()
    if out.exists():raise ValueError('Use a fresh output directory to preserve previous results')
    out.mkdir(parents=True);assets=out/'assets';assets.mkdir()
    snapshot=out/'meshes';snapshot.mkdir()
    for f in Path(config['meshes']).glob('*.npz'):shutil.copy2(f,snapshot/f.name)
    inputs={'config':digest(config['_source_config']),'views':digest(config['views']),
            'meshes':{f.name:digest(f) for f in sorted(Path(config['meshes']).glob('*.npz'))},
            'photos':{v['source_name']:digest(Path(config['images'])/v['source_name']) for v in views}}
    write(out/'inputs.json',inputs);write(out/'config-resolved.json',config)
    write(out/'views.json',{'views':views,'heldout':config['heldout']})
    report={'status':'running','source_view_count':len(views),'heldout':config['heldout'],'objects':{},
            'coverage_measure':'fraction of covered UV atlas texels, not world surface area',
            'limitations':['Approximate mesh correspondence','Source photograph illumination remains baked',
                           'Unobserved regions are inferred, not measured','Rejected registration reduces coverage']}
    write(out/'summary.json',report)
    projection=Projection(config,views,out);projection.visibility()
    for obj in config['objects']:
        name=obj['name'];start=time.perf_counter();print('TEXTURE',name,flush=True)
        if obj['mode']=='planar_faces':
            from .planar import bake_faces
            image,observed,coverage,source,evidence=bake_faces(projection,obj)
        else:
            candidates,coverage=projection.candidates(obj)
            image,observed,source,evidence=patch_composite(candidates,min_patch=obj.get('min_patch_texels',8192),max_shift=obj.get('max_registration_shift',32),
                max_sources=obj.get('max_source_views',8),min_extent=obj.get('min_patch_extent',48))
            observed&=coverage;source[~observed]=-1
            image=fill_missing(image,observed,coverage)
        Image.fromarray(image).save(assets/(name+'-color.png'))
        Image.fromarray(observed.astype(np.uint8)*255).save(assets/(name+'-observed.png'))
        np.savez_compressed(assets/(name+'-provenance.npz'),observed=observed,coverage=coverage,source_view=source)
        report['objects'][name]={'mode':obj['mode'],'resolution':[image.shape[1],image.shape[0]],
            'observed_surface_fraction':float(observed[coverage].mean()),'inferred_surface_fraction':float((~observed)[coverage].mean()),
            'sources':evidence,'color_sha256':digest(assets/(name+'-color.png')),'seconds':time.perf_counter()-start}
        write(out/'summary.json',report);print('FINISHED',name,report['objects'][name]['observed_surface_fraction'],flush=True)
    report['status']='complete';write(out/'summary.json',report)
    from .report import generate
    generate(out)
    return report
