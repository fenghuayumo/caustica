"""Photometric seam refinement; keeps UVs, source records and missing-data labels."""
import json,shutil,hashlib
from pathlib import Path
import cv2
import numpy as np
from scipy.fft import dctn,idctn


def boundary(labels):
    seam=np.zeros(labels.shape,bool)
    seam[:,1:]|=labels[:,1:]!=labels[:,:-1];seam[:,:-1]|=labels[:,1:]!=labels[:,:-1]
    seam[1:]|=labels[1:]!=labels[:-1];seam[:-1]|=labels[1:]!=labels[:-1]
    return seam


def smooth_patch_offsets(image,labels,*,strength=.7,band=5):
    """Screened Poisson low-frequency correction; no cross-face atlas mixing."""
    h,w=labels.shape;size=(min(w,512),min(h,256))
    small=cv2.resize(image.astype(np.float32),size,interpolation=cv2.INTER_AREA)
    ids=cv2.resize(labels.astype(np.float32),size,interpolation=cv2.INTER_NEAREST)
    dx=np.diff(small,axis=1);dy=np.diff(small,axis=0)
    dx[ids[:,1:]!=ids[:,:-1]]=0;dy[ids[1:]!=ids[:-1]]=0
    div=np.zeros_like(small);div[:,:-1]-=dx;div[:,1:]+=dx;div[:-1]-=dy;div[1:]+=dy
    y,x=np.indices(ids.shape);eigen=2-2*np.cos(np.pi*x/ids.shape[1])+2-2*np.cos(np.pi*y/ids.shape[0])
    screen=.015
    solved=idctn(dctn(div+screen*small,type=2,norm='ortho')/(eigen[:,:,None]+screen),type=2,norm='ortho')
    correction=cv2.resize((solved-small)*strength,(w,h),interpolation=cv2.INTER_CUBIC)
    corrected=np.clip(image.astype(np.float32)+correction,0,255).astype(np.uint8)
    seam=boundary(labels)
    mask=cv2.dilate(seam.astype(np.uint8),cv2.getStructuringElement(cv2.MORPH_ELLIPSE,(2*band+1,2*band+1)))>0
    # A narrow band may contain subpixel chart-source discontinuities left by the
    # low-frequency solve. These changed texels are explicitly inferred below.
    fixed=cv2.inpaint(cv2.cvtColor(corrected,cv2.COLOR_RGB2BGR),mask.astype(np.uint8)*255,3,cv2.INPAINT_NS)
    return cv2.cvtColor(fixed,cv2.COLOR_BGR2RGB),mask,correction


def refine(source,output,*,strength=.7,band=5):
    source=Path(source).resolve();out=Path(output).resolve()
    summary=json.loads((source/'summary.json').read_text(encoding='utf-8'))
    config=json.loads((source/'config-resolved.json').read_text(encoding='utf-8'))
    objects={o['name']:o for o in config['objects']}
    if summary['status']!='complete':raise ValueError('Input texture run is incomplete')
    if out.exists():raise ValueError('Use a fresh refinement output directory')
    if source in out.parents:raise ValueError('Refinement output cannot be inside its source')
    shutil.copytree(source,out,ignore=shutil.ignore_patterns('visibility'))
    summary['status']='running'
    summary['seam_refinement']={'source':str(source),'strength':strength,'inpaint_band_px':band,
        'method':'screened Poisson low-frequency patch correction and narrow inferred seam bands',
        'geometry_modified':False,'limitations':'Smooths seam offsets; cannot recover missing printed cover or remove all baked lighting'}
    (out/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    for name,obj in summary['objects'].items():
        asset=out/'assets';path=asset/(name+'-color.png');image=cv2.cvtColor(cv2.imread(str(path)),cv2.COLOR_BGR2RGB)
        data=dict(np.load(asset/(name+'-provenance.npz')));labels=data['source_view']
        adjusted=np.zeros(labels.shape,bool);correction=np.zeros(image.shape,np.float32)
        if obj['mode']=='planar_faces':
            # Each book face is independent; retain spine lettering and paper.
            n=objects[name].get('face_resolution',1024);tile=np.s_[:n,:n]
            image[tile],adjusted[tile],correction[tile]=smooth_patch_offsets(image[tile],labels[tile],strength=strength*.5,band=band)
        else:image,adjusted,correction=smooth_patch_offsets(image,labels,strength=strength,band=band)
        adjusted&=data['coverage'];data['seam_inferred']=adjusted;data['low_frequency_correction']=correction.astype(np.float16)
        data['observed'][adjusted]=False;data['source_view'][adjusted]=-1
        cv2.imwrite(str(path),cv2.cvtColor(image,cv2.COLOR_RGB2BGR))
        cv2.imwrite(str(asset/(name+'-observed.png')),data['observed'].astype(np.uint8)*255)
        cv2.imwrite(str(asset/(name+'-seams.png')),adjusted.astype(np.uint8)*255)
        np.savez_compressed(asset/(name+'-provenance.npz'),**data)
        obj.update(color_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            observed_surface_fraction=float(data['observed'][data['coverage']].mean()),
            inferred_surface_fraction=float((~data['observed'])[data['coverage']].mean()),
            seam_refinement_fraction=float(adjusted[data['coverage']].mean()))
    summary['status']='complete'
    (out/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
    from .report import generate
    generate(out)
    return summary
