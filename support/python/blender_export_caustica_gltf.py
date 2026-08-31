# Runs inside Blender: remap .tx -> .png, unhide the scene, export glTF + sidecar.
# blender --background file.blend --python this_script.py -- --out attic.gltf

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import addon_utils
import bpy
from mathutils import Matrix, Vector


def _enable_gltf_addon() -> None:
    try:
        addon_utils.enable("io_scene_gltf2", default_set=False, persistent=False)
    except Exception as exc:
        print("Warning: could not enable io_scene_gltf2:", exc)
    if not hasattr(bpy.ops.export_scene, "gltf"):
        raise SystemExit("Blender glTF exporter is unavailable (io_scene_gltf2).")


def _argv_after_dash() -> list[str]:
    if "--" in sys.argv:
        return sys.argv[sys.argv.index("--") + 1 :]
    return []


def _parse_args(argv: list[str]) -> dict:
    out = None
    sidecar = None
    i = 0
    while i < len(argv):
        if argv[i] == "--out" and i + 1 < len(argv):
            out = Path(argv[i + 1])
            i += 2
            continue
        if argv[i] == "--sidecar" and i + 1 < len(argv):
            sidecar = Path(argv[i + 1])
            i += 2
            continue
        i += 1
    if out is None:
        raise SystemExit("blender_export_caustica_gltf.py requires --out <path.gltf>")
    if sidecar is None:
        sidecar = out.with_suffix(".export.json")
    return {"out": out, "sidecar": sidecar}


def _unpack_packed_images(out_dir: Path) -> int:
    unpacked = 0
    tex_dir = out_dir / "unpacked"
    for img in bpy.data.images:
        if not getattr(img, "packed_file", None):
            continue
        try:
            tex_dir.mkdir(parents=True, exist_ok=True)
            dest = tex_dir / Path(img.name).name
            if dest.suffix == "":
                dest = dest.with_suffix(".png")
            img.filepath = str(dest)
            img.save()
            unpacked += 1
        except Exception as exc:
            print(f"Warning: could not unpack {img.name}: {exc}")
    if unpacked:
        print(f"Unpacked {unpacked} packed images to {tex_dir}")
    return unpacked


def _remap_tx_images() -> list[dict]:
    report = []
    for img in bpy.data.images:
        raw = img.filepath or ""
        abs_path = Path(bpy.path.abspath(raw)) if raw else None
        png = None
        if abs_path and abs_path.suffix.lower() == ".tx":
            png = abs_path.with_suffix(".png")
        if png and png.is_file():
            img.filepath = str(png)
            try:
                img.reload()
            except Exception as exc:
                report.append({"name": img.name, "from": raw, "to": str(png), "reload": str(exc)})
                continue
            report.append({"name": img.name, "from": raw, "to": str(png), "reload": "ok"})
        else:
            report.append(
                {
                    "name": img.name,
                    "from": raw,
                    "to": str(png) if png else None,
                    "reload": "missing-png" if png else "no-tx",
                }
            )
    return report


def _has_volume_output(material) -> bool:
    tree = getattr(material, "node_tree", None)
    if not material or not material.use_nodes or not tree:
        return False
    for output in tree.nodes:
        if output.type != "OUTPUT_MATERIAL" or not getattr(output, "is_active_output", True):
            continue
        volume = output.inputs.get("Volume")
        if volume and volume.is_linked:
            return True
    return False


def _should_export_object(obj) -> bool:
    if obj.hide_render:
        return False
    lowered = (obj.name or "").lower()
    if lowered.startswith(("hide_", "hlp-", "env-", "fog")):
        return False
    # glTF has no volume-material representation. Exporting a volume domain as
    # a regular mesh turns its boundary into an opaque surface. Omit it even if
    # the material also has a boundary Surface; the whole material needs a
    # future native Caustica volume conversion to remain physically meaningful.
    # Names such as "fog" or "proxy" are deliberately not used here.
    if obj.type == "MESH" and any(
        _has_volume_output(slot.material) for slot in obj.material_slots
    ):
        return False
    return True


def _prepare_visibility() -> None:
    # Keep Blender render visibility. Viewport-only hides are cleared so
    # renderable objects still reach the glTF exporter.
    hidden = 0
    for obj in bpy.data.objects:
        export = _should_export_object(obj)
        if not export:
            hidden += 1
        obj.hide_render = not export
        obj.hide_viewport = not export
        try:
            obj.hide_set(not export)
        except Exception:
            pass
        try:
            obj.select_set(export)
        except Exception:
            pass
    print(f"Prepared visibility, excluded {hidden} helper/hidden objects")


def _matrix_trs(matrix: Matrix) -> dict:
    loc, rot, scale = matrix.decompose()
    return {
        "translation": [loc.x, loc.y, loc.z],
        "rotation": [rot.x, rot.y, rot.z, rot.w],
        "scale": [scale.x, scale.y, scale.z],
    }


def _gltf_yup(matrix: Matrix) -> dict:
    # Blender Z-up (X,Y,Z) -> glTF Y-up (X,Z,-Y), matching Khronos I/O.
    convert = Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))
    return _matrix_trs(convert @ matrix)


def _camera_info(obj) -> dict:
    cam = obj.data
    lens = float(cam.lens)
    render = bpy.context.scene.render
    aspect = (render.resolution_x * render.pixel_aspect_x) / max(
        1e-6, render.resolution_y * render.pixel_aspect_y
    )
    sensor_fit = cam.sensor_fit
    if sensor_fit == "AUTO":
        sensor_fit = "HORIZONTAL" if aspect >= 1.0 else "VERTICAL"
    sensor_h = float(cam.sensor_height) if sensor_fit == "VERTICAL" else float(cam.sensor_width) / aspect
    vfov = 2.0 * math.atan((0.5 * sensor_h) / max(lens, 1e-6))
    forward = obj.matrix_world.to_quaternion() @ Vector((0.0, 0.0, -1.0))
    hit, location, _normal, _face, hit_object, _matrix = bpy.context.scene.ray_cast(
        bpy.context.evaluated_depsgraph_get(),
        obj.matrix_world.translation,
        forward,
        distance=float(cam.clip_start),
    )
    info = {
        "name": obj.name,
        "type": cam.type,
        "lens_mm": lens,
        "sensor_fit": cam.sensor_fit,
        "sensor_width": float(cam.sensor_width),
        "sensor_height": float(cam.sensor_height),
        "clip_start": float(cam.clip_start),
        "clip_end": float(cam.clip_end),
        "vertical_fov": vfov,
        "ortho_scale": float(cam.ortho_scale),
        "clip_obstruction_distance": (
            float((location - obj.matrix_world.translation).length) if hit else None
        ),
        "clip_obstruction_object": hit_object.name if hit and hit_object else None,
        "blender_world": _matrix_trs(obj.matrix_world),
        "gltf_yup": _gltf_yup(obj.matrix_world),
    }
    return info


def _light_info(obj) -> dict:
    light = obj.data
    return {
        "name": obj.name,
        "type": light.type,
        "energy": float(getattr(light, "energy", 0.0)),
        "exposure": float(getattr(light, "exposure", 0.0)),
        "normalize": bool(getattr(light, "normalize", True)),
        "color": list(light.color),
        "shadow_soft_size": float(getattr(light, "shadow_soft_size", 0.0)),
        "spot_size": float(getattr(light, "spot_size", math.pi / 2.0)),
        "spot_blend": float(getattr(light, "spot_blend", 0.0)),
        "shape": str(getattr(light, "shape", "DISK")),
        "size": float(getattr(light, "size", 0.0)),
        "size_y": float(getattr(light, "size_y", getattr(light, "size", 0.0))),
        "blender_world": _matrix_trs(obj.matrix_world),
        "gltf_yup": _gltf_yup(obj.matrix_world),
    }


def _world_info() -> dict:
    world = bpy.context.scene.world
    info = {"name": world.name if world else None, "use_nodes": bool(world and world.use_nodes)}
    if not world or not world.use_nodes or not world.node_tree:
        return info
    for node in world.node_tree.nodes:
        if node.type == "TEX_ENVIRONMENT" and getattr(node, "image", None):
            info["environment_texture"] = bpy.path.abspath(node.image.filepath)
        if node.type == "TEX_SKY":
            info["sky_texture"] = {
                "type": str(getattr(node, "sky_type", "")),
                "sun_direction": list(getattr(node, "sun_direction", (0.0, 0.0, 1.0))),
                "sun_elevation": float(getattr(node, "sun_elevation", 0.0)),
                "sun_rotation": float(getattr(node, "sun_rotation", 0.0)),
                "sun_intensity": float(getattr(node, "sun_intensity", 1.0)),
                "altitude": float(getattr(node, "altitude", 0.0)),
                "air_density": float(getattr(node, "air_density", 1.0)),
                "dust_density": float(getattr(node, "dust_density", 1.0)),
                "ozone_density": float(getattr(node, "ozone_density", 1.0)),
            }
        if node.type == "BACKGROUND":
            col = node.inputs["Color"].default_value
            info["background_color"] = [col[0], col[1], col[2]]
            info["background_strength"] = float(node.inputs["Strength"].default_value)
    return info


def _dump_sidecar(path: Path, image_report: list[dict]) -> None:
    scene = bpy.context.scene
    cameras = [_camera_info(obj) for obj in scene.objects if obj.type == "CAMERA"]
    if scene.camera:
        cameras.sort(key=lambda camera: camera["name"] != scene.camera.name)
    lights = [
        _light_info(obj)
        for obj in scene.objects
        if obj.type == "LIGHT" and _should_export_object(obj)
    ]
    materials = []
    for mat in bpy.data.materials:
        materials.append(
            {
                "name": mat.name,
                "blend_method": getattr(mat, "blend_method", None),
                "use_nodes": bool(mat.use_nodes),
            }
        )
    payload = {
        "blend": bpy.data.filepath,
        "scene": scene.name,
        "unit_system": scene.unit_settings.system,
        "unit_scale": float(scene.unit_settings.scale_length),
        "resolution": [scene.render.resolution_x, scene.render.resolution_y],
        "active_camera": scene.camera.name if scene.camera else None,
        "render_engine": scene.render.engine,
        "view": {
            "exposure": float(scene.view_settings.exposure),
            "look": str(scene.view_settings.look),
        },
        "world": _world_info(),
        "cameras": cameras,
        "lights": lights,
        "unsupported_volumes": [
            {
                "object": obj.name,
                "materials": [
                    slot.material.name
                    for slot in obj.material_slots
                    if _has_volume_output(slot.material)
                ],
                "reason": "Volume material omitted: current glTF/Caustica conversion has no volume representation",
            }
            for obj in scene.objects
            if obj.type == "MESH"
            and any(_has_volume_output(slot.material) for slot in obj.material_slots)
        ],
        "object_count": len(scene.objects),
        "mesh_count": sum(1 for obj in scene.objects if obj.type == "MESH"),
        "materials": materials,
        "images": image_report,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"Wrote sidecar {path}")


def _socket(node, *names):
    for name in names:
        sock = node.inputs.get(name)
        if sock is not None:
            return sock
    return None


def _image_role(node) -> str:
    name = ((node.image.name if node.image else "") + " " + node.name).lower()
    if any(key in name for key in ("normal", "nor_", "_nor", "nrm", "bump")):
        return "normal"
    if any(key in name for key in ("rough", "gloss", "spec")):
        return "roughness"
    if "metal" in name:
        return "metallic"
    if any(key in name for key in ("emit", "emiss")):
        return "emission"
    return "base"


def _as_rgba(value) -> list[float]:
    try:
        values = list(value)
    except TypeError:
        scalar = float(value)
        return [scalar, scalar, scalar, 1.0]
    if len(values) == 1:
        return [values[0], values[0], values[0], 1.0]
    return [float(values[0]), float(values[1]), float(values[2]), float(values[3]) if len(values) > 3 else 1.0]


def _image_average(image, cache: dict) -> list[float]:
    cached = cache.get(image.name_full)
    if cached is not None:
        return cached
    try:
        width, height = image.size
        if width <= 0 or height <= 0:
            raise ValueError("empty image")
        # Sparse sampling avoids copying multi-gigabyte packed benchmark maps.
        samples = []
        grid = 12
        pixels = image.pixels
        for gy in range(grid):
            y = min(height - 1, int((gy + 0.5) * height / grid))
            for gx in range(grid):
                x = min(width - 1, int((gx + 0.5) * width / grid))
                offset = (y * width + x) * 4
                samples.append([pixels[offset + channel] for channel in range(4)])
        average = [sum(sample[channel] for sample in samples) / len(samples) for channel in range(4)]
    except Exception:
        average = [0.5, 0.5, 0.5, 1.0]
    cache[image.name_full] = average
    return average


def _evaluate_color_socket(socket, image_cache: dict, depth: int = 0) -> list[float]:
    """Approximate a procedural socket by its average linear RGBA value."""
    if socket is None or depth > 24:
        return [0.5, 0.5, 0.5, 1.0]
    if not socket.is_linked:
        return _as_rgba(getattr(socket, "default_value", 0.5))
    source = socket.links[0].from_socket
    node = source.node
    if node.type == "TEX_IMAGE" and node.image:
        if source.name == "Alpha":
            alpha = _image_average(node.image, image_cache)[3]
            return [alpha, alpha, alpha, 1.0]
        return _image_average(node.image, image_cache)
    if node.type == "REROUTE":
        return _evaluate_color_socket(node.inputs[0], image_cache, depth + 1)
    if node.type == "RGB":
        return _as_rgba(node.outputs[0].default_value)
    if node.type == "VALUE":
        return _as_rgba(node.outputs[0].default_value)
    if node.type == "VALTORGB":
        factor = _evaluate_color_socket(node.inputs[0], image_cache, depth + 1)[0]
        elements = sorted(node.color_ramp.elements, key=lambda element: element.position)
        left, right = elements[0], elements[-1]
        for candidate in elements[1:]:
            if factor <= candidate.position:
                right = candidate
                break
            left = candidate
        span = max(1e-8, right.position - left.position)
        amount = min(max((factor - left.position) / span, 0.0), 1.0)
        return [left.color[i] * (1.0 - amount) + right.color[i] * amount for i in range(4)]
    if node.type in {"MIX", "MIX_RGB"}:
        if node.type == "MIX" and getattr(node, "data_type", "") == "RGBA":
            factor_socket, a_socket, b_socket = node.inputs[0], node.inputs[6], node.inputs[7]
        else:
            factor_socket, a_socket, b_socket = node.inputs[0], node.inputs[1], node.inputs[2]
        factor = min(max(_evaluate_color_socket(factor_socket, image_cache, depth + 1)[0], 0.0), 1.0)
        a = _evaluate_color_socket(a_socket, image_cache, depth + 1)
        b = _evaluate_color_socket(b_socket, image_cache, depth + 1)
        blend = getattr(node, "blend_type", "MIX")
        if blend == "ADD":
            result = [a[i] + b[i] * factor for i in range(4)]
        elif blend == "SUBTRACT":
            result = [a[i] - b[i] * factor for i in range(4)]
        elif blend == "MULTIPLY":
            result = [a[i] * ((1.0 - factor) + b[i] * factor) for i in range(4)]
        else:
            result = [a[i] * (1.0 - factor) + b[i] * factor for i in range(4)]
        return [min(max(component, 0.0), 1.0) for component in result]
    # Noise, geometry, attributes and custom color groups cannot be represented
    # by glTF. Blender keeps the author's fallback on the receiving socket; it
    # is a materially better approximation than neutral gray (and commonly
    # carries the intended vertex-color tint).
    return _as_rgba(getattr(socket, "default_value", 0.5))


def _direct_image_socket(socket):
    """Return an image output only when no procedural color operation intervenes."""
    if socket is None or not socket.is_linked:
        return None
    source = socket.links[0].from_socket
    seen = set()
    while source.node.type == "REROUTE" and source.node not in seen:
        seen.add(source.node)
        reroute_input = source.node.inputs[0]
        if not reroute_input.is_linked:
            return None
        source = reroute_input.links[0].from_socket
    return source if source.node.type == "TEX_IMAGE" else None


def _albedo_image_socket(socket, depth: int = 0):
    """Find a base-color image through common Cycles color-correction nodes."""
    if socket is None or not getattr(socket, "is_linked", False) or depth > 16:
        return None
    source = socket.links[0].from_socket
    node = source.node
    if node.type == "TEX_IMAGE":
        return source if _image_role(node) == "base" else None
    if node.type == "REROUTE" and node.inputs:
        return _albedo_image_socket(node.inputs[0], depth + 1)
    if node.type in {"HUE_SAT", "BRIGHTCONTRAST", "CURVE_RGB", "GAMMA", "INVERT", "HUE_CORRECT"}:
        return _albedo_image_socket(node.inputs.get("Color"), depth + 1)
    if node.type in {"MIX", "MIX_RGB"}:
        if node.type == "MIX" and getattr(node, "data_type", "") == "RGBA":
            first, second = node.inputs[6], node.inputs[7]
        else:
            first, second = node.inputs[1], node.inputs[2]
        return _albedo_image_socket(first, depth + 1) or _albedo_image_socket(second, depth + 1)
    return None


def _upgrade_legacy_to_principled() -> int:
    upgraded = 0
    image_average_cache = {}
    for mat in bpy.data.materials:
        tree = getattr(mat, "node_tree", None)
        if not tree:
            continue
        nodes = tree.nodes
        links = tree.links
        output = next(
            (node for node in nodes if node.type == "OUTPUT_MATERIAL" and getattr(node, "is_active_output", True)),
            next((node for node in nodes if node.type == "OUTPUT_MATERIAL"), None),
        )
        if output is None:
            output = nodes.new("ShaderNodeOutputMaterial")
            output.location = (400, 0)

        surface_links = list(output.inputs["Surface"].links)
        upstream = set()
        pending = [link.from_node for link in surface_links]
        while pending:
            node = pending.pop()
            if node in upstream:
                continue
            upstream.add(node)
            pending.extend(link.from_node for sock in node.inputs for link in sock.links)

        if any(node.type == "BSDF_PRINCIPLED" for node in upstream):
            continue

        graph_nodes = upstream or set(nodes)
        images = [node for node in graph_nodes if node.type == "TEX_IMAGE" and node.image]
        glasses = [node for node in graph_nodes if node.type == "BSDF_GLASS"]
        emissions = [node for node in graph_nodes if node.type == "EMISSION"]
        glossies = [node for node in graph_nodes if node.type == "BSDF_GLOSSY"]
        diffs = [node for node in graph_nodes if node.type == "BSDF_DIFFUSE"]
        groups = [node for node in graph_nodes if node.type == "GROUP"]
        transparent = any(node.type == "BSDF_TRANSPARENT" for node in graph_nodes)
        if not (images or glasses or emissions or glossies or diffs):
            continue

        principled = nodes.new("ShaderNodeBsdfPrincipled")
        principled.location = (80, 0)

        def set_input(names, value=None, from_socket=None):
            sock = _socket(principled, *names)
            if sock is None:
                return
            if from_socket is not None:
                links.new(from_socket, sock)
            elif value is not None:
                if sock.type == "RGBA" and len(value) >= 3:
                    sock.default_value = (value[0], value[1], value[2], 1.0 if len(value) < 4 else value[3])
                elif sock.type != "SHADER":
                    sock.default_value = value

        def copy_socket(target_names, source):
            if source is None:
                return False
            if source.is_linked:
                direct_image = _direct_image_socket(source) or _albedo_image_socket(source)
                if direct_image is not None:
                    set_input(target_names, from_socket=direct_image)
                else:
                    fallback = _as_rgba(getattr(source, "default_value", 0.8))
                    is_neutral = all(abs(fallback[channel] - 0.8) < 0.02 for channel in range(3))
                    if not is_neutral:
                        set_input(target_names, value=fallback)
                    else:
                        evaluated = _evaluate_color_socket(source, image_average_cache)
                        target = _socket(principled, *target_names)
                        set_input(
                            target_names,
                            value=evaluated if target and target.type == "RGBA" else evaluated[0],
                        )
            else:
                value = source.default_value
                try:
                    value = list(value)
                except TypeError:
                    value = float(value)
                set_input(target_names, value=value)
            return True

        def semantic_group_socket(*names):
            lowered = {name.lower() for name in names}
            for group in groups:
                for sock in group.inputs:
                    if sock.name.lower() in lowered:
                        return sock
            return None

        base_img = next((node for node in images if _image_role(node) == "base"), None)
        normal_img = next((node for node in images if _image_role(node) == "normal"), None)
        diffuse_socket = semantic_group_socket("Diffuse Color", "Base Color", "Color")
        if diffuse_socket is not None:
            copy_socket(("Base Color",), diffuse_socket)
        elif diffs:
            copy_socket(("Base Color",), diffs[0].inputs.get("Color"))
        elif base_img:
            set_input(("Base Color",), from_socket=base_img.outputs[0])

        roughness_socket = semantic_group_socket("Roughness", "Glossy Roughness", "Specular Roughness")
        if roughness_socket is not None:
            copy_socket(("Roughness",), roughness_socket)
        elif glossies:
            copy_socket(("Roughness",), glossies[0].inputs.get("Roughness"))

        if normal_img:
            nmap = nodes.new("ShaderNodeNormalMap")
            nmap.location = (-120, -180)
            links.new(normal_img.outputs[0], nmap.inputs["Color"])
            set_input(("Normal",), from_socket=nmap.outputs["Normal"])
        else:
            bump_socket = semantic_group_socket("Bump", "Height")
            if bump_socket is not None:
                bump = nodes.new("ShaderNodeBump")
                bump.location = (-120, -180)
                if bump_socket.is_linked:
                    links.new(bump_socket.links[0].from_socket, bump.inputs["Height"])
                else:
                    bump.inputs["Height"].default_value = float(bump_socket.default_value)
                set_input(("Normal",), from_socket=bump.outputs["Normal"])

        if glasses:
            glass = glasses[0]
            color = glass.inputs["Color"].default_value
            set_input(("Base Color",), value=list(color)[:3])
            set_input(("Transmission Weight", "Transmission"), value=1.0)
            set_input(("Metallic",), value=0.0)
            set_input(("Roughness",), value=float(glass.inputs["Roughness"].default_value))
            set_input(("IOR",), value=float(glass.inputs["IOR"].default_value))
            if base_img:
                set_input(("Base Color",), from_socket=base_img.outputs[0])
        elif transparent and not emissions:
            # Light-path glass (picture glass, lamp globes) is not a Glass BSDF.
            # Keep it transmissive and ignore near-black "shadow" diffuse colors.
            diffuse_color = [1.0, 1.0, 1.0]
            if diffs:
                diffuse_color = _as_rgba(diffs[0].inputs["Color"].default_value)[:3]
            if max(diffuse_color) < 0.05:
                diffuse_color = [1.0, 1.0, 1.0]
            set_input(("Base Color",), value=diffuse_color)
            set_input(("Transmission Weight", "Transmission"), value=0.85)
            set_input(("Metallic",), value=0.0)
            set_input(("Roughness",), value=0.08 if not glossies else float(glossies[0].inputs["Roughness"].default_value))
            set_input(("Alpha",), value=0.2)

        if emissions:
            emission = emissions[0]
            color_sock = emission.inputs.get("Color")
            # Cycles lamps often mix emission through Light Path. The socket
            # fallback is the authored tungsten color; evaluating the mix
            # collapses it to camera-ray white.
            if color_sock is not None:
                set_input(("Emission Color", "Emission"), value=_as_rgba(color_sock.default_value)[:3])
            copy_socket(("Emission Strength",), emission.inputs.get("Strength"))

        if glossies and not glasses and not diffs:
            glossy = glossies[0]
            set_input(("Metallic",), value=1.0)
            set_input(("Roughness",), value=float(glossy.inputs["Roughness"].default_value))
            set_input(("Base Color",), value=list(glossy.inputs["Color"].default_value)[:3])

        if diffs and not glasses and not (transparent and not emissions):
            diffuse = diffs[0]
            if diffuse_socket is None and not base_img:
                copy_socket(("Base Color",), diffuse.inputs.get("Color"))

        if transparent:
            alpha_img = next(
                (node for node in images if _image_role(node) == "base" and node.outputs.get("Alpha")),
                None,
            )
            if alpha_img and not glasses:
                set_input(("Alpha",), from_socket=alpha_img.outputs["Alpha"])
            try:
                mat.surface_render_method = "DITHERED"
            except Exception:
                try:
                    mat.blend_method = "BLEND"
                except Exception:
                    pass

        if output.inputs["Surface"].is_linked:
            for link in list(output.inputs["Surface"].links):
                links.remove(link)
        links.new(principled.outputs["BSDF"], output.inputs["Surface"])
        upgraded += 1

    print(f"Upgraded {upgraded} legacy Cycles materials to Principled")
    return upgraded


def _export_gltf(out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    kwargs = {
        "filepath": str(out),
        "export_format": "GLTF_SEPARATE",
        "export_cameras": False,
        # Lights are written from the sidecar. This avoids duplicate punctual
        # lights and lets the scene writer approximate Blender AREA lights.
        "export_lights": False,
        "export_apply": True,
        "export_image_format": "AUTO",
        "export_keep_originals": False,
        "export_texcoords": True,
        "export_normals": True,
        "export_tangents": True,
        "export_materials": "EXPORT",
        "export_yup": True,
        "export_extras": False,
        # Selection is populated by _prepare_visibility. This is more robust
        # than relying on Blender-version-specific visibility filtering alone.
        "use_selection": True,
        "use_visible": True,
        "use_renderable": True,
        "use_active_collection": False,
        "use_active_scene": True,
        "export_animations": False,
        "export_skins": False,
        "export_morph": False,
    }
    try:
        rna = bpy.ops.export_scene.gltf.get_rna_type()
        valid = {prop.identifier for prop in rna.properties}
        kwargs = {key: value for key, value in kwargs.items() if key in valid}
    except Exception as exc:
        print(f"Warning: could not filter glTF export kwargs ({exc})")
    print("export_scene.gltf", kwargs)
    result = bpy.ops.export_scene.gltf(**kwargs)
    print("export result", result)
    if "FINISHED" not in result:
        raise SystemExit(f"glTF export failed: {result}")

    # Blender may emit texCoord: -1 for Generated/Object coordinates that have
    # no glTF equivalent. That value is invalid glTF; defaulting to UV0 keeps
    # the asset loadable and makes the limitation explicit in exporter logs.
    doc = json.loads(out.read_text(encoding="utf-8"))
    repaired = 0

    def repair(value):
        nonlocal repaired
        if isinstance(value, dict):
            if isinstance(value.get("texCoord"), int) and value["texCoord"] < 0:
                value.pop("texCoord")
                repaired += 1
            for child in value.values():
                repair(child)
        elif isinstance(value, list):
            for child in value:
                repair(child)

    repair(doc)
    if repaired:
        out.write_text(json.dumps(doc, separators=(",", ":")), encoding="utf-8")
        print(f"Repaired {repaired} invalid negative texCoord entries (defaulted to UV0)")


def main() -> None:
    _enable_gltf_addon()
    args = _parse_args(_argv_after_dash())
    print("Opened", bpy.data.filepath, "scene", bpy.context.scene.name)
    _unpack_packed_images(args["out"].parent)
    image_report = _remap_tx_images()
    missing = [row for row in image_report if row.get("reload") == "missing-png"]
    print(f"Images remapped: {len(image_report)} missing-png={len(missing)}")
    _upgrade_legacy_to_principled()
    _prepare_visibility()
    _dump_sidecar(args["sidecar"], image_report)
    _export_gltf(args["out"])
    print("Export complete", args["out"])


if __name__ == "__main__":
    main()
