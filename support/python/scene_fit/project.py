"""Versioned projects, bounded scalar bindings and private asset snapshots."""
from __future__ import annotations

import copy
import hashlib
import json
import math
import re
import shutil
import struct
from pathlib import Path

import numpy as np
from PIL import Image


def read_json(path):
    def reject(value):
        raise ValueError(f"Non-finite JSON number: {value}")
    return json.loads(Path(path).read_text(encoding="utf-8"), parse_constant=reject)


def write_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, ensure_ascii=False, allow_nan=False), encoding="utf-8")
    temporary.replace(path)


def number(value, label):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"{label} must be a finite number")
    return float(value)


def identifier(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,79}", value):
        raise ValueError(f"Invalid identifier: {value!r}")
    return value


def inside(root, relative):
    path = (root / relative).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError:
        raise ValueError(f"Asset escapes asset_root: {relative}") from None
    return path


def tree_hash(root):
    digest = hashlib.sha256()
    for path in sorted(Path(root).rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Symlinks are not supported in asset bundles: {path}")
        if path.is_file():
            digest.update(path.relative_to(root).as_posix().encode())
            digest.update(b"\0")
            digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def pointer_parent(document, pointer):
    if not isinstance(pointer, str) or not pointer.startswith("/"):
        raise ValueError("Binding path must be a nonempty JSON pointer")
    tokens = [part.replace("~1", "/").replace("~0", "~") for part in pointer[1:].split("/")]
    node = document
    for token in tokens[:-1]:
        if isinstance(node, list):
            if not token.isdigit():
                raise ValueError("JSON pointer array indices must be nonnegative integers")
            node = node[int(token)]
        else:
            node = node[token]
    key = int(tokens[-1]) if isinstance(node, list) else tokens[-1]
    if isinstance(key, int) and key < 0:
        raise ValueError("Negative array indices are not JSON pointers")
    number(node[key], pointer)
    return node, key


def candidate_hash(root):
    root = Path(root)
    digest = hashlib.sha256()
    for name in ("scene.scene.json", "parameters.json"):
        digest.update((root / name).read_bytes())
    digest.update(tree_hash(root / "assets").encode())
    return digest.hexdigest()


def asset_dependency(root, owner, value):
    if value.startswith("data:"):
        return None
    if ":" in value or Path(value).is_absolute():
        raise ValueError(f"Asset dependencies must be bundle-relative: {owner}: {value}")
    path = inside(root, (owner.parent / value).relative_to(root))
    if not path.is_file():
        raise ValueError(f"Missing asset dependency: {owner}: {value}")
    return path


def material_resources(node, root, owner, *, relocate=False):
    """Material image references are relative to the owning material file."""
    if isinstance(node, dict):
        result = {}
        for key, value in node.items():
            if isinstance(value, str) and value and ("texture" in key.lower() or key.lower() in ("path", "source")):
                path = asset_dependency(root, owner, value)
                if relocate and path is not None:
                    value = path.as_posix()
            result[key] = material_resources(value, root, owner, relocate=relocate)
        return result
    if isinstance(node, list):
        return [material_resources(value, root, owner, relocate=relocate) for value in node]
    return node


def validate_bundle(root):
    for path in root.rglob("*"):
        if path.name.endswith(".material.json"):
            material_resources(read_json(path), root, path)
        elif path.suffix.lower() in (".gltf", ".glb"):
            if path.suffix.lower() == ".gltf":
                data = read_json(path)
            else:
                raw = path.read_bytes()
                if len(raw) < 20 or raw[:4] != b"glTF":
                    raise ValueError(f"Invalid GLB: {path}")
                size, kind = struct.unpack_from("<II", raw, 12)
                if kind != 0x4E4F534A or size > len(raw) - 20:
                    raise ValueError(f"GLB has no valid JSON chunk: {path}")
                data = json.loads(raw[20:20 + size].decode("utf-8"))
            for entry in data.get("buffers", []) + data.get("images", []):
                if "uri" in entry:
                    asset_dependency(root, path, entry["uri"])


class Project:
    def __init__(self, path):
        self.path = Path(path).resolve()
        self.data = read_json(self.path)
        data = self.data
        if data.get("schema_version") != 1:
            raise ValueError("Expected schema_version=1")
        if data.get("coordinates") != "caustica_y_up_meters":
            raise ValueError("Explicit caustica_y_up_meters coordinate convention is required")
        if data.get("comparison_space") != "display_rgb":
            raise ValueError("v1 compares fixed display RGB; expected comparison_space=display_rgb")
        self.root = self.path.parent
        self.asset_root = (self.root / data["asset_root"]).resolve()
        if not self.asset_root.is_dir():
            raise ValueError("asset_root must be a private existing directory")
        tree_hash(self.asset_root)
        validate_bundle(self.asset_root)
        self.assets = data.get("assets", {})
        for name, relative in self.assets.items():
            identifier(name)
            if not str(relative).lower().endswith((".gltf", ".glb", ".ply", ".material.json", ".png", ".jpg", ".jpeg", ".dds", ".hdr", ".exr", ".tga", ".bmp", ".webp")):
                raise ValueError("v1 asset aliases support glTF/GLB/PLY, material JSON and image/environment files")
            if not inside(self.asset_root, relative).is_file():
                raise ValueError(f"Missing asset: {relative}")
        self.scene = data["scene"]
        if "base" in self.scene or "overrides" in self.scene:
            raise ValueError("Resolve scene overlays before fitting")
        ids = [entity["id"] for entity in self.scene["entities"]]
        if len(set(ids)) != len(ids):
            raise ValueError("Duplicate authored entity IDs")
        self.entities = {entity["id"]: entity for entity in self.scene["entities"]}
        targets = data["foreground_entities"]
        if not targets or len(set(targets)) != len(targets) or any(target not in self.entities for target in targets):
            raise ValueError("foreground_entities must identify authored entities")
        self.resolve_assets(self.scene, self.asset_root)
        self.render = data["render"]
        if self.render.get("denoiser", "off") not in {"off", "oidn"}:
            raise ValueError("Unsupported render.denoiser; choose off or oidn")
        if self.render.get("tone_map_operator", "AgX") not in {
            "Linear", "Reinhard", "ReinhardModified", "HejiHableAlu", "HableUc2",
            "Aces", "PbrNeutral", "IdentitySoftShoulder", "AgX", "CameraLut",
        }:
            raise ValueError("Unsupported render.tone_map_operator")
        for key in ("width", "height", "spp", "bounces"):
            if type(self.render[key]) is not int or self.render[key] <= 0:
                raise ValueError(f"render.{key} must be a positive integer")
        for key in ("timeout_seconds", "exposure_compensation"):
            if number(self.render[key], key) <= 0:
                raise ValueError(f"render.{key} must be positive")
        self.parameters = data["parameters"]
        names, bindings = set(), set()
        for parameter in self.parameters:
            name = identifier(parameter["name"])
            binding = parameter["binding"]
            signature = json.dumps(binding, sort_keys=True)
            if name in names or signature in bindings:
                raise ValueError("Duplicate parameter names or bindings")
            names.add(name)
            bindings.add(signature)
            if not parameter.get("unit") or parameter["group"] not in ("geometry", "material", "lighting"):
                raise ValueError(f"Parameter {name} needs unit and geometry/material/lighting group")
            if parameter["group"] == "material" and binding["target"] != "material":
                raise ValueError("Material parameters must bind material assets")
            if parameter["group"] == "lighting":
                lights = {"PointLight", "SpotLight", "RectLight", "DirectionalLight", "EnvironmentLight"}
                entity = self.entities.get(binding["id"], {}) if binding["target"] == "entity" else {}
                components = entity.get("components", {})
                writable = (lights & components.keys()) | {"Transform"}
                path = binding["path"].split("/")
                if not lights.intersection(components) or len(path) < 4 or path[1] != "components" or path[2] not in writable:
                    raise ValueError("Lighting parameters must bind a light component or its transform")
            lo, hi = [number(value, name) for value in parameter["bounds"]]
            initial = number(parameter["initial"], name)
            step = number(parameter["step"], name)
            if not lo < hi or not lo <= initial <= hi or not 0 < step <= hi - lo:
                raise ValueError(f"Invalid bounds/initial/step: {name}")
            node = self.binding_document(binding, self.scene, self.asset_root)
            pointer_parent(node, binding["path"])
        views = data["views"]
        if not views or not any(view["split"] == "train" for view in views):
            raise ValueError("At least one training view is required")
        view_ids = set()
        for view in views:
            view_id = identifier(view["id"])
            if view_id in view_ids or view["split"] not in ("train", "validation"):
                raise ValueError("Duplicate view ID or invalid split")
            view_ids.add(view_id)
            self.validate_camera(view["camera"])
            for key in ("image", "mask"):
                image = self.reference_path(view[key])
                with Image.open(image) as pixels:
                    if pixels.size != (self.render["width"], self.render["height"]):
                        raise ValueError(f"{view_id} {key}: resolution does not match render")
                    if key == "mask" and not np.asarray(pixels.convert("L")).any():
                        raise ValueError(f"{view_id}: empty foreground mask")
            if "depth" in view:
                depth = np.load(self.reference_path(view["depth"]), allow_pickle=False)
                if depth.shape != (self.render["height"], self.render["width"]):
                    raise ValueError("Depth must be HxW view-Z meters, with 0/NaN invalid")
        weights = data.get("loss_weights", {"rgb": 1.0, "silhouette": 1.0, "depth": 0.0})
        if set(weights) - {"rgb", "silhouette", "depth"}:
            raise ValueError("Unknown loss weight")
        self.weights = {key: number(weights.get(key, 0), key) for key in ("rgb", "silhouette", "depth")}
        if any(value < 0 for value in self.weights.values()) or sum(self.weights.values()) == 0:
            raise ValueError("Loss weights must be nonnegative with a positive total")
        if self.weights["depth"] > 0 and self.weights["silhouette"] <= 0:
            raise ValueError("Depth fitting requires a positive silhouette weight to penalize missing geometry")

    def reference_path(self, value):
        path = (self.root / value).resolve()
        if not path.is_file():
            raise ValueError(f"Missing reference: {path}")
        return path

    def validate_camera(self, camera):
        vectors = []
        for key in ("position", "direction", "up"):
            vector = camera[key]
            if len(vector) != 3:
                raise ValueError(f"camera.{key} must have three components")
            vectors.append(np.array([number(value, key) for value in vector]))
        if np.linalg.norm(vectors[1]) < 1e-6 or np.linalg.norm(np.cross(vectors[1], vectors[2])) < 1e-6:
            raise ValueError("Camera direction/up must be nonzero and not parallel")
        intrinsics = camera["intrinsics"]
        for key in ("fx", "fy", "cx", "cy"):
            number(intrinsics[key], key)
        if intrinsics["fx"] <= 0 or intrinsics["fy"] <= 0:
            raise ValueError("Focal lengths must be positive pixel values")

    def binding_document(self, binding, scene, asset_root):
        if binding["target"] == "entity":
            return next(entity for entity in scene["entities"] if entity["id"] == binding["id"])
        if binding["target"] == "material":
            relative = self.assets[binding["id"]]
            if not relative.endswith(".material.json"):
                raise ValueError("Material bindings require .material.json assets")
            return read_json(inside(asset_root, relative))
        raise ValueError("Binding target must be entity or material")

    def resolve_assets(self, node, asset_root):
        if isinstance(node, dict):
            result = {}
            for key, value in node.items():
                if isinstance(value, str) and value.startswith("asset:"):
                    alias = value[6:]
                    if alias not in self.assets:
                        raise ValueError(f"Unknown asset alias: {alias}")
                    value = inside(asset_root, self.assets[alias]).as_posix()
                elif isinstance(value, str) and (key in ("source", "path") or value.lower().endswith((".json", ".gltf", ".glb", ".ply", ".obj", ".hdr", ".dds"))) and not value.startswith(("builtin:", "procedural:")):
                    raise ValueError("Scene resource source/path must use asset: aliases or builtin/procedural references")
                result[key] = self.resolve_assets(value, asset_root)
            return result
        if isinstance(node, list):
            return [self.resolve_assets(value, asset_root) for value in node]
        return node

    @property
    def initial(self):
        return {parameter["name"]: parameter["initial"] for parameter in self.parameters}

    def values(self, overrides):
        if set(overrides) - set(self.initial):
            raise ValueError("Unknown parameter override")
        values = {**self.initial, **overrides}
        for parameter in self.parameters:
            value = number(values[parameter["name"]], parameter["name"])
            if not parameter["bounds"][0] <= value <= parameter["bounds"][1]:
                raise ValueError(f"Parameter outside bounds: {parameter['name']}")
            values[parameter["name"]] = value
        return values

    def freeze(self, destination):
        destination = Path(destination)
        try:
            destination.resolve().relative_to(self.asset_root)
        except ValueError:
            pass
        else:
            raise ValueError("Frozen input directory must be outside asset_root")
        destination.mkdir(parents=True, exist_ok=False)
        shutil.copytree(self.asset_root, destination / "assets")
        data = copy.deepcopy(self.data)
        data["asset_root"] = "assets"
        for view in data["views"]:
            for key in ("image", "mask", "depth"):
                if key in view:
                    source = self.reference_path(view[key])
                    target = destination / "references" / f"{view['id']}-{key}{source.suffix}"
                    target.parent.mkdir(exist_ok=True)
                    shutil.copy2(source, target)
                    view[key] = target.relative_to(destination).as_posix()
        write_json(destination / "project.json", data)
        return Project(destination / "project.json")

    def materialize(self, destination, values):
        values = self.values(values)
        destination = Path(destination)
        try:
            destination.resolve().relative_to(self.asset_root)
        except ValueError:
            pass
        else:
            raise ValueError("Candidate directory must be outside asset_root")
        destination.mkdir(parents=True, exist_ok=False)
        assets = destination / "assets"
        shutil.copytree(self.asset_root, assets)
        scene = copy.deepcopy(self.scene)
        for parameter in self.parameters:
            binding = parameter["binding"]
            document = self.binding_document(binding, scene, assets)
            node, key = pointer_parent(document, binding["path"])
            node[key] = values[parameter["name"]]
            if binding["target"] == "material":
                write_json(inside(assets, self.assets[binding["id"]]), document)
        for path in assets.rglob("*.material.json"):
            write_json(path, material_resources(read_json(path), assets, path, relocate=True))
        scene = self.resolve_assets(scene, assets)
        scene.setdefault("settings", {}).update({"enableAnimations": False, "enableKeyframes": False, "startingCamera": -1})
        scene["settings"].pop("startingCameraId", None)
        write_json(destination / "scene.scene.json", scene)
        write_json(destination / "parameters.json", values)
        return candidate_hash(destination)
