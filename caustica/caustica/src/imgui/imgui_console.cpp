#include <imgui/imgui_console.h>
#include <imgui/imgui_renderer.h>

#include <core/console/ConsoleInterpreter.h>
#include <core/console/ConsoleObjects.h>
#include <core/string_utils.h>

#include <algorithm>
#include <cstdarg>
#include <cctype>
#include <cstring>

using namespace caustica;
using namespace caustica;


namespace
{
// UE Output Log–inspired palette
constexpr ImVec4 kColLog      = { 0.82f, 0.84f, 0.86f, 1.f };
constexpr ImVec4 kColInfo     = { 0.55f, 0.78f, 1.00f, 1.f };
constexpr ImVec4 kColWarning  = { 1.00f, 0.78f, 0.22f, 1.f };
constexpr ImVec4 kColError    = { 1.00f, 0.38f, 0.35f, 1.f };
constexpr ImVec4 kColFatal    = { 1.00f, 0.20f, 0.25f, 1.f };
constexpr ImVec4 kColCommand  = { 0.45f, 0.92f, 0.62f, 1.f };
constexpr ImVec4 kColRowA     = { 0.10f, 0.11f, 0.13f, 1.f };
constexpr ImVec4 kColRowB     = { 0.12f, 0.13f, 0.16f, 1.f };
constexpr ImVec4 kColChildBg  = { 0.08f, 0.09f, 0.11f, 1.f };
constexpr ImVec4 kColToolbar  = { 0.14f, 0.15f, 0.18f, 1.f };

ImVec4 getSeverityColor(caustica::Severity severity)
{
	using namespace caustica;
	switch (severity)
	{
	case Severity::Debug:   return ImVec4(0.55f, 0.58f, 0.62f, 1.f);
	case Severity::Info:    return kColInfo;
	case Severity::Warning: return kColWarning;
	case Severity::Error:   return kColError;
	case Severity::Fatal:   return kColFatal;
	default:                return kColLog;
	}
}

const char* getSeverityTag(caustica::Severity severity)
{
	using namespace caustica;
	switch (severity)
	{
	case Severity::Debug:   return "Debug";
	case Severity::Info:    return "Log";
	case Severity::Warning: return "Warning";
	case Severity::Error:   return "Error";
	case Severity::Fatal:   return "Fatal";
	default:                return "Log";
	}
}

bool passesFilter(std::string const& text, char const* filter)
{
	if (!filter || filter[0] == '\0')
		return true;
	auto containsCi = [](std::string const& hay, char const* needle) {
		if (!needle || !*needle)
			return true;
		for (size_t i = 0; i < hay.size(); ++i)
		{
			size_t j = 0;
			while (needle[j]
				&& i + j < hay.size()
				&& std::tolower(static_cast<unsigned char>(hay[i + j]))
					== std::tolower(static_cast<unsigned char>(needle[j])))
			{
				++j;
			}
			if (!needle[j])
				return true;
		}
		return false;
	};
	return containsCi(text, filter);
}

bool severityChip(char const* label, bool* enabled, ImVec4 const& color)
{
	ImGui::PushID(label);
	const ImVec4 bg = *enabled
		? ImVec4(color.x * 0.35f, color.y * 0.35f, color.z * 0.35f, 1.f)
		: ImVec4(0.18f, 0.19f, 0.22f, 1.f);
	const ImVec4 border = *enabled ? color : ImVec4(0.30f, 0.32f, 0.36f, 1.f);
	ImGui::PushStyleColor(ImGuiCol_Button, bg);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(bg.x + 0.06f, bg.y + 0.06f, bg.z + 0.06f, 1.f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(bg.x + 0.10f, bg.y + 0.10f, bg.z + 0.10f, 1.f));
	ImGui::PushStyleColor(ImGuiCol_Border, border);
	ImGui::PushStyleColor(ImGuiCol_Text, *enabled ? color : ImVec4(0.55f, 0.57f, 0.60f, 1.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f, 3.f));
	const bool pressed = ImGui::Button(label);
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor(5);
	if (pressed)
		*enabled = !*enabled;
	ImGui::PopID();
	return pressed;
}

inline std::string_view isolateKeyword(std::string_view line)
{
	ds::ltrim(line);
	if (auto it = std::find_if(line.rbegin(), line.rend(), [](int ch) { return std::isspace(ch); });
		it != line.rend())
	{
		line.remove_prefix(static_cast<size_t>(std::distance(line.begin(), it.base())));
	}
	return line;
}
} // namespace

ImGui_Console::ImGui_Console(std::shared_ptr<console::Interpreter> interpreter, Options const& options) 
	: m_Options(options)
	, m_Interpreter(interpreter)
{
	if (options.capture_log)
	{
		// Keep a raw this pointer only while alive; destructor must resetCallback()
		// before GpuDevice/Streamline shutdown logs can fire into a destroyed buffer.
		caustica::setCallback([this](caustica::Severity severity, char const* msg) {
				ImVec4 color = getSeverityColor(severity);
				this->m_ItemsLog.push_back({severity, color, msg});
			});
	}
}

ImGui_Console::~ImGui_Console()
{
	if (m_Options.capture_log)
		caustica::resetCallback();
}

void ImGui_Console::Print(char const* fmt, ...)
{
	InputBuffer buf;
	std::va_list args;

	va_start(args, fmt);
	vsnprintf(buf.data(), buf.size(), fmt, args);
	buf.back() = 0;
	va_end(args);

	LogItem item;
	item.text = buf.data();
	item.textColor = (item.text.rfind("> ", 0) == 0) ? kColCommand : kColLog;
	m_ItemsLog.push_back(item);
}

void ImGui_Console::Print(std::string_view line)
{
	LogItem item;
	item.text = line;
	item.textColor = (item.text.rfind("> ", 0) == 0) ? kColCommand : kColLog;
	m_ItemsLog.push_back(item);
}

void ImGui_Console::clearLog()
{
	m_ItemsLog.clear();
}

void ImGui_Console::clearHistory()
{
	m_History.clear();
	m_HistoryIterator = m_History.rend();
}

void ImGui_Console::render(bool* open, bool requestFocus)
{
	ImGui::SetNextWindowSize(ImVec2(860, 480), ImGuiCond_FirstUseEver);
	if (requestFocus)
	{
		ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
		ImGui::SetNextWindowFocus();
	}

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 8.f));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.12f, 0.14f, 1.f));
	if (!ImGui::Begin("Console Log", open))
	{
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		return;
	}

	// --- Toolbar (UE Output Log style) ---
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, kColToolbar);
		ImGui::BeginChild("##ConsoleToolbar", ImVec2(0.f, ImGui::GetFrameHeightWithSpacing() + 6.f),
			true, ImGuiWindowFlags_NoScrollbar);
		if (ImGui::Button("Clear"))
			clearLog();
		ImGui::SameLine();
		ImGui::Checkbox("Auto-scroll", &m_Options.auto_scroll);
		ImGui::SameLine(0.f, 12.f);
		severityChip("Errors", &m_Options.show_errors, kColError);
		ImGui::SameLine();
		severityChip("Warnings", &m_Options.show_warnings, kColWarning);
		ImGui::SameLine();
		severityChip("Messages", &m_Options.show_info, kColInfo);
		ImGui::SameLine(0.f, 12.f);
		ImGui::SetNextItemWidth((std::max)(120.f, ImGui::GetContentRegionAvail().x));
		ImGui::InputTextWithHint("##ConsoleFilter", "Filter...", m_FilterBuf, sizeof(m_FilterBuf));
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	const float footerHeight =
		ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing() + 4.f;

	// --- Log list ---
	ImGui::PushStyleColor(ImGuiCol_ChildBg, kColChildBg);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
	ImGui::BeginChild(
		"##ConsoleLogList",
		ImVec2(0.f, -footerHeight),
		true,
		ImGuiWindowFlags_HorizontalScrollbar);

	if (ImGui::BeginPopupContextWindow())
	{
		if (ImGui::MenuItem("Clear Log"))
			clearLog();
		if (ImGui::MenuItem("Clear History"))
			clearHistory();
		ImGui::EndPopup();
	}

	if (m_Options.font)
		ImGui::PushFont(m_Options.font->getScaledFont());

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	int visibleRow = 0;
	for (auto const& item : m_ItemsLog)
	{
		using namespace caustica;

		bool showItem = true;
		switch (item.severity)
		{
		case Severity::Info:
		case Severity::Debug:
		case Severity::None:
			showItem = m_Options.show_info;
			break;
		case Severity::Warning:
			showItem = m_Options.show_warnings;
			break;
		case Severity::Error:
		case Severity::Fatal:
			showItem = m_Options.show_errors;
			break;
		default:
			break;
		}
		if (!showItem || !passesFilter(item.text, m_FilterBuf))
			continue;

		const ImVec4 severityColor = (item.severity == Severity::None)
			? item.textColor
			: getSeverityColor(item.severity);
		const char* tag = getSeverityTag(item.severity);

		const ImVec2 rowMin = ImGui::GetCursorScreenPos();
		const float rowW = ImGui::GetContentRegionAvail().x;
		const float rowH = ImGui::GetTextLineHeightWithSpacing();
		const ImVec2 rowMax(rowMin.x + rowW, rowMin.y + rowH);

		drawList->AddRectFilled(
			rowMin,
			rowMax,
			ImGui::ColorConvertFloat4ToU32((visibleRow & 1) ? kColRowB : kColRowA));
		// Severity accent bar (UE-style)
		drawList->AddRectFilled(
			rowMin,
			ImVec2(rowMin.x + 3.f, rowMax.y),
			ImGui::ColorConvertFloat4ToU32(severityColor));

		ImGui::SetCursorScreenPos(ImVec2(rowMin.x + 8.f, rowMin.y + 1.f));
		ImGui::PushStyleColor(ImGuiCol_Text, severityColor);
		ImGui::TextUnformatted(tag);
		ImGui::PopStyleColor();
		ImGui::SameLine(72.f);
		ImGui::PushStyleColor(ImGuiCol_Text, item.textColor);
		ImGui::TextUnformatted(item.text.c_str());
		ImGui::PopStyleColor();

		// Keep layout advancing even if text wrapped oddly.
		if (ImGui::GetCursorScreenPos().y < rowMax.y)
			ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y));

		++visibleRow;
	}

	if (m_Options.scroll_to_bottom
		|| (m_Options.auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f))
	{
		ImGui::SetScrollHereY(1.f);
	}

	if (m_Options.font)
		ImGui::PopFont();

	m_Options.scroll_to_bottom = false;
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	// --- Command prompt ---
	ImGui::Spacing();
	ImGui::AlignTextToFramePadding();
	ImGui::PushStyleColor(ImGuiCol_Text, kColCommand);
	ImGui::TextUnformatted(">");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1.f);

	if (m_Options.font)
		ImGui::PushFont(m_Options.font->getScaledFont());

	bool reclaim_focus = requestFocus;
	const auto flags = ImGuiInputTextFlags_EnterReturnsTrue
		| ImGuiInputTextFlags_CallbackCompletion
		| ImGuiInputTextFlags_CallbackHistory;
	if (requestFocus)
		ImGui::SetKeyboardFocusHere();
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.07f, 0.08f, 0.10f, 1.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);
	if (ImGui::InputText(
			"##Command",
			m_InputBuffer.data(),
			m_InputBuffer.size(),
			flags,
			[](ImGuiInputTextCallbackData* data) {
				return static_cast<ImGui_Console*>(data->UserData)->textEditCallback(data);
			},
			this))
	{
		if (m_InputBuffer.front() != '\0')
		{
			this->execCommand(m_InputBuffer.data());
			m_InputBuffer.front() = 0;
		}
		reclaim_focus = true;
	}
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	if (m_Options.font)
		ImGui::PopFont();

	ImGui::SetItemDefaultFocus();
	if (reclaim_focus)
		ImGui::SetKeyboardFocusHere(-1);

	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
}

std::vector<std::string> ImGui_Console::currentSuggestions() const
{
	if (!m_Interpreter)
		return {};
	const size_t cursor = std::strlen(m_InputBuffer.data());
	return m_Interpreter->suggest(m_InputBuffer.data(), cursor);
}

void ImGui_Console::applySuggestion(std::string const& suggestion)
{
	if (suggestion.empty())
		return;

	std::string_view cmdline(m_InputBuffer.data());
	std::string_view keyword = isolateKeyword(cmdline);
	const size_t keywordBegin = static_cast<size_t>(keyword.data() - cmdline.data());

	std::string rebuilt(cmdline.substr(0, keywordBegin));
	rebuilt += suggestion;
	if (rebuilt.empty() || rebuilt.back() != ' ')
		rebuilt.push_back(' ');

	std::snprintf(m_InputBuffer.data(), m_InputBuffer.size(), "%s", rebuilt.c_str());
	m_SuggestionIndex = -1;
}

void ImGui_Console::renderCommandBar(
	bool* open,
	bool requestFocus,
	ImVec2 anchorPos,
	ImVec2 anchorSize)
{
	if (!open || !*open)
		return;

	const ImGuiViewport* mainVp = ImGui::GetMainViewport();
	if (!mainVp)
		return;

	constexpr float kBarMargin = 8.f;
	constexpr float kBarHeight = 36.f;
	constexpr int kMaxSuggestions = 12;
	constexpr int kRecentLogLines = 8;

	// Prefer the editor Viewport rect so the bar does not cover Hierarchy / Inspector.
	ImVec2 regionPos = mainVp->WorkPos;
	ImVec2 regionSize = mainVp->WorkSize;
	if (anchorSize.x > 1.f && anchorSize.y > 1.f)
	{
		regionPos = anchorPos;
		regionSize = anchorSize;
	}

	m_Suggestions = currentSuggestions();
	if (m_SuggestionIndex >= static_cast<int>(m_Suggestions.size()))
		m_SuggestionIndex = static_cast<int>(m_Suggestions.size()) - 1;
	if (m_Suggestions.empty())
		m_SuggestionIndex = -1;
	else if (m_SuggestionIndex < 0 && !m_Suggestions.empty() && m_InputBuffer.front() != '\0')
		m_SuggestionIndex = 0;

	const float suggestRowH = ImGui::GetTextLineHeightWithSpacing();
	const int visibleSuggestions = std::min(kMaxSuggestions, static_cast<int>(m_Suggestions.size()));
	const float suggestH = visibleSuggestions > 0
		? (suggestRowH * static_cast<float>(visibleSuggestions) + 8.f)
		: 0.f;

	// Recent command/log output strip (so Enter results are visible without opening Console).
	std::vector<LogItem const*> recent;
	recent.reserve(kRecentLogLines);
	if (!m_ItemsLog.empty())
	{
		for (auto it = m_ItemsLog.rbegin();
			it != m_ItemsLog.rend() && recent.size() < static_cast<size_t>(kRecentLogLines);
			++it)
		{
			recent.push_back(&*it);
		}
		std::reverse(recent.begin(), recent.end());
	}
	const float logH = recent.empty()
		? 0.f
		: (suggestRowH * static_cast<float>(recent.size()) + 10.f);

	const float maxBarH = (std::max)(kBarHeight + 8.f, regionSize.y - kBarMargin * 2.f);
	const float totalH = (std::min)(kBarHeight + suggestH + logH + 16.f, maxBarH);
	const float barW = (std::max)(120.f, regionSize.x - kBarMargin * 2.f);
	const ImVec2 barPos(
		regionPos.x + kBarMargin,
		regionPos.y + regionSize.y - totalH - kBarMargin);
	const ImVec2 barSize(barW, totalH);

	ImGui::SetNextWindowPos(barPos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(barSize, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.94f);
	ImGui::SetNextWindowViewport(mainVp->ID);

	ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoDecoration
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoDocking
		| ImGuiWindowFlags_NoNavFocus
		| ImGuiWindowFlags_NoCollapse;

	if (!ImGui::Begin("##CommandBar", open, flags))
	{
		ImGui::End();
		return;
	}

	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		*open = false;
		ImGui::End();
		return;
	}

	if (m_Options.font)
		ImGui::PushFont(m_Options.font->getScaledFont());

	// Recent output
	if (!recent.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.06f, 0.85f));
		ImGui::BeginChild("##CommandBarLog", ImVec2(0.f, logH), true, ImGuiWindowFlags_NoScrollbar);
		for (LogItem const* item : recent)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, item->textColor);
			ImGui::TextUnformatted(item->text.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::SetScrollHereY(1.f);
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	// Live suggestion list
	if (visibleSuggestions > 0)
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.14f, 0.96f));
		ImGui::BeginChild("##CommandBarSuggest", ImVec2(0.f, suggestH), true, ImGuiWindowFlags_NoScrollbar);

		for (int i = 0; i < visibleSuggestions; ++i)
		{
			const bool selected = (i == m_SuggestionIndex);
			ImGui::PushID(i);
			if (ImGui::Selectable(m_Suggestions[i].c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
			{
				applySuggestion(m_Suggestions[i]);
				m_SuggestionIndex = -1;
				requestFocus = true;
			}
			if (selected)
				ImGui::SetItemDefaultFocus();
			ImGui::PopID();
		}

		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	// Prompt + input
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(">");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1.f);

	if (requestFocus)
		ImGui::SetKeyboardFocusHere();

	// Navigate suggestions with Up/Down before the InputText history callback eats them.
	const bool suggestionsActive = !m_Suggestions.empty() && m_InputBuffer.front() != '\0';
	if (suggestionsActive && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
	{
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
		{
			m_SuggestionIndex = (m_SuggestionIndex <= 0)
				? visibleSuggestions - 1
				: m_SuggestionIndex - 1;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
			m_SuggestionIndex = (m_SuggestionIndex + 1) % std::max(1, visibleSuggestions);
	}

	ImGuiInputTextFlags inputFlags =
		ImGuiInputTextFlags_EnterReturnsTrue
		| ImGuiInputTextFlags_CallbackCompletion
		| ImGuiInputTextFlags_CallbackEdit;
	// History arrows only when there is no live suggestion list to navigate.
	if (!suggestionsActive)
		inputFlags |= ImGuiInputTextFlags_CallbackHistory;

	bool submitted = ImGui::InputText(
		"##CommandBarInput",
		m_InputBuffer.data(),
		m_InputBuffer.size(),
		inputFlags,
		[](ImGuiInputTextCallbackData* data)
		{
			return static_cast<ImGui_Console*>(data->UserData)->textEditCallback(data);
		},
		this);

	if (submitted)
	{
		if (m_InputBuffer.front() != '\0')
		{
			// Enter with a selected suggestion that still matches prefix: prefer executing
			// the typed line (UE style). Tab already applied completion.
			this->execCommand(m_InputBuffer.data());
			m_InputBuffer.front() = 0;
			m_Suggestions.clear();
			m_SuggestionIndex = -1;
		}
		ImGui::SetKeyboardFocusHere(-1);
	}

	if (m_Options.font)
		ImGui::PopFont();

	ImGui::End();
}

static void printLines(ImGui_Console& console, std::string const& output)
{
	if (output.empty())
		return;

	size_t start = 0;
	for (size_t curr = 0; curr < output.size(); ++curr)
	{
		if ((output[curr] == '\r') || (output[curr] == '\n'))
		{
			console.Print(std::string_view(&output[start], curr - start));
			if (output[curr] == '\r' && curr + 1 < output.size() && output[curr + 1] == '\n')
				++curr;
			start = curr + 1;
		}
	}
	if (start < output.size())
		console.Print(std::string_view(&output[start], output.size() - start));
}

void ImGui_Console::execCommand(char const* cmdline)
{
	std::string_view const cmd = cmdline;
	if (!cmd.empty())
	{
		this->Print("> %s", cmd.data());

		auto result = m_Interpreter->execute(cmd);
		if (!result.output.empty())
			printLines(*this, result.output.c_str());
		else if (!result.status)
			this->Print("Command failed. Use 'help <name>' or 'help --list <regex>'.");

		m_History.push_back(cmd.data());
		m_HistoryIterator = m_History.rend();
	}
}


// XXXX mk: we should probably use the columns features instead ?
static void printColumns(ImGui_Console& console, std::vector<std::string> const& items)
{
	auto computeLineWidth = []() {
		// XXXX mk: this only works if the font is monospace !
		float width = ImGui::CalcItemWidth();
		ImVec2 charWidth = ImGui::CalcTextSize("A");
		return (size_t)(width / charWidth.x);
	};

	size_t max_len = 0;
	for (auto const& candidate : items)
		max_len = std::max(max_len, candidate.size());

	size_t line_width = computeLineWidth();
	size_t ncolumns = line_width / max_len;

	std::string line; int col = 1;
	for (auto const& candidate : items)
	{
		if ((col % ncolumns) != 0)
		{
			line += candidate;
			line += ' ';
			++col;
		}
		else
		{
			console.Print(line.c_str());
			line.clear();
			col = 1;
		}
	}
	if (!line.empty())
		console.Print(line.c_str());
}

static std::string extendKeyword(std::string_view keyword, std::vector<std::string> const& candidates)
{
	std::string match(keyword.data(), keyword.length());
	while (true)
	{
		int c = -1, cpos = (int)match.size();
		for (std::string_view const candidate : candidates)
		{
			if (cpos < candidate.size())
			{
				if (c == -1)
					c = candidate[cpos];
				else
					if (c != candidate[cpos])
						return match;
			}
			else
				return match;
		}
		match.push_back(c);
	}
}

int ImGui_Console::autoCompletionCallback(ImGuiInputTextCallbackData* data)
{
	// Prefer the highlighted live-suggestion entry (command bar).
	if (m_SuggestionIndex >= 0
		&& m_SuggestionIndex < static_cast<int>(m_Suggestions.size()))
	{
		std::string const& suggestion = m_Suggestions[static_cast<size_t>(m_SuggestionIndex)];
		std::string_view cmdline(data->Buf, data->BufTextLen);
		std::string_view keyword = isolateKeyword(cmdline);
		const int keywordBegin = static_cast<int>(keyword.data() - cmdline.data());
		const int keywordLen = static_cast<int>(keyword.size());
		data->DeleteChars(keywordBegin, keywordLen);
		data->InsertChars(keywordBegin, suggestion.c_str());
		data->InsertChars(data->CursorPos, " ");
		m_SuggestionIndex = -1;
		return 0;
	}

	std::string_view cmdline(data->Buf, data->CursorPos);
	std::string_view keyword = isolateKeyword(cmdline);

	if (auto candidates = m_Interpreter->suggest(data->Buf, data->CursorPos); !candidates.empty())
	{
		if (candidates.size() == 1)
		{
			std::string_view candidate = candidates.front();
			candidate.remove_prefix(keyword.size());
			data->InsertChars(data->CursorPos, candidate.data(), candidate.data() + candidate.size());
			data->InsertChars(data->CursorPos, " ");
		}
		else
		{
			// multiple candidates : append as many characters as possible to input (auto-complete)
			if (std::string match = extendKeyword(keyword, candidates); match.size() > keyword.size())
			{
				data->InsertChars(data->CursorPos, match.data() + keyword.size(), match.data() + match.size());
			}

			// print all candidates in columns (full Console panel)
			if (candidates.size() < 64)
				printColumns(*this, candidates);
			else
				Print("Too many matches (%d)", static_cast<int>(candidates.size()));
		}
	}
	return 0;
}

int ImGui_Console::historyKeyCallback(ImGuiInputTextCallbackData* data)
{
	HistoryBuffer::reverse_iterator currentPos = m_HistoryIterator;
	switch (data->EventKey)
	{
	case ImGuiKey_UpArrow: ++m_HistoryIterator; break;
	case ImGuiKey_DownArrow: --m_HistoryIterator; break;
	default: break;
	}
	if (currentPos != m_HistoryIterator)
	{
		std::string const& cmd = *m_HistoryIterator;
		snprintf(data->Buf, data->BufSize, "%s", cmd.c_str());
		data->BufTextLen = (int)cmd.length();
		data->BufDirty = true;
	}
	return 0;
}

int ImGui_Console::textEditCallback(ImGuiInputTextCallbackData* data)
{
	switch (data->EventFlag)
	{
	case ImGuiInputTextFlags_CallbackCompletion:
		return autoCompletionCallback(data);

	case ImGuiInputTextFlags_CallbackHistory:
		return historyKeyCallback(data);

	case ImGuiInputTextFlags_CallbackEdit:
		m_SuggestionIndex = -1;
		return 0;
	}
	return 0;
}
