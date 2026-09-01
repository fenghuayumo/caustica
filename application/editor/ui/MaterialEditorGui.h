#pragma once

struct StandardMaterial;
class MaterialGpuCache;

namespace caustica::editor
{

// Inspector controls for StandardMaterial. Lives in the editor so render
// does not own ImGui authoring UI.
bool DrawStandardMaterialEditor(StandardMaterial& material, MaterialGpuCache& cache);

} // namespace caustica::editor
