# --------------------------------------------------------------------
# caustica Python scripting - basic example
# --------------------------------------------------------------------
# Run this script either at startup with the --pythonScript CLI option
# or interactively from the in-app `System -> Python scripting` panel.
#
# Available globals:
#   * caustica.app()      -> the Sample renderer singleton
#   * caustica.settings() -> shortcut for caustica.app().settings
#
# Use caustica.log_info / log_warning / log_error to print to the host log.
# --------------------------------------------------------------------

import caustica

app = caustica.app()
scene = app.scene
settings = caustica.settings()

caustica.log_info(f"Loaded scene: {app.scene_name}")

# 1) List every material -- print a summary
print("\n=== Materials ===")
for mat in scene.get_materials():
    print(f"{mat.name:32s}  base={mat.base_color}  rough={mat.roughness:.2f}  metal={mat.metalness:.2f}")

# 2) Tweak a specific material (look up by name or unique-id)
floor = scene.find_material("Floor")
if floor is not None:
    floor.base_color = (0.2, 0.05, 0.05)   # accept tuple/list/iterable
    floor.roughness  = 0.85
    floor.metalness  = 0.0
    caustica.log_info(f"Recolored '{floor.name}'")

# 3) Make the wall mildly emissive
wall = scene.find_material("Wall")
if wall is not None:
    wall.emissive_color     = (1.0, 0.7, 0.4)
    wall.emissive_intensity = 5.0

# 4) Enumerate scene lights and bump their intensity
print("\n=== Lights ===")
for light in scene.get_lights():
    print(f"  type={light.light_type}  name={light.name}  color={light.color}")
    if light.intensity:
        light.intensity = light.intensity * 1.5

# 5) Tone & quality settings
settings.bounce_count       = 8       # path-tracer max bounces
settings.diffuse_bounce_count = 3
settings.realtime_mode      = True
settings.realtime_aa        = 3       # DLSS-RR
settings.enable_bloom       = True
settings.bloom_intensity    = 0.005

# 6) Adjust the environment map runtime parameters
env = settings.environment_map
env.intensity   = 1.5
env.tint_color  = (1.0, 0.95, 0.9)
env.rotation_xyz = (0.0, 35.0, 0.0)   # degrees
env.enabled     = True
env.visible_to_camera = True           # False hides the HDRI background but keeps its lighting

# 7) Camera tweaks
settings.camera_aperture       = 0.0
settings.camera_focal_distance = 25.0
app.set_camera_fov(55.0)

caustica.log_info("Python customization done.")
