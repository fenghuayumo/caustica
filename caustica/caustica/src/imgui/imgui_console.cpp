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


static ImVec4 getSeverityColor(caustica::Severity severity)
{
	using namespace caustica;	
	switch (severity)
	{
	case Severity::Info: return ImVec4(.6f, .8f, 1.f, 1.f);
	case Severity::Warning: return ImVec4(1.f, .5f, 0.f, 1.f);
	case Severity::Error: return ImVec4(1.f, 0.f, 0.f, 1.f);
	default:
		break;
	}
	return ImVec4(1.f, 1.f, 1.f, 1.f);
}

namespace
{
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
	m_ItemsLog.push_back(item);
}

void ImGui_Console::Print(std::string_view line)
{
	LogItem item;
	item.text = line;
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
	ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
	if (requestFocus)
	{
		ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
		ImGui::SetNextWindowFocus();
	}

	if (!ImGui::Begin("Console", open, ImGuiWindowFlags_MenuBar))
	{
		ImGui::End();
		return;
	}

	if (ImGui::BeginPopupContextItem())
	{
		if (ImGui::MenuItem("Close Console"))
			*open = false;
		ImGui::EndPopup();
	}

	if (ImGui::BeginMenuBar())
	{
		if (ImGui::BeginMenu("Edit"))
		{
			bool clearLog = ImGui::MenuItem("clear Log");
			bool clearHistory = ImGui::MenuItem("clear History");
			bool clearAll = ImGui::MenuItem("clear All");

			if (clearLog || clearAll)
				this->clearLog();
			if (clearHistory || clearAll)
				this->clearHistory();
			ImGui::EndMenu();
		}
		ImGui::EndMenuBar();			
	}

	// Log area

	const float footer_height = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
	ImGui::BeginChild("Log panel", ImVec2(0, -footer_height), false, ImGuiWindowFlags_HorizontalScrollbar);

	// right click popup on log panel
	if (ImGui::BeginPopupContextWindow()) 
	{
		if (ImGui::Selectable("clear"))
			clearLog();
		ImGui::EndPopup();
	}

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1)); // Tighten spacing

	if (m_Options.font)
		ImGui::PushFont(m_Options.font->getScaledFont());
	for (auto const& item : m_ItemsLog)
	{
		using namespace caustica;

		bool showItem = true;
		switch (item.severity)
		{
		case Severity::Info: showItem = m_Options.show_info; break;
		case Severity::Warning: showItem = m_Options.show_warnings; break;
		case Severity::Error: showItem = m_Options.show_errors; break;
		default: break;
		}

		if (showItem)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, item.textColor);
			ImGui::TextUnformatted(item.text.c_str());
			ImGui::PopStyleColor();
		}
	}

	if (m_Options.scroll_to_bottom || (m_Options.auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
	{
		ImGui::SetScrollHereY(1.f);
	}

	if (m_Options.font)
		ImGui::PopFont();

	m_Options.scroll_to_bottom = false;
	ImGui::PopStyleVar();
	ImGui::EndChild(); // end log scroll area

	ImGui::Separator();

	// Command line (also available in the log panel for convenience)
	if (m_Options.font)
		ImGui::PushFont(m_Options.font->getScaledFont());

	bool reclaim_focus = requestFocus;
	auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
	if (requestFocus)
		ImGui::SetKeyboardFocusHere();
	if (ImGui::InputText("##Command", m_InputBuffer.data(), m_InputBuffer.size(), flags, 
		[](ImGuiInputTextCallbackData* data)
		{
			ImGui_Console* console = (ImGui_Console*)data->UserData;
			return console->textEditCallback(data);
		}, (void*)this))
	{
		if (m_InputBuffer.front() != '\0')
		{
			this->execCommand(m_InputBuffer.data());
			m_InputBuffer.front() = 0;
		}
		reclaim_focus = true;
	}

	if (m_Options.font)
		ImGui::PopFont();

	// Auto-focus on window apparition / after submitting a command
	ImGui::SetItemDefaultFocus();
	if (reclaim_focus)
		ImGui::SetKeyboardFocusHere(-1); // Auto focus previous widget

	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::Text("Filters : "); ImGui::SameLine();
	ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
	auto filterButton = [](char const* label, bool* value, caustica::Severity severity) {
		ImGui::PushStyleColor(ImGuiCol_Border, getSeverityColor(severity));
		ImGui::Checkbox(label, value);
		ImGui::PopStyleColor();
	};
	filterButton("Errors", &m_Options.show_errors, caustica::Severity::Error); ImGui::SameLine();
	filterButton("Warnings", &m_Options.show_warnings, caustica::Severity::Warning); ImGui::SameLine();
	filterButton("Info", &m_Options.show_info, caustica::Severity::Info);
	ImGui::PopStyleVar(); // FrameBorder

	ImGui::End();
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

void ImGui_Console::renderCommandBar(bool* open, bool requestFocus)
{
	if (!open || !*open)
		return;

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	if (!vp)
		return;

	constexpr float kBarMargin = 10.f;
	constexpr float kBarHeight = 36.f;
	constexpr int kMaxSuggestions = 12;
	constexpr int kRecentLogLines = 8;

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

	const float totalH = kBarHeight + suggestH + logH + 16.f;
	const ImVec2 barPos(vp->WorkPos.x + kBarMargin, vp->WorkPos.y + vp->WorkSize.y - totalH - kBarMargin);
	const ImVec2 barSize(vp->WorkSize.x - kBarMargin * 2.f, totalH);

	ImGui::SetNextWindowPos(barPos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(barSize, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.94f);
	ImGui::SetNextWindowViewport(vp->ID);

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
