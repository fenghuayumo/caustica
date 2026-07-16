// Minimal OpenUSD smoke test.
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: smoke_openusd <file.usdc>\n");
        return 2;
    }
    UsdStageRefPtr stage = UsdStage::Open(argv[1]);
    if (!stage)
    {
        std::fprintf(stderr, "UsdStage::Open failed: %s\n", argv[1]);
        return 1;
    }
    size_t meshes = 0;
    for (const UsdPrim& p : stage->Traverse())
        if (p.IsA<UsdGeomMesh>())
            ++meshes;
    std::printf("OK stage=%s meshes=%zu start=%.1f end=%.1f\n",
        argv[1], meshes, stage->GetStartTimeCode(), stage->GetEndTimeCode());
    return 0;
}
