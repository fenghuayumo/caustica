# --------------------------------------------------------------------
# caustica Python scripting - per-frame light animation
# --------------------------------------------------------------------
# Each top-level call of this script picks the next phase in a
# pre-baked color table and writes it to every PointLight / SpotLight
# in the scene.
#
# To get a continuously-animated light, pair this script with the
# in-app inline runner (Run inline button) and tap it repeatedly,
# or wrap the body in your own scheduling logic from the host side.
# --------------------------------------------------------------------

import math
import time
import caustica

app = caustica.app()
scene = app.scene

phase = (time.monotonic() % (2.0 * math.pi))

palette = [
    (1.0, 0.2, 0.2),   # red
    (0.2, 1.0, 0.4),   # green
    (0.2, 0.4, 1.0),   # blue
    (1.0, 0.9, 0.2),   # yellow
]

lights = scene.get_lights()
for i, light in enumerate(lights):
    if light.light_type not in (2, 3):  # Spot / Point
        continue
    color = palette[i % len(palette)]
    light.color = (color[0] * (0.7 + 0.3 * math.sin(phase + i)),
                   color[1] * (0.7 + 0.3 * math.sin(phase + i + 1.2)),
                   color[2] * (0.7 + 0.3 * math.sin(phase + i + 2.4)))
    light.intensity = max(50.0, light.intensity)

caustica.log_info(f"Updated {len(lights)} lights at phase {phase:.2f}")
