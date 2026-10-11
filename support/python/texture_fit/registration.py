"""Register projected UV images before they are allowed to contribute pixels."""
from __future__ import annotations
import cv2
import numpy as np


def remap(image, matrix, size, *, mask=False):
    return cv2.warpPerspective(image, matrix, size, flags=cv2.INTER_NEAREST if mask else cv2.INTER_CUBIC,
                               borderMode=cv2.BORDER_CONSTANT, borderValue=0)


def align(reference, candidate, reference_mask, candidate_mask, *, max_shift=32):
    """Return candidate -> reference homography and measured registration evidence.

    Work at thumbnail resolution; coordinates in the returned transform belong to
    the input images. No unsupported flow is extrapolated into unseen regions.
    """
    h,w=reference_mask.shape;scale=min(1.,512/w);size=(round(w*scale),round(h*scale))
    a=cv2.resize(reference,size);b=cv2.resize(candidate,size)
    am=cv2.resize(reference_mask.astype(np.uint8)*255,size,interpolation=cv2.INTER_NEAREST)
    bm=cv2.resize(candidate_mask.astype(np.uint8)*255,size,interpolation=cv2.INTER_NEAREST)
    gray=lambda im:cv2.cvtColor(im,cv2.COLOR_RGB2GRAY)
    sift=cv2.SIFT_create(nfeatures=2500,contrastThreshold=.015)
    ka,da=sift.detectAndCompute(gray(a),am);kb,db=sift.detectAndCompute(gray(b),bm)
    evidence={'accepted':False,'method':'no reliable registration','overlap_texels':int((reference_mask&candidate_mask).sum())}
    matrix=None
    if da is not None and db is not None and len(da)>1:
        pairs=cv2.BFMatcher().knnMatch(db,da,k=2)
        good=[m for pair in pairs if len(pair)==2 for m,n in [pair] if m.distance<.72*n.distance]
        if len(good)>=12:
            src=np.float32([kb[m.queryIdx].pt for m in good]);dst=np.float32([ka[m.trainIdx].pt for m in good])
            affine,inliers=cv2.estimateAffinePartial2D(src,dst,method=cv2.RANSAC,ransacReprojThreshold=2,maxIters=3000)
            if affine is not None and int(inliers.sum())>=10 and float(inliers.mean())>=.5:
                matrix=np.vstack([affine,[0,0,1]])
                evidence.update(method='SIFT/RANSAC similarity',matches=len(good),inliers=int(inliers.sum()))
    if matrix is None:
        overlap=(am>0)&(bm>0)
        if overlap.sum()<max(500,.04*am.size):return None,evidence
        ga=gray(a).astype(np.float32)/255;gb=gray(b).astype(np.float32)/255
        if min(float(ga[overlap].std()),float(gb[overlap].std()))<.035:return None,evidence
        try:
            coefficient,warp=cv2.findTransformECC(ga,gb,np.eye(2,3,dtype=np.float32),cv2.MOTION_TRANSLATION,
                (cv2.TERM_CRITERIA_EPS|cv2.TERM_CRITERIA_COUNT,80,1e-5),overlap.astype(np.uint8)*255,5)
            if coefficient<.72:return None,evidence
            matrix=np.linalg.inv(np.vstack([warp,[0,0,1]]))
            evidence.update(method='masked ECC translation',ecc=float(coefficient))
        except cv2.error:return None,evidence
    corners=np.float32([[0,0],[size[0]-1,0],[size[0]-1,size[1]-1],[0,size[1]-1]])
    shift=np.linalg.norm(cv2.perspectiveTransform(corners[None],matrix)[0]-corners,axis=1).max()
    if shift>max_shift or not .85<np.linalg.det(matrix[:2,:2])<1.18:
        evidence['rejection']='transform exceeds geometric bounds';return None,evidence
    mapped=remap(b,matrix,size);valid=(remap(bm,matrix,size,mask=True)>0)&(am>0)
    if valid.sum()<500:return None,evidence
    x=gray(a)[valid].astype(float);y=gray(mapped)[valid].astype(float)
    if min(x.std(),y.std())<3:return None,evidence
    corr=float(np.corrcoef(x,y)[0,1])
    if not np.isfinite(corr) or corr<.55:
        evidence.update(rejection='registered overlap inconsistent',correlation=corr);return None,evidence
    # Similar overall shading is not proof that skin pores / mottling correspond.
    ga=gray(a).astype(np.float32);gb=gray(mapped).astype(np.float32)
    da=ga-cv2.GaussianBlur(ga,(0,0),3);db=gb-cv2.GaussianBlur(gb,(0,0),3)
    interior=cv2.erode(valid.astype(np.uint8),np.ones((7,7),np.uint8))>0
    if interior.sum()<300:return None,evidence
    detail=float(np.corrcoef(da[interior],db[interior])[0,1])
    if not np.isfinite(detail) or detail<.20:
        evidence.update(rejection='skin-detail correspondence not verified',detail_correlation=detail);return None,evidence
    S=np.diag([scale,scale,1.]);matrix=np.linalg.inv(S)@matrix@S
    evidence.update(accepted=True,correlation=corr,detail_correlation=detail,max_thumbnail_shift=float(shift),matrix=matrix.tolist())
    return matrix,evidence


def patch_composite(candidates, *, min_patch=256, max_shift=32,max_sources=64,min_extent=1):
    """One photograph per observed texel. Failed registration may not fill holes."""
    if not candidates:raise ValueError('No safe source pixels for this object')
    candidates=sorted(candidates,key=lambda c:c['score'],reverse=True)
    master=candidates[0];image=master['image'].copy();known=master['valid'].copy()
    source=np.full(known.shape,-1,np.int16);source[known]=master['view_index']
    evidence=[{'view':master['view'],'method':'master','accepted':True,'new_texels':int(known.sum())}]
    source_count=1
    for c in candidates[1:]:
        if source_count>=max_sources:break
        if int((c['valid']&~known).sum())<min_patch:continue
        H,record=align(image,c['image'],known,c['valid'],max_shift=max_shift)
        record['view']=c['view'];evidence.append(record)
        if H is None:continue
        size=(image.shape[1],image.shape[0]);other=remap(c['image'],H,size);valid=remap(c['valid'].astype(np.uint8),H,size,mask=True)>0
        overlap=valid&known
        gain=np.clip(np.median(image[overlap],axis=0)/np.maximum(np.median(other[overlap],axis=0),1),.85,1.15)
        add=valid&~known
        count,labels,stats,_=cv2.connectedComponentsWithStats(add.astype(np.uint8),8)
        sizes=stats[:,cv2.CC_STAT_AREA];sizes[0]=0
        accepted=(sizes>=min_patch)&(stats[:,cv2.CC_STAT_WIDTH]>=min_extent)&(stats[:,cv2.CC_STAT_HEIGHT]>=min_extent)
        add=accepted[labels]&add
        if not add.any():record['new_texels']=0;continue
        image[add]=np.clip(other[add]*gain,0,255).astype(np.uint8)
        known|=add;source[add]=c['view_index'];record.update(new_texels=int(add.sum()),gain=gain.tolist())
        source_count+=1
    return image,known,source,evidence


def fill_missing(image, observed, coverage):
    """Fill without introducing occluder samples; never promote inferred pixels."""
    if not observed.any():raise ValueError('Cannot infer material without any observations')
    # Clear rejected photo pixels before downsampling, so they cannot seed inpaint.
    clean=image.copy();median=np.median(image[observed],axis=0).astype(np.uint8);clean[~observed]=median
    h,w=observed.shape;size=(min(w,512),min(h,256))
    small=cv2.resize(clean,size,interpolation=cv2.INTER_AREA)
    unknown=cv2.resize((~observed).astype(np.uint8)*255,size,interpolation=cv2.INTER_NEAREST)
    filled=cv2.inpaint(small,unknown,4,cv2.INPAINT_TELEA)
    filled=cv2.resize(filled,(w,h),interpolation=cv2.INTER_CUBIC);clean[~observed]=filled[~observed]
    return clean
