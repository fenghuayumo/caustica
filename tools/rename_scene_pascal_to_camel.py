#!/usr/bin/env python3
"""Rename PascalCase scene APIs to camelCase across first-party sources."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Full Old -> new map for scene APIs (lower first letter only).
RENAMES: list[tuple[str, str]] = [
    ("AddAnimationChannel", "addAnimationChannel"),
    ("AddChannel", "addChannel"),
    ("AnimateOrbit", "animateOrbit"),
    ("AnimateRoll", "animateRoll"),
    ("AnimateSmooth", "animateSmooth"),
    ("AnimateTranslation", "animateTranslation"),
    ("Animate", "animate"),
    ("ApplyAnimationChannel", "applyAnimationChannel"),
    ("ApplyAnimation", "applyAnimation"),
    ("Apply", "apply"),
    ("AttachLightToRoot", "attachLightToRoot"),
    ("AttachRuntimeSceneImport", "attachRuntimeSceneImport"),
    ("BaseLookAt", "baseLookAt"),
    ("BufferOrFallback", "bufferOrFallback"),
    ("BuildOpaqueDrawList", "buildOpaqueDrawList"),
    ("BuildTransparentDrawList", "buildTransparentDrawList"),
    ("BuildViewDrawLists", "buildViewDrawLists"),
    ("Clone", "clone"),
    ("CreateLeaf", "createLeaf"),
    ("CreateMaterial", "createMaterial"),
    ("CreateMeshGeometry", "createMeshGeometry"),
    ("CreateMeshInstance", "createMeshInstance"),
    ("CreateMesh", "createMesh"),
    ("CreateSkinnedMeshFromPrototype", "createSkinnedMeshFromPrototype"),
    ("CreateSkinnedMeshInstance", "createSkinnedMeshInstance"),
    ("DeleteRuntimeSceneNode", "deleteRuntimeSceneNode"),
    ("EnsureCache", "ensureCache"),
    ("ExtractSceneRenderData", "extractSceneRenderData"),
    ("FillConstantBuffer", "fillConstantBuffer"),
    ("FillLightConstants", "fillLightConstants"),
    ("FillLightProbeConstants", "fillLightProbeConstants"),
    ("FillShadowConstants", "fillShadowConstants"),
    ("FinalizeRuntimeSceneMutation", "finalizeRuntimeSceneMutation"),
    ("FindEnvironmentLightEntity", "findEnvironmentLightEntity"),
    ("FindLightControllerInfo", "findLightControllerInfo"),
    ("FindPreferredScene", "findPreferredScene"),
    ("FromKeys", "fromKeys"),
    ("GetActiveUserCamera", "getActiveUserCamera"),
    ("GetAnimationContentFlags", "getAnimationContentFlags"),
    ("GetAnimationDuration", "getAnimationDuration"),
    ("GetAnimationEntities", "getAnimationEntities"),
    ("GetAssetHandle", "getAssetHandle"),
    ("GetAt", "getAt"),
    ("GetAttribute", "getAttribute"),
    ("GetCameraContentFlags", "getCameraContentFlags"),
    ("GetCameraEntities", "getCameraEntities"),
    ("GetCameraViewToWorldMatrix", "getCameraViewToWorldMatrix"),
    ("GetCameraWorldToViewMatrix", "getCameraWorldToViewMatrix"),
    ("GetCascade", "getCascade"),
    ("GetChannels", "getChannels"),
    ("GetContentFlags", "getContentFlags"),
    ("GetDir", "getDir"),
    ("GetDirection", "getDirection"),
    ("GetDistance", "getDistance"),
    ("GetDuration", "getDuration"),
    ("GetEntityWorld", "getEntityWorld"),
    ("GetEnvironmentLightPath", "getEnvironmentLightPath"),
    ("GetFadeRangeInTexels", "getFadeRangeInTexels"),
    ("GetFirstPersonCamera", "getFirstPersonCamera"),
    ("GetGameSettingsNode", "getGameSettingsNode"),
    ("GetGeometryCount", "getGeometryCount"),
    ("GetGeometryData", "getGeometryData"),
    ("GetGeometryInstanceIndex", "getGeometryInstanceIndex"),
    ("GetGeometryInstancesCount", "getGeometryInstancesCount"),
    ("GetGpuResources", "getGpuResources"),
    ("GetInstanceIndex", "getInstanceIndex"),
    ("GetInstanceName", "getInstanceName"),
    ("GetInstance", "getInstance"),
    ("GetJsonData", "getJsonData"),
    ("GetLastUpdateFrameIndex", "getLastUpdateFrameIndex"),
    ("GetLeafPropertyName", "getLeafPropertyName"),
    ("GetLightContentFlags", "getLightContentFlags"),
    ("GetLightDirection", "getLightDirection"),
    ("GetLightEntities", "getLightEntities"),
    ("GetLightPosition", "getLightPosition"),
    ("GetLightType", "getLightType"),
    ("GetLights", "getLights"),
    ("GetLoadingStats", "getLoadingStats"),
    ("GetLocalBoundingBox", "getLocalBoundingBox"),
    ("GetMaterials", "getMaterials"),
    ("GetMaxDistance", "getMaxDistance"),
    ("GetMaxGeometryCountPerMesh", "getMaxGeometryCountPerMesh"),
    ("GetMeshAsset", "getMeshAsset"),
    ("GetMeshContentFlags", "getMeshContentFlags"),
    ("GetMeshInstances", "getMeshInstances"),
    ("GetMeshLocalBounds", "getMeshLocalBounds"),
    ("GetMeshes", "getMeshes"),
    ("GetMesh", "getMesh"),
    ("GetModelName", "getModelName"),
    ("GetModelPose", "getModelPose"),
    ("GetModels", "getModels"),
    ("GetNumberOfCascades", "getNumberOfCascades"),
    ("GetNumberOfPerObjectShadows", "getNumberOfPerObjectShadows"),
    ("GetPerObjectShadow", "getPerObjectShadow"),
    ("GetPointLight", "getPointLight"),
    ("GetPosDirUp", "getPosDirUp"),
    ("GetPosition", "getPosition"),
    ("GetPrototypeMesh", "getPrototypeMesh"),
    ("GetRenderCommands", "getRenderCommands"),
    ("GetRenderData", "getRenderData"),
    ("GetRootEntity", "getRootEntity"),
    ("GetRotationPitch", "getRotationPitch"),
    ("GetRotationYaw", "getRotationYaw"),
    ("GetSampleSettingsNode", "getSampleSettingsNode"),
    ("GetSampler", "getSampler"),
    ("GetSceneBounds", "getSceneBounds"),
    ("GetSceneCameraProjectionParams", "getSceneCameraProjectionParams"),
    ("GetSceneCamera", "getSceneCamera"),
    ("GetSceneTypeFactory", "getSceneTypeFactory"),
    ("GetScene", "getScene"),
    ("GetSkinnedMeshInstances", "getSkinnedMeshInstances"),
    ("GetSpotLight", "getSpotLight"),
    ("GetTargetEntity", "getTargetEntity"),
    ("GetTargetMaterial", "getTargetMaterial"),
    ("GetTargetPosition", "getTargetPosition"),
    ("GetTextureSize", "getTextureSize"),
    ("GetTexture", "getTexture"),
    ("GetThirdPersonCamera", "getThirdPersonCamera"),
    ("GetTranslatedWorldToViewMatrix", "getTranslatedWorldToViewMatrix"),
    ("GetUVRange", "getUVRange"),
    ("GetUp", "getUp"),
    ("GetVertexAttributeDesc", "getVertexAttributeDesc"),
    ("GetViewToWorldMatrix", "getViewToWorldMatrix"),
    ("GetView", "getView"),
    ("GetWorldToUvzwMatrix", "getWorldToUvzwMatrix"),
    ("GetWorldToViewMatrix", "getWorldToViewMatrix"),
    ("HasSceneStructureChanged", "hasSceneStructureChanged"),
    ("HasSceneTransformsChanged", "hasSceneTransformsChanged"),
    ("HasSkinnedMesh", "hasSkinnedMesh"),
    ("InitializeAnimationComponent", "initializeAnimationComponent"),
    ("InitializeCameraComponent", "initializeCameraComponent"),
    ("InitializeLightComponent", "initializeLightComponent"),
    ("InitializeMeshInstanceComponent", "initializeMeshInstanceComponent"),
    ("IsActive", "isActive"),
    ("IsAnimationChannelValid", "isAnimationChannelValid"),
    ("IsAnimationValid", "isAnimationValid"),
    ("IsCurve", "isCurve"),
    ("IsDirectMeshSceneFile", "isDirectMeshSceneFile"),
    ("IsFirstPersonActive", "isFirstPersonActive"),
    ("IsInfiniteLight", "isInfiniteLight"),
    ("IsLitOutOfBounds", "isLitOutOfBounds"),
    ("IsOrthographicCamera", "isOrthographicCamera"),
    ("IsPerspectiveCamera", "isPerspectiveCamera"),
    ("IsSceneCameraActive", "isSceneCameraActive"),
    ("IsThirdPersonActive", "isThirdPersonActive"),
    ("IsVald", "isVald"),
    ("IsValid", "isValid"),
    ("JoystickButtonUpdate", "joystickButtonUpdate"),
    ("JoystickUpdate", "joystickUpdate"),
    ("KeyboardUpdate", "keyboardUpdate"),
    ("LoadAnimations", "loadAnimations"),
    ("LoadBuiltinModel", "loadBuiltinModel"),
    ("LoadCustomData", "loadCustomData"),
    ("LoadFromJsonString", "loadFromJsonString"),
    ("LoadJsonDocument", "loadJsonDocument"),
    ("LoadModelAsync", "loadModelAsync"),
    ("LoadModelFile", "loadModelFile"),
    ("LoadModels", "loadModels"),
    ("LoadRuntimeGltfMeshFile", "loadRuntimeGltfMeshFile"),
    ("LoadRuntimeMeshFile", "loadRuntimeMeshFile"),
    ("LoadRuntimeObjMeshFile", "loadRuntimeObjMeshFile"),
    ("LoadRuntimeUrdfMeshFile", "loadRuntimeUrdfMeshFile"),
    ("LoadSceneEntities", "loadSceneEntities"),
    ("LoadStlFile", "loadStlFile"),
    ("LoadWithThreadPool", "loadWithThreadPool"),
    ("Load", "load"),
    ("LookAt", "lookAt"),
    ("LookTo", "lookTo"),
    ("MapLightControllers", "mapLightControllers"),
    ("MaterialDomainToString", "materialDomainToString"),
    ("MouseButtonUpdate", "mouseButtonUpdate"),
    ("MousePosUpdate", "mousePosUpdate"),
    ("MouseScrollUpdate", "mouseScrollUpdate"),
    ("ProcessNodesRecursive", "processNodesRecursive"),
    ("PublishRenderSnapshot", "publishRenderSnapshot"),
    ("Read", "read"),
    ("RecalculateAnimationDuration", "recalculateAnimationDuration"),
    ("RefreshSceneWorld", "refreshSceneWorld"),
    ("RegisterMeshInstanceEntity", "registerMeshInstanceEntity"),
    ("ResolveCachePath", "resolveCachePath"),
    ("SetAssetHandle", "setAssetHandle"),
    ("SetCameraProperty", "setCameraProperty"),
    ("SetDirection", "setDirection"),
    ("SetDistance", "setDistance"),
    ("SetLastUpdateFrameIndex", "setLastUpdateFrameIndex"),
    ("SetLeafPropertyName", "setLeafPropertyName"),
    ("SetLightDirection", "setLightDirection"),
    ("SetLightPosition", "setLightPosition"),
    ("SetLightProperty", "setLightProperty"),
    ("SetMaxDistance", "setMaxDistance"),
    ("SetMeshProperty", "setMeshProperty"),
    ("SetMoveSpeed", "setMoveSpeed"),
    ("SetPosition", "setPosition"),
    ("SetProperty", "setProperty"),
    ("SetRotateSpeed", "setRotateSpeed"),
    ("SetRotation", "setRotation"),
    ("SetTargetEntity", "setTargetEntity"),
    ("SetTargetPosition", "setTargetPosition"),
    ("SetTransformFromCamera", "setTransformFromCamera"),
    ("SetTransform", "setTransform"),
    ("SetView", "setView"),
    ("Store", "store"),
    ("SwitchToFirstPerson", "switchToFirstPerson"),
    ("SwitchToSceneCamera", "switchToSceneCamera"),
    ("SwitchToThirdPerson", "switchToThirdPerson"),
    ("ToTransform", "toTransform"),
    ("TryGetAnimation", "tryGetAnimation"),
    ("TryGetCamera", "tryGetCamera"),
    ("TryGetDirectionalLightData", "tryGetDirectionalLightData"),
    ("TryGetEnvironmentLightData", "tryGetEnvironmentLightData"),
    ("TryGetLight", "tryGetLight"),
    ("TryGetMeshInstance", "tryGetMeshInstance"),
    ("TryGetOrthographicCameraData", "tryGetOrthographicCameraData"),
    ("TryGetPerspectiveCameraData", "tryGetPerspectiveCameraData"),
    ("TryGetPointLightData", "tryGetPointLightData"),
    ("TryGetSkinnedMesh", "tryGetSkinnedMesh"),
    ("TryGetSpotLightData", "tryGetSpotLightData"),
    ("UnregisterMeshInstanceEntity", "unregisterMeshInstanceEntity"),
    ("UpdateCachedDirection", "updateCachedDirection"),
    ("UpdateCamera", "updateCamera"),
    ("UpdateHierarchy", "updateHierarchy"),
    ("UpdateLightFromControllers", "updateLightFromControllers"),
    ("UpdateWorldToView", "updateWorldToView"),
    ("Write", "write"),
]

# ResourceTracker only — never touch COM/nvrhi AddRef/Release.
RESOURCETRACKER_RENAMES = [
    ("AddRef", "addRef"),
    ("Release", "release"),
]

SKIP_DIR_PARTS = {
    "third_party",
    "External",
    "bin",
    ".git",
    "build",
    "out",
    "ShaderPrecompiled",
    "rhi",  # backend/rhi (nvrhi)
    ".vs",
    "CMakeFiles",
}

EXT = {".h", ".hpp", ".cpp", ".c", ".inl", ".cc", ".cxx"}


def should_skip(path: Path) -> bool:
    parts = set(path.parts)
    if parts & SKIP_DIR_PARTS:
        return True
    # Extra safety for vendored trees
    text = str(path).replace("\\", "/")
    if "/backend/rhi/" in text or "/third_party/" in text:
        return True
    return False


def sort_key(item: tuple[str, str]) -> tuple[int, str]:
    # Longest first so GetMeshInstances is replaced before GetMesh
    return (-len(item[0]), item[0])


def build_patterns(renames: list[tuple[str, str]]) -> list[tuple[re.Pattern[str], str, str]]:
    out: list[tuple[re.Pattern[str], str, str]] = []
    for old, new in sorted(renames, key=sort_key):
        # Word-boundary identifier replace (decls, defs, calls, comments).
        pat = re.compile(rf"\b{re.escape(old)}\b")
        out.append((pat, new, old))
    return out


def transform(text: str, patterns: list[tuple[re.Pattern[str], str, str]]) -> tuple[str, int]:
    total = 0
    for pat, new, _old in patterns:
        text, n = pat.subn(new, text)
        total += n
    return text, total


def collect_files() -> list[Path]:
    roots = [
        ROOT / "caustica" / "caustica",
        ROOT / "caustica" / "Python",
        ROOT / "caustica" / "apps",
        ROOT / "caustica" / "samples",
        ROOT / "application",
        ROOT / "apps",
        ROOT / "samples",
        ROOT / "python",
        ROOT / "support",
    ]
    files: list[Path] = []
    for root in roots:
        if not root.exists():
            continue
        for p in root.rglob("*"):
            if not p.is_file() or p.suffix.lower() not in EXT:
                continue
            if should_skip(p):
                continue
            files.append(p)
    return files


def main() -> int:
    dry = "--dry-run" in sys.argv
    patterns = build_patterns(RENAMES)
    rt_patterns = build_patterns(RESOURCETRACKER_RENAMES)
    files = collect_files()
    changed_files = 0
    total_repls = 0

    for path in files:
        raw_bytes = path.read_bytes()
        newline = b"\r\n" if b"\r\n" in raw_bytes else b"\n"
        raw = raw_bytes.decode("utf-8", errors="surrogateescape")
        # Normalize to \n for regex processing
        raw_norm = raw.replace("\r\n", "\n").replace("\r", "\n")
        new, n = transform(raw_norm, patterns)

        # ResourceTracker AddRef/Release only in tracker + SceneResources
        # (never COM/nvrhi ->AddRef()/->Release()).
        rel = str(path).replace("\\", "/")
        if rel.endswith("/ResourceTracker.h") or rel.endswith("/SceneResources.cpp"):
            tmp = new
            for _pat, repl, old in rt_patterns:
                tmp2, n2 = re.subn(rf"(?<!->)\b{re.escape(old)}\b", repl, tmp)
                n += n2
                tmp = tmp2
            new = tmp

        if n == 0 or new == raw_norm:
            continue
        changed_files += 1
        total_repls += n
        rel_out = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
        print(f"{rel_out}: {n} replacements")
        if not dry:
            out = new.encode("utf-8", errors="surrogateescape")
            if newline == b"\r\n":
                out = out.replace(b"\n", b"\r\n")
            path.write_bytes(out)

    print(f"---\nfiles={changed_files} replacements={total_repls} dry_run={dry}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
