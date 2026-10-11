"""Canonical per-face texture reconstruction for boxes / book covers and spines."""
import cv2
import numpy as np
from .pipeline import project
from .registration import patch_composite,fill_missing


def bake_faces(projection,obj):
    n=obj.get('face_resolution',1024);cols=obj.get('atlas_columns',3);faces=obj['faces'];rows=(len(faces)+cols-1)//cols
    atlas=np.zeros((rows*n,cols*n,3),np.uint8);observed=np.zeros(atlas.shape[:2],bool);source=np.full(atlas.shape[:2],-1,np.int16)
    evidence=[];dest=np.float32([[0,0],[n-1,0],[n-1,n-1],[0,n-1]])
    names=obj['solid_occluders'];points=np.concatenate([projection.meshes[name]['positions'] for name in names])
    guards=[];radius=obj.get('occlusion_guard_px',64)
    for photo,v in zip(projection.photos,projection.views):
        uv,z=project(points,v)
        mask=np.zeros(photo.shape[:2],np.uint8)
        if (z>0).any():cv2.fillConvexPoly(mask,np.rint(cv2.convexHull(uv[z>0])).astype(np.int32),255)
        mask=cv2.dilate(mask,cv2.getStructuringElement(cv2.MORPH_ELLIPSE,(2*radius+1,2*radius+1)))
        guards.append(mask)
    for j,face in enumerate(faces):
        known=np.zeros((n,n),bool);sources=np.full((n,n),-1,np.int16);records=[]
        if face.get('inferred')=='paper':
            y=np.arange(n)[:,None];lines=3*np.sin(y*1.3)
            image=np.broadcast_to(np.clip(np.array([211,209,198])[None,None,:]+lines[:,:,None],0,255),(n,n,3)).astype(np.uint8)
            records=[{'method':'inferred paper edges'}]
        elif face.get('inferred')=='binding':
            image=np.full((n,n,3),[48,49,50],np.uint8);records=[{'method':'inferred hidden binding'}]
        else:
            corners=np.array(face['corners'],float);normal=np.array(face['normal'],float);normal/=np.linalg.norm(normal)
            candidates=[]
            for i,(photo,v,guard) in enumerate(zip(projection.photos,projection.views,guards)):
                direction=np.array(v['camera']['position'])-corners.mean(0);direction/=np.linalg.norm(direction)
                facing=float(normal@direction)
                if facing<.25:continue
                uv,z=project(corners,v)
                if (z<=0).any():continue
                annotations=obj.get('annotations',{}).get(v['id'],{})
                for k,corner_id in enumerate(face['corner_ids']):
                    if str(corner_id) in annotations:uv[k]=annotations[str(corner_id)]
                if abs(cv2.contourArea(uv))<1000:continue
                H=cv2.getPerspectiveTransform(uv,dest)
                image=cv2.warpPerspective(photo,H,(n,n),flags=cv2.INTER_CUBIC)
                valid=cv2.warpPerspective(255-guard,H,(n,n),flags=cv2.INTER_NEAREST,borderValue=0)
                valid=cv2.erode(valid,np.ones((9,9),np.uint8))>0;valid[:6]=False;valid[-6:]=False;valid[:,:6]=False;valid[:,-6:]=False
                if valid.mean()<.02:continue
                candidates.append({'image':image,'valid':valid,'score':float(valid.sum())*facing*facing*np.sqrt(max(v.get('sharpness',1),1)),
                                   'view':v['source_name'],'view_index':i})
            image,known,sources,records=patch_composite(candidates,min_patch=100,max_shift=obj.get('max_registration_shift',43))
            image=fill_missing(image,known,np.ones(known.shape,bool))
        row,col=divmod(j,cols);tile=np.s_[row*n:(row+1)*n,col*n:(col+1)*n]
        atlas[tile]=image;observed[tile]=known;source[tile]=sources
        evidence.append({'face':j,'observed_fraction':float(known.mean()),'sources':records})
    return atlas,observed,np.ones(observed.shape,bool),source,evidence
