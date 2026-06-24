#pragma once

#include <core/scope.h>
#include <imgui.h>

// Editor UI convenience macros — migrated from the former SampleCommon.h
#define UI_SCOPED_INDENT(indent) RAII_SCOPE(ImGui::Indent(indent); , ImGui::Unindent(indent); )
#define UI_SCOPED_DISABLE(cond)  RAII_SCOPE(ImGui::BeginDisabled(cond); , ImGui::EndDisabled(); )
