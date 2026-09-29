#include <scene/loader/ColladaLoader.h>
#include <scene/loader/UrdfImporter.h>
#include <scene/SceneEcs.h>
#include <scene/SceneImport.h>
#include <scene/SceneObjects.h>
#include <scene/SceneTypes.h>
#include <assets/AssetRegistry.h>
#include <assets/AssetStore.h>
#include <assets/ImageAsset.h>
#include <assets/loader/TextureLoader.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
bool g_Failed = false;

void expect(bool condition, const std::string& message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << "\n";
    g_Failed = true;
}

bool near(float a, float b, float eps = 1e-4f)
{
    return std::abs(a - b) <= eps;
}

bool near3(caustica::math::float3 v, float x, float y, float z, float eps = 1e-4f)
{
    return near(v.x, x, eps) && near(v.y, y, eps) && near(v.z, z, eps);
}

class TempDir
{
public:
    explicit TempDir(const std::string& name)
        : path(std::filesystem::temp_directory_path() / name)
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

void writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary);
    file << text;
}

const caustica::ColladaMaterialInfo* findMaterial(const caustica::ColladaMeshData& mesh, const std::string& id)
{
    auto found = mesh.materials.find(id);
    return found == mesh.materials.end() ? nullptr : &found->second;
}

const caustica::ColladaPrimitive* findPrimitive(const caustica::ColladaMeshData& mesh, const std::string& materialId)
{
    for (const caustica::ColladaPrimitive& primitive : mesh.primitives)
    {
        if (primitive.materialId == materialId)
            return &primitive;
    }
    return nullptr;
}

std::string phongDae(const std::string& upAxis, const std::string& meter, const std::string& extraEffect, const std::string& geometryXml, const std::string& sceneXml)
{
    return
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<COLLADA version=\"1.4.1\">\n"
        "  <asset><unit name=\"meter\" meter=\"" + meter + "\"/><up_axis>" + upAxis + "</up_axis></asset>\n"
        "  <library_effects>\n"
        "    <effect id=\"white-effect\"><profile_COMMON><technique sid=\"common\"><phong>\n"
        "      <emission><color>0 0 0 1</color></emission>\n"
        "      <diffuse><color>1 1 1 1</color></diffuse>\n"
        "      <specular><color>1 1 1 1</color></specular>\n"
        "      <shininess><float>0</float></shininess>\n"
        "      <transparent opaque=\"A_ONE\"><color>1 1 1 1</color></transparent>\n"
        "    </phong></technique></profile_COMMON></effect>\n"
        "    <effect id=\"dark-effect\"><profile_COMMON><technique sid=\"common\"><phong>\n"
        "      <diffuse><color>0.25 0.25 0.25 1</color></diffuse>\n"
        "      <specular><color>0.25 0.25 0.25 1</color></specular>\n"
        "      <shininess><float>0</float></shininess>\n"
        "      <emission><color>0.1 0.2 0.3 1</color></emission>\n"
        "    </phong></technique></profile_COMMON></effect>\n"
        + extraEffect +
        "  </library_effects>\n"
        "  <library_materials>\n"
        "    <material id=\"white-material\" name=\"white\"><instance_effect url=\"#white-effect\"/></material>\n"
        "    <material id=\"dark-material\" name=\"dark\"><instance_effect url=\"#dark-effect\"/></material>\n"
        "  </library_materials>\n"
        "  <library_geometries>\n" + geometryXml +
        "  </library_geometries>\n"
        "  <library_visual_scenes><visual_scene id=\"Scene\">\n" + sceneXml +
        "  </visual_scene></library_visual_scenes>\n"
        "  <scene><instance_visual_scene url=\"#Scene\"/></scene>\n"
        "</COLLADA>\n";
}

std::string triangleGeometry(const std::string& id, const std::string& positions, const std::string& materialSymbol)
{
    return
        "    <geometry id=\"" + id + "-mesh\"><mesh>\n"
        "      <source id=\"" + id + "-pos\"><float_array id=\"" + id + "-pos-array\" count=\"9\">" + positions + "</float_array>\n"
        "        <technique_common><accessor source=\"#" + id + "-pos-array\" count=\"3\" stride=\"3\">\n"
        "          <param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" type=\"float\"/>\n"
        "        </accessor></technique_common></source>\n"
        "      <source id=\"" + id + "-nrm\"><float_array id=\"" + id + "-nrm-array\" count=\"3\">0 0 1</float_array>\n"
        "        <technique_common><accessor source=\"#" + id + "-nrm-array\" count=\"1\" stride=\"3\">\n"
        "          <param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" type=\"float\"/>\n"
        "        </accessor></technique_common></source>\n"
        "      <vertices id=\"" + id + "-vertices\"><input semantic=\"POSITION\" source=\"#" + id + "-pos\"/></vertices>\n"
        "      <triangles material=\"" + materialSymbol + "\" count=\"1\">\n"
        "        <input semantic=\"VERTEX\" source=\"#" + id + "-vertices\" offset=\"0\"/>\n"
        "        <input semantic=\"NORMAL\" source=\"#" + id + "-nrm\" offset=\"1\"/>\n"
        "        <p>0 0 1 0 2 0</p>\n"
        "      </triangles>\n"
        "    </mesh></geometry>\n";
}

void testPhongColorsAndIndices()
{
    TempDir dir("caustica-collada-phong");
    const std::string xml = phongDae(
        "Z_UP", "1", "",
        triangleGeometry("white", "1 0 0  0 1 0  0 0 1", "white-symbol")
            + triangleGeometry("dark", "2 0 0  0 2 0  0 0 2", "dark-symbol"),
        "    <node id=\"n0\"><matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>\n"
        "      <instance_geometry url=\"#white-mesh\"><bind_material><technique_common>\n"
        "        <instance_material symbol=\"white-symbol\" target=\"#white-material\"/>\n"
        "      </technique_common></bind_material></instance_geometry></node>\n"
        "    <node id=\"n1\"><instance_geometry url=\"#dark-mesh\"><bind_material><technique_common>\n"
        "      <instance_material symbol=\"dark-symbol\" target=\"#dark-material\"/>\n"
        "    </technique_common></bind_material></instance_geometry></node>\n");
    const auto path = dir.path / "link.dae";
    writeFile(path, xml);

    caustica::ColladaMeshData mesh;
    expect(caustica::loadColladaFile(path, mesh), "phong dae failed to load");
    expect(mesh.primitives.size() == 2, "expected two primitives");

    const auto* white = findMaterial(mesh, "white-material");
    const auto* dark = findMaterial(mesh, "dark-material");
    expect(white && dark, "missing phong materials");
    if (white)
    {
        expect(near3(white->diffuse, 1.f, 1.f, 1.f), "white diffuse");
        expect(near3(white->specular, 1.f, 1.f, 1.f), "white specular");
        expect(near(white->shininess, 0.f), "white shininess");
        expect(near(white->roughness, 1.f), "shininess 0 should be roughness 1");
        expect(near(white->metalness, 0.f), "bright diffuse stays dielectric");
        expect(near3(white->baseColor, 1.f, 1.f, 1.f), "white albedo");
        expect(near(white->opacity, 1.f), "opaque transparent color");
    }
    if (dark)
    {
        expect(near3(dark->diffuse, 0.25f, 0.25f, 0.25f), "dark diffuse");
        expect(near3(dark->emission, 0.1f, 0.2f, 0.3f), "dark emission");
        expect(near3(dark->baseColor, 0.25f, 0.25f, 0.25f), "dark albedo");
    }

    const auto* whitePrim = findPrimitive(mesh, "white-material");
    expect(whitePrim && whitePrim->indices.size() == 3, "white triangle index count");
    if (whitePrim && !whitePrim->positions.empty())
    {
        expect(near3(whitePrim->positions[0], 1.f, 0.f, 0.f), "interleaved VERTEX/NORMAL position");
        expect(near3(whitePrim->normals[0], 0.f, 0.f, 1.f), "interleaved normal");
    }
}

void testUpAxisUnitAndMatrix()
{
    TempDir dir("caustica-collada-axis");
    const std::string geometry =
        "    <geometry id=\"up-mesh\"><mesh>\n"
        "      <source id=\"up-pos\"><float_array id=\"up-pos-array\" count=\"9\">0 1 0  1 1 0  0 1 1</float_array>\n"
        "        <technique_common><accessor source=\"#up-pos-array\" count=\"3\" stride=\"3\">\n"
        "          <param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" type=\"float\"/>\n"
        "        </accessor></technique_common></source>\n"
        "      <vertices id=\"up-vertices\"><input semantic=\"POSITION\" source=\"#up-pos\"/></vertices>\n"
        "      <triangles material=\"white-material\" count=\"1\">\n"
        "        <input semantic=\"VERTEX\" source=\"#up-vertices\" offset=\"0\"/><p>0 1 2</p>\n"
        "      </triangles></mesh></geometry>\n";
    const std::string scene =
        "    <node><translate>0 0 0</translate><instance_geometry url=\"#up-mesh\">\n"
        "      <bind_material><technique_common><instance_material symbol=\"white-material\" target=\"#white-material\"/></technique_common></bind_material>\n"
        "    </instance_geometry></node>\n";
    const auto path = dir.path / "yup.dae";
    writeFile(path, phongDae("Y_UP", "1", "", geometry, scene));

    caustica::ColladaMeshData mesh;
    expect(caustica::loadColladaFile(path, mesh), "Y_UP dae failed");
    const auto* prim = findPrimitive(mesh, "white-material");
    expect(prim && prim->positions.size() >= 1, "Y_UP primitive");
    if (prim && !prim->positions.empty())
        expect(near3(prim->positions[0], 0.f, 0.f, 1.f), "Y_UP (0,1,0) should become Z-up (0,0,1)");

    const std::string scaledGeometry =
        "    <geometry id=\"unit-mesh\"><mesh>\n"
        "      <source id=\"unit-pos\"><float_array id=\"unit-pos-array\" count=\"9\">100 0 0  0 0 0  0 100 0</float_array>\n"
        "        <technique_common><accessor source=\"#unit-pos-array\" count=\"3\" stride=\"3\">\n"
        "          <param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" type=\"float\"/>\n"
        "        </accessor></technique_common></source>\n"
        "      <vertices id=\"unit-vertices\"><input semantic=\"POSITION\" source=\"#unit-pos\"/></vertices>\n"
        "      <triangles material=\"white-material\" count=\"1\"><input semantic=\"VERTEX\" source=\"#unit-vertices\" offset=\"0\"/><p>0 1 2</p></triangles>\n"
        "    </mesh></geometry>\n";
    const auto unitPath = dir.path / "unit.dae";
    writeFile(unitPath, phongDae("Z_UP", "0.01", "", scaledGeometry,
        "<node><instance_geometry url=\"#unit-mesh\"><bind_material><technique_common>"
        "<instance_material symbol=\"white-material\" target=\"#white-material\"/></technique_common></bind_material></instance_geometry></node>\n"));
    caustica::ColladaMeshData unitMesh;
    expect(caustica::loadColladaFile(unitPath, unitMesh), "unit dae failed");
    const auto* unitPrim = findPrimitive(unitMesh, "white-material");
    if (unitPrim && !unitPrim->positions.empty())
        expect(near3(unitPrim->positions[0], 1.f, 0.f, 0.f), "meter=0.01 should scale 100 to 1");

    const std::string movedGeometry =
        "    <geometry id=\"move-mesh\"><mesh>\n"
        "      <source id=\"move-pos\"><float_array id=\"move-pos-array\" count=\"9\">0 0 0  1 0 0  0 1 0</float_array>\n"
        "        <technique_common><accessor source=\"#move-pos-array\" count=\"3\" stride=\"3\">\n"
        "          <param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" type=\"float\"/>\n"
        "        </accessor></technique_common></source>\n"
        "      <vertices id=\"move-vertices\"><input semantic=\"POSITION\" source=\"#move-pos\"/></vertices>\n"
        "      <triangles material=\"white-material\" count=\"1\"><input semantic=\"VERTEX\" source=\"#move-vertices\" offset=\"0\"/><p>0 1 2</p></triangles>\n"
        "    </mesh></geometry>\n";
    const auto movePath = dir.path / "move.dae";
    writeFile(movePath, phongDae("Z_UP", "1", "", movedGeometry,
        "<node><matrix>1 0 0 4 0 1 0 5 0 0 1 6 0 0 0 1</matrix><instance_geometry url=\"#move-mesh\">"
        "<bind_material><technique_common><instance_material symbol=\"white-material\" target=\"#white-material\"/></technique_common></bind_material>"
        "</instance_geometry></node>\n"));
    caustica::ColladaMeshData moved;
    expect(caustica::loadColladaFile(movePath, moved), "matrix dae failed");
    const auto* movedPrim = findPrimitive(moved, "white-material");
    if (movedPrim && !movedPrim->positions.empty())
        expect(near3(movedPrim->positions[0], 4.f, 5.f, 6.f), "row-major matrix translation");
}

void testPolylistTransparencyMetalAndTexture()
{
    TempDir dir("caustica-collada-extra");
    const std::string effects =
        "    <effect id=\"glass-effect\"><profile_COMMON><technique sid=\"common\"><phong>\n"
        "      <diffuse><color>0.2 0.4 0.9 1</color></diffuse>\n"
        "      <specular><color>1 1 1 1</color></specular>\n"
        "      <shininess><float>50</float></shininess>\n"
        "      <transparent opaque=\"A_ONE\"><color>1 1 1 0.25</color></transparent>\n"
        "    </phong></technique></profile_COMMON></effect>\n"
        "    <effect id=\"metal-effect\"><profile_COMMON><technique sid=\"common\"><phong>\n"
        "      <diffuse><color>0 0 0 1</color></diffuse>\n"
        "      <specular><color>0.8 0.6 0.2 1</color></specular>\n"
        "      <shininess><float>50</float></shininess>\n"
        "    </phong></technique></profile_COMMON></effect>\n"
        "    <effect id=\"tex-effect\"><profile_COMMON>\n"
        "      <newparam sid=\"surface\"><surface type=\"2D\"><init_from>albedo-image</init_from></surface></newparam>\n"
        "      <newparam sid=\"sampler\"><sampler2D><source>surface</source></sampler2D></newparam>\n"
        "      <technique sid=\"common\"><lambert><diffuse><texture texture=\"sampler\" texcoord=\"UVMap\"/></diffuse></lambert></technique>\n"
        "    </profile_COMMON></effect>\n";
    const std::string materials =
        "    <material id=\"glass-material\" name=\"glass\"><instance_effect url=\"#glass-effect\"/></material>\n"
        "    <material id=\"metal-material\" name=\"metal\"><instance_effect url=\"#metal-effect\"/></material>\n"
        "    <material id=\"tex-material\" name=\"tex\"><instance_effect url=\"#tex-effect\"/></material>\n";
    const std::string xml =
        "<?xml version=\"1.0\"?>\n<COLLADA version=\"1.4.1\"><asset><unit meter=\"1\"/><up_axis>Z_UP</up_axis></asset>\n"
        "<library_images><image id=\"albedo-image\"><init_from>images/albedo.png</init_from></image></library_images>\n"
        "<library_effects>\n" + effects + "</library_effects>\n"
        "<library_materials>\n" + materials + "</library_materials>\n"
        "<library_geometries>\n"
        "  <geometry id=\"quad-mesh\"><mesh>\n"
        "    <source id=\"quad-pos\"><float_array id=\"quad-pos-array\" count=\"12\">0 0 0  1 0 0  1 1 0  0 1 0</float_array>\n"
        "      <technique_common><accessor source=\"#quad-pos-array\" count=\"4\" stride=\"3\"><param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" type=\"float\"/></accessor></technique_common></source>\n"
        "    <vertices id=\"quad-vertices\"><input semantic=\"POSITION\" source=\"#quad-pos\"/></vertices>\n"
        "    <polylist material=\"glass-material\" count=\"1\"><input semantic=\"VERTEX\" source=\"#quad-vertices\" offset=\"0\"/><vcount>4</vcount><p>0 1 2 3</p></polylist>\n"
        "  </mesh></geometry>\n"
        "  <geometry id=\"metal-mesh\"><mesh>\n"
        "    <source id=\"metal-pos\"><float_array id=\"metal-pos-array\" count=\"9\">0 0 0  1 0 0  0 1 0</float_array>\n"
        "      <technique_common><accessor source=\"#metal-pos-array\" count=\"3\" stride=\"3\"><param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" type=\"float\"/></accessor></technique_common></source>\n"
        "    <vertices id=\"metal-vertices\"><input semantic=\"POSITION\" source=\"#metal-pos\"/></vertices>\n"
        "    <triangles material=\"metal-material\" count=\"1\"><input semantic=\"VERTEX\" source=\"#metal-vertices\" offset=\"0\"/><p>0 1 2</p></triangles>\n"
        "  </mesh></geometry>\n"
        "  <geometry id=\"tex-mesh\"><mesh>\n"
        "    <source id=\"tex-pos\"><float_array id=\"tex-pos-array\" count=\"9\">0 0 0  1 0 0  0 1 0</float_array>\n"
        "      <technique_common><accessor source=\"#tex-pos-array\" count=\"3\" stride=\"3\"><param name=\"X\" type=\"float\"/><param name=\"Y\" type=\"float\"/><param name=\"Z\" type=\"float\"/></accessor></technique_common></source>\n"
        "    <vertices id=\"tex-vertices\"><input semantic=\"POSITION\" source=\"#tex-pos\"/></vertices>\n"
        "    <triangles material=\"tex-material\" count=\"1\"><input semantic=\"VERTEX\" source=\"#tex-vertices\" offset=\"0\"/><p>0 1 2</p></triangles>\n"
        "  </mesh></geometry>\n"
        "</library_geometries>\n"
        "<library_visual_scenes><visual_scene id=\"Scene\">\n"
        "  <node><instance_geometry url=\"#quad-mesh\"/></node>\n"
        "  <node><instance_geometry url=\"#metal-mesh\"/></node>\n"
        "  <node><instance_geometry url=\"#tex-mesh\"/></node>\n"
        "</visual_scene></library_visual_scenes>\n"
        "<scene><instance_visual_scene url=\"#Scene\"/></scene></COLLADA>\n";
    const auto path = dir.path / "extra.dae";
    writeFile(path, xml);

    caustica::ColladaMeshData mesh;
    expect(caustica::loadColladaFile(path, mesh), "extra dae failed");
    const auto* glassPrim = findPrimitive(mesh, "glass-material");
    expect(glassPrim && glassPrim->indices.size() == 6, "polylist quad should become two triangles");
    const auto* glass = findMaterial(mesh, "glass-material");
    if (glass)
    {
        expect(near(glass->opacity, 0.25f), "A_ONE alpha is opacity");
        expect(near3(glass->baseColor, 0.2f, 0.4f, 0.9f), "shiny plastic keeps diffuse");
        expect(near(glass->metalness, 0.f), "shiny plastic stays dielectric");
        expect(near(glass->roughness, std::sqrt(2.f / 52.f), 1e-4f), "phong exponent 50 roughness");
    }
    const auto* metal = findMaterial(mesh, "metal-material");
    if (metal)
    {
        expect(near3(metal->baseColor, 0.8f, 0.6f, 0.2f), "dark diffuse with strong specular becomes the metal color");
        expect(metal->metalness > 0.5f, "metalness from phong specular");
    }
    const auto* tex = findMaterial(mesh, "tex-material");
    if (tex)
    {
        expect(tex->diffuseTexture.filename() == "albedo.png", "diffuse texture filename");
        expect(near3(tex->baseColor, 1.f, 1.f, 1.f), "textured diffuse without a color factor stays white");
    }
}

void testUrdfUsesColladaMaterials()
{
    TempDir dir("caustica-collada-urdf");
    const std::string dae = phongDae(
        "Z_UP", "1", "",
        triangleGeometry("white", "1 0 0  0 1 0  0 0 1", "white-symbol")
            + triangleGeometry("dark", "2 0 0  0 2 0  0 0 2", "dark-symbol"),
        "    <node><instance_geometry url=\"#white-mesh\"><bind_material><technique_common>\n"
        "      <instance_material symbol=\"white-symbol\" target=\"#white-material\"/></technique_common></bind_material></instance_geometry></node>\n"
        "    <node><instance_geometry url=\"#dark-mesh\"><bind_material><technique_common>\n"
        "      <instance_material symbol=\"dark-symbol\" target=\"#dark-material\"/></technique_common></bind_material></instance_geometry></node>\n");
    writeFile(dir.path / "part.dae", dae);
    writeFile(dir.path / "tri.stl",
        "solid t\n"
        "  facet normal 0 0 1\n"
        "    outer loop\n"
        "      vertex 0 0 0\n"
        "      vertex 1 0 0\n"
        "      vertex 0 1 0\n"
        "    endloop\n"
        "  endfacet\n"
        "endsolid t\n");
    writeFile(dir.path / "robot.urdf",
        "<robot name=\"demo\">\n"
        "  <link name=\"base\">\n"
        "    <visual>\n"
        "      <material name=\"red\"><color rgba=\"1 0 0 1\"/></material>\n"
        "      <geometry><mesh filename=\"part.dae\" scale=\"2 1 1\"/></geometry>\n"
        "    </visual>\n"
        "    <visual>\n"
        "      <material name=\"red\"><color rgba=\"1 0 0 1\"/></material>\n"
        "      <geometry><mesh filename=\"tri.stl\"/></geometry>\n"
        "    </visual>\n"
        "  </link>\n"
        "</robot>\n");

    caustica::AssetRegistry registry;
    caustica::AssetStore<caustica::ImageAsset> images;
    caustica::TextureLoader textures(nullptr, nullptr, nullptr, registry, images);
    caustica::SceneTypeFactory factory;
    caustica::SceneLoadingStats stats{};
    caustica::SceneImportResult result;
    caustica::UrdfImporter importer(nullptr, std::make_shared<caustica::SceneTypeFactory>());
    expect(importer.load(dir.path / "robot.urdf", textures, stats, false, result), "urdf import failed");
    (void)factory;

    std::vector<std::shared_ptr<caustica::MeshInfo>> meshes;
    if (result.entityWorld)
    {
        result.entityWorld->world().each<caustica::scene::MeshInstanceComponent>(
            [&](caustica::ecs::Entity, caustica::scene::MeshInstanceComponent& instance)
            {
                if (instance.mesh)
                    meshes.push_back(instance.mesh);
            });
    }
    expect(meshes.size() == 2, "urdf should spawn dae and stl visuals");

    const caustica::MeshInfo* daeMesh = nullptr;
    const caustica::MeshInfo* stlMesh = nullptr;
    for (const auto& mesh : meshes)
    {
        if (mesh->geometries.size() >= 2)
            daeMesh = mesh.get();
        else if (mesh->geometries.size() == 1)
            stlMesh = mesh.get();
    }
    expect(daeMesh && stlMesh, "missing dae or stl mesh");
    if (daeMesh)
    {
        expect(daeMesh->geometries.size() == 2, "dae visual should keep two materials");
        bool sawWhite = false;
        bool sawDark = false;
        bool sawScaled = false;
        for (const auto& geometry : daeMesh->geometries)
        {
            expect(geometry && geometry->material, "dae geometry material");
            if (!geometry || !geometry->material)
                continue;
            const auto& color = geometry->material->baseOrDiffuseColor;
            if (near3(color, 1.f, 1.f, 1.f))
                sawWhite = true;
            if (near3(color, 0.25f, 0.25f, 0.25f))
                sawDark = true;
            expect(!near3(color, 1.f, 0.f, 0.f), "urdf red must not replace a bound collada material");
        }
        expect(sawWhite && sawDark, "dae geometries lost diffuse colors");
        if (daeMesh->buffers)
        {
            for (const auto& position : daeMesh->buffers->positionData)
            {
                if (near(position.x, 2.f) && near(position.y, 0.f) && near(position.z, 0.f))
                    sawScaled = true;
            }
        }
        expect(sawScaled, "urdf mesh scale should apply to collada vertices");
    }
    if (stlMesh && stlMesh->geometries.front() && stlMesh->geometries.front()->material)
        expect(near3(stlMesh->geometries.front()->material->baseOrDiffuseColor, 1.f, 0.f, 0.f), "stl keeps the urdf color");
}

void testFrankaLink0()
{
    const std::filesystem::path path =
        "D:/is6/Lib/site-packages/isaacsim/exts/isaacsim.asset.importer.urdf/data/urdf/robots/franka_description/meshes/visual/link0.dae";
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        std::cout << "skip Franka link0.dae (not present)\n";
        return;
    }

    caustica::ColladaMeshData mesh;
    expect(caustica::loadColladaFile(path, mesh), "Franka link0.dae failed");
    expect(mesh.materials.size() == 12, "Franka link0 material count");
    expect(mesh.primitives.size() == 12, "Franka link0 primitive count");

    const auto* light = findMaterial(mesh, "Part__Feature022_001-material");
    const auto* dark = findMaterial(mesh, "Part__Feature023_001-material");
    const auto* shell = findMaterial(mesh, "Part__Feature019_001-material");
    expect(light && near3(light->diffuse, 0.9019608f, 0.9215686f, 0.9294118f, 1e-5f), "Franka light shell diffuse");
    expect(dark && near3(dark->diffuse, 0.2509804f, 0.2509804f, 0.2509804f, 1e-5f), "Franka dark part diffuse");
    expect(shell && near3(shell->baseColor, 1.f, 1.f, 1.f), "Franka large shell albedo");
    if (light)
    {
        expect(near(light->opacity, 1.f), "Franka opacity");
        expect(near(light->roughness, 1.f), "Franka shininess 0");
        expect(near(light->metalness, 0.f), "Franka light shell is dielectric");
    }

    const auto* large = findPrimitive(mesh, "Part__Feature019_001-material");
    expect(large && large->indices.size() / 3 == 13185, "Franka large shell triangle count");

    size_t triangles = 0;
    caustica::math::box3 bounds = caustica::math::box3::empty();
    for (const auto& primitive : mesh.primitives)
    {
        triangles += primitive.indices.size() / 3;
        bounds |= primitive.bounds;
    }
    expect(triangles == 20483, "Franka link0 triangle total");
    const auto size = bounds.diagonal();
    const float extent = std::max(size.x, std::max(size.y, size.z));
    expect(extent > 0.01f && extent < 2.f, "Franka link0 should stay in metres");
}
}

int main()
{
    testPhongColorsAndIndices();
    testUpAxisUnitAndMatrix();
    testPolylistTransparencyMetalAndTexture();
    testUrdfUsesColladaMaterials();
    testFrankaLink0();
    if (g_Failed)
    {
        std::cerr << "Collada loader tests failed\n";
        return 1;
    }
    std::cout << "Collada loader tests passed\n";
    return 0;
}
