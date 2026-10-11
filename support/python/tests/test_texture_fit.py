import json,sys,tempfile,unittest
from pathlib import Path
import cv2
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from texture_fit.registration import align,fill_missing,patch_composite
from texture_fit.pipeline import safe_mask,validate,photo_line_guard
from texture_fit.seams import smooth_patch_offsets


class TextureFitTests(unittest.TestCase):
    def test_seam_refinement_reduces_color_step_without_flattening_detail(self):
        y,x=np.indices((128,256));detail=3*np.sin(x*1.5)+2*np.sin(y*.7)
        image=np.repeat((100+detail)[:,:,None],3,axis=2);image[:,128:]+=35;image=image.astype(np.uint8)
        labels=np.zeros((128,256),np.int16);labels[:,128:]=1
        fixed,mask,_=smooth_patch_offsets(image,labels,strength=.9,band=3)
        before=float(np.abs(image[:,129].astype(float)-image[:,126]).mean())
        after=float(np.abs(fixed[:,129].astype(float)-fixed[:,126]).mean())
        self.assertLess(after,before*.5);self.assertTrue(mask[:,128].all())
        self.assertGreater(float(fixed[10:110,10:80,0].std()),1)
    def test_registration_corrects_shift_and_rejects_unrelated_image(self):
        rng=np.random.default_rng(17)
        a=cv2.GaussianBlur(rng.integers(0,256,(256,320,3),dtype=np.uint8),(3,3),0)
        b=cv2.warpAffine(a,np.float32([[1,0,7],[0,1,-4]]),(320,256))
        mask=np.zeros(a.shape[:2],bool);mask[20:-20,20:-20]=True
        H,evidence=align(a,b,mask,mask)
        self.assertIsNotNone(H);self.assertTrue(evidence['accepted'])
        np.testing.assert_allclose(H[:2,2],[-7,4],atol=1)
        other=rng.integers(0,256,a.shape,dtype=np.uint8)
        H,evidence=align(a,other,mask,mask);self.assertIsNone(H)

    def test_fill_cannot_seed_from_rejected_pixels_or_promote_observation(self):
        image=np.full((100,120,3),[30,180,40],np.uint8);mask=np.ones((100,120),bool);mask[25:75,30:90]=False
        image[~mask]=[240,5,5]
        filled=fill_missing(image,mask,np.ones(mask.shape,bool))
        np.testing.assert_array_equal(filled[mask],image[mask])
        self.assertLess(int(filled[50,60,0]),60);self.assertGreater(int(filled[50,60,1]),140)
        self.assertFalse(mask[50,60])

    def test_similar_shading_without_corresponding_detail_is_rejected(self):
        rng=np.random.default_rng(45);shape=(256,320)
        y,x=np.indices(shape);gradient=(80+.3*x+.15*y).astype(np.float32)
        a=np.clip(gradient+rng.normal(0,2,shape),0,255).astype(np.uint8)
        b=np.clip(gradient+rng.normal(0,2,shape),0,255).astype(np.uint8)
        a=np.repeat(a[:,:,None],3,axis=2);b=np.repeat(b[:,:,None],3,axis=2)
        mask=np.zeros(shape,bool);mask[20:-20,20:-20]=True
        H,record=align(a,b,mask,mask);self.assertIsNone(H)

    def test_neighbouring_fruit_is_guarded_as_well_as_wire(self):
        owner=np.ones((100,100),np.uint8);owner[:,65:]=2
        photo=np.full((100,100,3),[60,170,40],np.uint8)
        valid=safe_mask(owner,1,photo,'green',10)
        self.assertTrue(valid[50,30]);self.assertFalse(valid[50,60]);self.assertFalse(valid[50,80])

    def test_failed_registration_does_not_fill_from_another_view(self):
        rng=np.random.default_rng(3);shape=(100,120)
        master=np.full((*shape,3),[50,150,40],np.uint8);candidate=rng.integers(0,256,master.shape,dtype=np.uint8)
        a=np.zeros(shape,bool);a[:,:60]=True;b=np.zeros(shape,bool);b[:,60:]=True
        result,known,source,evidence=patch_composite([
            dict(image=master,valid=a,score=2,view='a',view_index=4),dict(image=candidate,valid=b,score=1,view='b',view_index=8)])
        self.assertFalse(known[:,60:].any());self.assertTrue((source[:,60:]==-1).all())
        np.testing.assert_array_equal(result[a],master[a])

    def test_photo_wire_is_rejected_even_when_geometry_mask_is_offset(self):
        photo=np.full((120,180,3),[70,180,40],np.uint8)
        cv2.line(photo,(15,60),(160,60),(170,185,160),3)
        owner=np.ones(photo.shape[:2],np.uint8)
        guard=photo_line_guard(photo)
        valid=safe_mask(owner,1,photo,'green',5,guard)
        self.assertFalse(valid[60,80]);self.assertTrue(valid[25,80])

    def test_heldout_is_rejected_before_loading_renderer(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'images').mkdir();(root/'meshes').mkdir();(root/'runtime').mkdir()
            (root/'views.json').write_text(json.dumps({'views':[{'id':'test','source_name':'held.jpg'}]}))
            config={'images':'images','meshes':'meshes','views':'views.json','photara_runtime':'runtime','heldout':['held.jpg'],'objects':[]}
            (root/'config.json').write_text(json.dumps(config))
            with self.assertRaisesRegex(ValueError,'Heldout'):validate(root/'config.json')


if __name__=='__main__':unittest.main()
