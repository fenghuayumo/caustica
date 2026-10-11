"""Runs only inside Blender. A request contains validated geometry and cameras."""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Vector

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from blender_fit.common import read, write, validate_geometry, diagnostic_colors


def build(geometry):
    validate_geometry(geometry)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    material = bpy.data.materials.new("GeometryPreview")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (0.65, 0.7, 0.8, 1)
    material.node_tree.links.new(emission.outputs[0], output.inputs["Surface"])
    objects = []
    for part in geometry["parts"]:
        kind = part["kind"]
        if kind == "box":
            bpy.ops.mesh.primitive_cube_add(size=1)
        elif kind == "ellipsoid":
            bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=0.5)
        elif kind == "cylinder":
            bpy.ops.mesh.primitive_cylinder_add(vertices=64, radius=0.5, depth=1)
        else:
            samples = part["profile"]
            n = 64
            vertices = [(radius * math.cos(2*math.pi*i/n), radius * math.sin(2*math.pi*i/n), height)
                        for height, radius in samples for i in range(n)]
            faces = [(j*n+i, j*n+(i+1)%n, (j+1)*n+(i+1)%n, (j+1)*n+i)
                     for j in range(len(samples)-1) for i in range(n)]
            if not part.get("thickness", 0):
                faces += [tuple(reversed(range(n))), tuple((len(samples)-1)*n+i for i in range(n))]
            mesh = bpy.data.meshes.new(part["name"])
            mesh.from_pydata(vertices, [], faces)
            mesh.update()
            obj = bpy.data.objects.new(part["name"], mesh)
            bpy.context.collection.objects.link(obj)
            bpy.context.view_layer.objects.active = obj
            obj.select_set(True)
            if part.get("thickness", 0):
                modifier = obj.modifiers.new("PhysicalWall", "SOLIDIFY")
                modifier.thickness = part["thickness"]
                modifier.offset = -1
        obj = bpy.context.view_layer.objects.active
        obj.name = part["name"]
        obj.location = part["position"]
        obj.rotation_euler = [math.radians(x) for x in part["rotation"]]
        # Lathe profile is in local units; size scales it. Other sizes are full dimensions.
        obj.scale = part["size"]
        obj.data.materials.append(material)
        objects.append(obj)
    return objects


def camera(view):
    scene = bpy.context.scene
    bpy.ops.object.camera_add()
    obj = bpy.context.object
    obj.name = view["id"]
    obj.location = view["camera"]["position"]
    direction = Vector(view["camera"]["direction"]).normalized()
    right = direction.cross(Vector(view["camera"]["up"])).normalized()
    up = right.cross(direction).normalized()
    obj.rotation_euler = Matrix((right, up, -direction)).transposed().to_euler()
    k = view["camera"]["intrinsics"]
    width, height = view["width"], view["height"]
    data = obj.data
    data.sensor_fit = "HORIZONTAL"
    data.sensor_width = 36
    data.lens = k["fx"] * 36 / width
    data.shift_x = (width/2 - k["cx"]) / width
    data.shift_y = (k["cy"] - height/2) * k["fx"] / k["fy"] / width
    data.clip_start = view.get("clip_start", 0.001)
    data.clip_end = view.get("clip_end", 1e7)
    scene.render.resolution_x = width
    scene.render.resolution_y = height
    scene.render.resolution_percentage = 100
    scene.render.pixel_aspect_x = 1
    scene.render.pixel_aspect_y = k["fx"] / k["fy"]
    scene.camera = obj
    return obj


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--request", required=True)
    args = parser.parse_args(sys.argv[sys.argv.index("--")+1:])
    request = read(args.request)
    root = Path(request["output_dir"])
    root.mkdir(parents=True, exist_ok=True)
    objects = build(request["geometry"])
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 1
    scene.cycles.use_denoising = False
    scene.render.film_transparent = True
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.view_settings.view_transform = "Standard"
    projections = {}
    diagnostic_materials = []
    if request.get("diagnostic"):
        # Distinct diffuse colors expose internal boundaries without fitting appearance.
        palette = diagnostic_colors(request["geometry"])
        for mesh_object in objects:
            mat = bpy.data.materials.new("Diagnostic_" + mesh_object.name)
            mat.use_nodes = True
            shader = mat.node_tree.nodes.get("Principled BSDF")
            shader.inputs["Base Color"].default_value = (*palette[mesh_object.name], 1)
            shader.inputs["Roughness"].default_value = 1
            shader.inputs["Specular IOR Level"].default_value = 0
            diagnostic_materials.append(mat)
        scene.world.use_nodes = True
        scene.world.node_tree.nodes.get("Background").inputs["Strength"].default_value = 0.35
        bpy.ops.object.light_add(type="AREA")
        diagnostic_light = bpy.context.object
    from bpy_extras.object_utils import world_to_camera_view
    for view in request["views"]:
        obj = camera(view)
        bpy.context.view_layer.update()
        projections[view["id"]] = []
        for point in request.get("probe_points", []):
            uv = world_to_camera_view(scene, obj, Vector(point))
            projections[view["id"]].append([uv.x * view["width"], (1-uv.y) * view["height"], uv.z])
        scene.render.filepath = str(root / (view["id"] + ".png"))
        bpy.ops.render.render(write_still=True)
        if request.get("diagnostic"):
            for mesh_object, mat in zip(objects, diagnostic_materials):
                mesh_object.data.materials[0] = mat
            # Camera-relative key light, sized and powered in scene units.
            centers = [o.location for o in objects]
            center = sum(centers, Vector()) / len(centers)
            extent = max((o.location - center).length + max(o.dimensions) for o in objects)
            extent = max(extent, 1e-6)
            right = Vector(view["camera"]["direction"]).cross(Vector(view["camera"]["up"])).normalized()
            diagnostic_light.location = obj.location + right * extent + Vector(view["camera"]["up"]).normalized() * extent
            diagnostic_light.rotation_euler = (center - diagnostic_light.location).to_track_quat("-Z", "Y").to_euler()
            diagnostic_light.data.energy = 3 * (diagnostic_light.location - center).length_squared
            diagnostic_light.data.size = extent
            scene.cycles.samples = 32
            scene.cycles.use_denoising = True
            scene.render.filepath = str(root / (view["id"] + "-diagnostic.png"))
            bpy.ops.render.render(write_still=True)
            for mesh_object in objects:
                mesh_object.data.materials[0] = bpy.data.materials["GeometryPreview"]
            scene.cycles.samples = 1
            scene.cycles.use_denoising = False
    if request.get("diagnostic"):
        bpy.data.objects.remove(diagnostic_light, do_unlink=True)
    if request.get("export", False):
        scene.cycles.samples = 128
        scene.cycles.use_denoising = True
        if request.get("reference_points"):
            cloud = bpy.data.meshes.new("SparseForegroundPoints")
            cloud.from_pydata(request["reference_points"], [], [])
            cloud.update()
            guide = bpy.data.objects.new("SparseForegroundGuide", cloud)
            bpy.context.collection.objects.link(guide)
            guide.hide_render = True
            guide.display_type = "WIRE"
        # A physical neutral material replaces the silhouette shader in exported assets.
        material = bpy.data.materials.new("AppearanceToFit")
        material.use_nodes = True
        for obj in objects:
            obj.data.materials.clear()
            obj.data.materials.append(material)
            if not obj.data.uv_layers:
                bpy.ops.object.select_all(action="DESELECT")
                obj.select_set(True)
                bpy.context.view_layer.objects.active = obj
                bpy.ops.object.mode_set(mode="EDIT")
                bpy.ops.mesh.select_all(action="SELECT")
                bpy.ops.uv.smart_project()
                bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="DESELECT")
        for obj in objects:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = objects[0]
        bpy.ops.wm.save_as_mainfile(filepath=str(root / "model.blend"))
        bpy.ops.export_scene.gltf(filepath=str(root / "model.glb"), export_format="GLB", use_selection=True,
                                  export_yup=True, export_apply=True, export_animations=False,
                                  export_cameras=False, export_lights=False)
    mesh_stats = {}
    depsgraph = bpy.context.evaluated_depsgraph_get()
    for obj in objects:
        evaluated = obj.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh()
        mesh_stats[obj.name] = {"vertices": len(mesh.vertices), "faces": len(mesh.polygons), "uv_layers": len(mesh.uv_layers)}
        evaluated.to_mesh_clear()
    write(root / "worker-result.json", {"blender_version": bpy.app.version_string,
                                         "projections": projections, "parts": len(objects), "mesh_stats": mesh_stats})


if __name__ == "__main__":
    main()
