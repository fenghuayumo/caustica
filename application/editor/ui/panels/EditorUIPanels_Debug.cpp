#include "ui/EditorUIInternal.h"

#include "SceneEditor.h"
#include "SceneEditor.h"
#include "common/ImGuiManager.h"

#include <render/core/PathTracerSettings.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneGaussianSplatPasses.h>
#include <engine/UserInterfaceUtils.h>
#include <core/vfs/VFS.h>
#include <scene/SceneTypes.h>
#include <imgui_internal.h>
#include <assets/loader/ShaderFactory.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/passes/debug/Korgi.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <game/GameScene.h>
#include <render/passes/debug/ZoomTool.h>
#include <common/CaptureScriptManager.h>

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace caustica;
using namespace caustica::editor;

namespace caustica::editor
{

#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
#include "ImNodesEz.h"
#endif
void EditorUI::BuildDeltaTreeExplorerPanel(const PanelLayout& layout)
{
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
    if (m_editorUI.ShowDeltaTree)
    {
        float scaledWindowWidth = layout.scaledWidth - layout.defWindowWidth - 20;
        ImGui::SetNextWindowPos(ImVec2(layout.scaledWidth - float(scaledWindowWidth) - 10, 10.f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(scaledWindowWidth, scaledWindowWidth * 0.5f), ImGuiCond_FirstUseEver);
        const DeltaTreeVizHeader& DeltaTreeVizHeader = m_sceneEditor.feedbackData().deltaPathTree;
        char windowName[1024];
        snprintf(windowName, sizeof(windowName), "Delta Tree Explorer, pixel (%d, %d), sampleIndex: %d, nodes: %d###DeltaExplorer", DeltaTreeVizHeader.pixelPos.x, DeltaTreeVizHeader.pixelPos.y, DeltaTreeVizHeader.sampleIndex, DeltaTreeVizHeader.nodeCount);

        if (ImGui::Begin(windowName, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImGui::PushItemWidth(layout.defItemWidth);
            buildDeltaTreeViz();
            ImGui::PopItemWidth();
        }
        ImGui::End();
    }
#endif


}

void EditorUI::BuildGameStandalonePanel(const PanelLayout& layout)
{
    if ( m_sceneEditor.GetGame() != nullptr && m_sceneEditor.GetGame()->IsInitialized() )
    {
        const auto view = m_sceneEditor.currentView();
        if (view)
            m_sceneEditor.GetGame()->StandaloneGUI(view, float2(m_sceneEditor.displaySize()));
    }

    // ImGui::ShowDemoWindow();

}


void EditorUI::buildDeltaTreeViz()
{
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
    // make tiny scaling
    int localScaleIndex = FindBestScaleFontIndex(m_currentScale*0.75f);
    float localScale = m_scaledFonts[localScaleIndex].second;
    ImGui::PushFont(m_scaledFonts[localScaleIndex].first);
    ImGuiStyle& style = ImGui::GetStyle(); 
    style = m_defaultStyle;
    style.ScaleAllSizes(localScale);

    // fixed a lot of stability issues so this no longer needed - probably, leaving in just for a bit longer
    // // Unfortunately, the ImNodes are unstable when changed every frame. At some point they can be dropped and all drawing done ourselves, since we do the layout anyway and only use it for drawing connections which we can do.
    // // Until that's done, we have to cache and only update once every few frames.
    // static DeltaTreeVizHeader cachedHeader = DeltaTreeVizHeader::make();
    // static DeltaTreeVizPathVertex cachedVertices[cDeltaTreeVizMaxVertices];
    // {
    //     static int frameCounter = 0; frameCounter++;
    //     static int lastUpdated = -10;
    //     if ((frameCounter - lastUpdated) > 0)
    //     {
    //         lastUpdated = frameCounter;
    //         cachedHeader = m_sceneEditor.feedbackData().deltaPathTree;
    //         memcpy( cachedVertices, m_sceneEditor.debugDeltaPathTree(), sizeof(DeltaTreeVizPathVertex)*cDeltaTreeVizMaxVertices );
    //     }
    // }
    const DeltaTreeVizHeader& DeltaTreeVizHeader   = m_sceneEditor.feedbackData().deltaPathTree; // cachedHeader;
    const DeltaTreeVizPathVertex* deltaPathTreeVertices = m_sceneEditor.debugDeltaPathTree(); // cachedVertices;
    const int nodeCount = DeltaTreeVizHeader.nodeCount;

    ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine(); ImGui::NewLine();
    ImGui::Text( "Stable planes branch IDs:" );
    for (int i = 0; i < cStablePlaneCount; i++)
    {
        ImGui::Text( " %d: 0x%08x (%d dec)", i, DeltaTreeVizHeader.stableBranchIDs[i], DeltaTreeVizHeader.stableBranchIDs[i] );
        if (i == DeltaTreeVizHeader.dominantStablePlaneIndex)
        {
            ImGui::SameLine();
            ImGui::Text( " <DOMINANT>");
        }
    }

    ImNodes::Ez::BeginCanvas();

    ImVec2 topLeft = { ImGui::GetStyle().ItemSpacing.x * 8.0f, ImGui::GetStyle().ItemSpacing.y * 12.0f };
    ImVec2 nodeSize = {};
    const int nodeWidthInChars  = 28;
    const int nodeHeightInLines = 40;
    nodeSize.x = ImGui::CalcTextSize(std::string(' ', (size_t)nodeWidthInChars).c_str()).x;
    nodeSize.y = ImGui::GetStyle().ItemSpacing.y * nodeHeightInLines;
    ImVec2 nodePadding = ImVec2(nodeSize.x * 0.5f, nodeSize.y * 0.1f);

    struct UITreeNode
    {
        ImVec2                      pos;
        bool                        selected;
        std::string                 title;
        DeltaTreeVizPathVertex      deltaVertex;
        uint                        parentLobe;
        uint                        vertexIndex;
        std::shared_ptr<caustica::Material> material;  // nullptr for sky
        UITreeNode *                parent = nullptr;
        std::vector<UITreeNode *>   children;

        void init(const DeltaTreeVizPathVertex& deltaVertex, SceneEditor & app, const ImVec2 & nodeSize, const ImVec2 & nodePadding, const ImVec2 & topLeft)
        {   app;
            this->deltaVertex = deltaVertex;
            selected = false;
            vertexIndex = deltaVertex.vertexIndex ;
            parentLobe = deltaVertex.getParentLobe();
            
            float thpLum = dm::luminance(deltaVertex.throughput);

            char info[1024];
            snprintf(info, sizeof(info), "Vertex: %d, Throughput: %.1f%%", vertexIndex, thpLum*100.0f );
            title = info;
            if(deltaVertex.isDominant)
                title += " DOM";
            int padding = max( 0, nodeWidthInChars - (int)title.size() );
            title.append((size_t)padding, ' ');
            pos = topLeft;
            pos.x += (vertexIndex-1) * (nodeSize.x + nodePadding.x);
        }
    };

    UITreeNode treeNodes[cDeltaTreeVizMaxVertices];
    std::vector<std::vector<UITreeNode*>> nodeLevels;
    nodeLevels.resize( MAX_BOUNCE_COUNT+2 );
    int longestLevelCount = 0;
    for (int i = 0; i < nodeCount; i++)
    {
        UITreeNode & node = treeNodes[i];
        node.init(deltaPathTreeVertices[i], m_sceneEditor, nodeSize, nodePadding, topLeft);
        assert(node.vertexIndex < nodeLevels.size());
        nodeLevels[node.vertexIndex].push_back(&node);
        longestLevelCount = std::max(longestLevelCount, (int)nodeLevels[node.vertexIndex].size());
        // find parent - which is the last node with lower vertex index
        if (node.vertexIndex > 1) // vertex index 0 is camera, vertex index 1 is primary hit
        {
            assert( i>0 );
            for( int j = i-1; j >= 0; j-- )
                if (treeNodes[j].vertexIndex == node.vertexIndex - 1)
                {
                    node.parent = &treeNodes[j];
                    node.parent->children.push_back(&node);
                    break;
                }
            assert( node.parent != nullptr );
        }
    }

    // update Y positions, including parents
    for (int i = (int)nodeLevels.size() - 1; i >= 0; i--)
    {
        auto& level = nodeLevels[i];
        for (int npl = 0; npl < level.size(); npl++)
        {
            auto& node = level[npl];
            node->pos.y = topLeft.y + std::max(0, npl) * (nodeSize.y + nodePadding.y);
            // just make aligned to the top child if any - easier to see
            if (node->children.size() > 0)
            {
                float topChild = FLT_MAX;
                for (auto& child : node->children)
                    topChild = std::min(topChild, child->pos.y);
                node->pos.y = std::max(topChild, node->pos.y);
            }
        }
    }
    
    auto outSlotName = [](int lobeIndex){ return "D" + std::to_string(lobeIndex); };
    ImNodes::Ez::SlotInfo inS; inS.kind = 1; inS.title = "in";

    auto ImGuiColorInfo = [&]( const char * text, ImVec4 color, const char * tooltipText, auto... tooltipParams ) -> bool
    {
        char info[1024];
        snprintf(info, sizeof(info), "%.2f, %.2f, %.2f###%s", color.x, color.y, color.z, text);
        bool selected = true;
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, color);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, color);
        ImGui::PushStyleColor(ImGuiCol_Header, color);
        ImGui::Text("%s",text); ImGui::SameLine();
        ImGui::Selectable(info, true, 0, ImVec2(nodeSize.x*0.7f, 0) ); /*, ImGuiSelectableFlags_Disabled*/
        ImGui::PopStyleColor(3);
        if( ImGui::IsItemHovered() )
        {
            ImGui::SetTooltip(tooltipText, tooltipParams...);
            return true;
        }
        return false;
    };

    for (int i = 0; i < nodeCount; i++)
    {
        UITreeNode & treeNode = treeNodes[i];

        int onPlaneIndex = -1; bool onStablePath = false;
        for (int spi = 0; spi < cStablePlaneCount; spi++)
        {
            if (StablePlaneIsOnPlane(DeltaTreeVizHeader.stableBranchIDs[spi], treeNode.deltaVertex.stableBranchID))
            {
                onPlaneIndex = spi;
                onStablePath = true;
                break;
            }
            onStablePath |= StablePlaneIsOnStablePath(DeltaTreeVizHeader.stableBranchIDs[spi], treeNode.deltaVertex.stableBranchID);
        }
        auto mergeColor = [](ImVec4 & inout, ImVec4 ref) { inout = ImVec4( min(1.0f, inout.x + ref.x), min(1.0f, inout.y + ref.y), min(1.0f, inout.z + ref.z), inout.w ); };
        ImVec4 colorAdd = { 0,0.0f,0.0f,0.0f };
        if (onPlaneIndex >= 0)
            colorAdd = ImVec4((onPlaneIndex == 0) ? 0.5f : 0.0f, (onPlaneIndex == 1) ? 0.5f : 0.0f, (onPlaneIndex == 2) ? 0.5f : 0.0f, 1);
        else if (onStablePath)
            colorAdd = ImVec4(0.3f, 0.3f, 0.0f, 1);

        ImVec4 cola{ 0.22f, 0.22f, 0.22f, 1.0f };   mergeColor(cola, colorAdd);
        ImVec4 colb{ 0.32f, 0.32f, 0.32f, 1.0f };   mergeColor(colb, colorAdd);
        ImVec4 colc{ 0.5f, 0.5f, 0.5f, 1.0f };      mergeColor(colc, colorAdd);
        ImNodes::Ez::PushStyleColor(ImNodesStyleCol_NodeTitleBarBg, cola);
        ImNodes::Ez::PushStyleColor(ImNodesStyleCol_NodeTitleBarBgHovered, colb);
        ImNodes::Ez::PushStyleColor(ImNodesStyleCol_NodeTitleBarBgActive, colc);

        if (ImNodes::Ez::BeginNode(&treeNode, treeNode.title.c_str(), &treeNode.pos, &treeNode.selected))
        {
            bool isAnyHovered = ImGui::IsItemHovered();
            if (isAnyHovered)
                ImGui::SetTooltip("Stable delta tree branch ID: 0x%08x (%d dec)", treeNode.deltaVertex.stableBranchID, treeNode.deltaVertex.stableBranchID);

            ImNodes::Ez::InputSlots(&inS, 1);

            isAnyHovered |= ImGuiColorInfo("Thp:", ImVec4(treeNode.deltaVertex.throughput.x, treeNode.deltaVertex.throughput.y, treeNode.deltaVertex.throughput.z, 1.0f),
                "Throughput at current vertex: %.4f, %.4f, %.4f\nLast segment volume absorption was %.1f%%\n", treeNode.deltaVertex.throughput.x, treeNode.deltaVertex.throughput.y, treeNode.deltaVertex.throughput.z, treeNode.deltaVertex.volumeAbsorption*100.0f );

            std::string matName = ">>SKY<<";
            if( treeNode.deltaVertex.materialID != 0xFFFFFFFF )
            {
                treeNode.material = m_sceneEditor.findMaterial((int)treeNode.deltaVertex.materialID);
                if( treeNode.material != nullptr )
                    matName = treeNode.material->name; 
            }
            std::string matNameFull = matName;
            if( matName.length() > 30 ) matName = matName.substr(0, 30) + "...";

            ImGui::Text("Surface: %s", matName.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Surface info: %s", matNameFull.c_str());
                isAnyHovered = true;
            }

            ImGui::Text("Lobes: %d", treeNode.deltaVertex.deltaLobeCount);

            //ImGui::Col
            ImNodes::Ez::SlotInfo outS[cMaxDeltaLobes+1+3];
            int outSN = 0;
            outS[outSN++] = ImNodes::Ez::SlotInfo{ "", 0 }; // empty text to align with ^ text
            outS[outSN++] = ImNodes::Ez::SlotInfo{ "", 0 }; // empty text to align with ^ text
            outS[outSN++] = ImNodes::Ez::SlotInfo{ "", 0 }; // empty text to align with ^ text
            for (int j = 0; j < (int)treeNode.deltaVertex.deltaLobeCount; j++ )
            {
                auto lobe = treeNode.deltaVertex.deltaLobes[j];
                if( lobe.probability > 0 )
                    outS[outSN++] = ImNodes::Ez::SlotInfo{ outSlotName(j), 1 };
                isAnyHovered |= ImGuiColorInfo( (std::string(" D")+std::to_string(j) + ":").c_str(), ImVec4(lobe.thp.x, lobe.thp.y, lobe.thp.z, 1.0f),
                    "Delta lobe %d throughput: %.4f, %.4f, %.4f\nType: %s", j, lobe.thp.x, lobe.thp.y, lobe.thp.z, lobe.transmission?("transmission"):("reflection") );
            }

            ImGui::Text(" Non-delta: %.1f%%", treeNode.deltaVertex.nonDeltaPart*100.0f);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("This is the amount of throughput that gets handled by diffuse and rough specular lobes");
                isAnyHovered = true;
            }

            ImNodes::Ez::OutputSlots(outS, outSN);
            if (ImGui::IsItemHovered())
                isAnyHovered = true;
            ImNodes::Ez::EndNode();
            if (ImGui::IsItemHovered())
                isAnyHovered = true;

            if (isAnyHovered)
            {
                float3 worldPos = treeNode.deltaVertex.worldPos;
                float3 viewVec = worldPos - m_sceneEditor.currentCamera().getPosition();
                float sphereSize = 0.006f + 0.004f * dm::length(viewVec);
                float step = 0.15f;
                viewVec = dm::normalize(viewVec);
                float3 right = dm::cross(viewVec, m_sceneEditor.currentCamera().getUp());
                float3 up = dm::cross(right, viewVec);
                float3 prev0 = worldPos;
                float3 prev1 = worldPos;
                float3 prev2 = worldPos;
                for (float s = 0.0f; s < 2.06f; s += step)
                {
                    float px = cos(s * dm::PI_f);
                    float py = sin(s * dm::PI_f);
                    float3 sp0 = worldPos + up * py * sphereSize + right * px * sphereSize;
                    float3 sp1 = worldPos + up * py * sphereSize * 0.8f + right * px * sphereSize * 0.8f;
                    float3 sp2 = worldPos + up * py * sphereSize * 0.6f + right * px * sphereSize * 0.6f;
                    float4 col1 = float4(colorAdd.x, colorAdd.y, colorAdd.z, 1);//float4(1,1,1,1); //float3( fmodf((s+1)*13.33f,1), fmodf((s+1)*17.55f,1), fmodf((s+1)*23.77f,1));
                    float4 col0 = float4(0,0,0,1);
                    if( s > 0.0f )
                    {
                        m_sceneEditor.debugDrawLine(prev0, sp0, col1, col1); 
                        m_sceneEditor.debugDrawLine(prev1, sp1, col0, col0); 
                        m_sceneEditor.debugDrawLine(prev0, sp1, col1, col0);
                        m_sceneEditor.debugDrawLine(prev2, sp0, col1, col0);
                        m_sceneEditor.debugDrawLine(prev2, sp2, col1, col1);
                    }
                    prev0 = sp0; prev1 = sp1; prev2 = sp2;
                }
            }
        }
        ImNodes::Ez::PopStyleColor(3);
    }

    // update connections
    for (auto& level : nodeLevels)
        for (int npl = 0; npl < level.size(); npl++)
        {
            auto& node = level[npl];
            if (node->parent != nullptr)
                ImNodes::Connection(node, inS.title.c_str(), node->parent, outSlotName(node->parentLobe).c_str());
        }

    ImNodes::Ez::EndCanvas();

    // reset scaling
    style = m_defaultStyle;
    style.ScaleAllSizes(m_currentScale);
    ImGui::PopFont();
#endif
}


} // namespace caustica::editor

