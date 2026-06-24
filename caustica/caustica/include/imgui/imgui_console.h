#pragma once

#include <core/circular_buffer.h>
#include <core/log.h>

#include <imgui.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace caustica::console
{
	class Interpreter;
}

namespace caustica
{
	class RegisteredFont;

	class ImGui_Console 
	{
	public:

		struct Options
		{
			
			std::shared_ptr<RegisteredFont> font;        // it is recommended to specify a monospace font

			bool auto_scroll = true;       // automatically keep log output scrolled to the most recent item
			bool scroll_to_bottom = false; // scoll to botom on console creation, if the log is not empty

			bool capture_log = true;       // captures engine event logs & redirects to the console
			bool show_info = false;        // default state of log events filters
			bool show_warnings = true;
			bool show_errors = true;
		};

		ImGui_Console(std::shared_ptr<caustica::console::Interpreter> interpreter, Options const& opts);

		~ImGui_Console();

		void Print(char const* fmt, ...);

		void Print(std::string_view line);

		void ClearLog();

		void ClearHistory();

		void Render(bool * open=nullptr);

	private:

		int HistoryKeyCallback(ImGuiInputTextCallbackData* data);

		int AutoCompletionCallback(ImGuiInputTextCallbackData* data);

		int TextEditCallback(ImGuiInputTextCallbackData* data);

		void ExecCommand(char const* cmd);

	private:

		typedef std::array<char, 256> InputBuffer;
		InputBuffer m_InputBuffer = { 0 };

		typedef caustica::core::circular_buffer<std::string, 1024> HistoryBuffer;
		HistoryBuffer m_History;
		HistoryBuffer::reverse_iterator m_HistoryIterator = m_History.rend();

		struct LogItem
		{
			caustica::Severity severity = caustica::Severity::None;
			ImVec4 textColor = ImVec4(1.f, 1.f, 1.f, 1.f);
			std::string text;
		};

		typedef caustica::core::circular_buffer<LogItem, 5000> ItemsLog;
		ItemsLog m_ItemsLog;

	private:

		Options m_Options;

		std::shared_ptr<caustica::console::Interpreter> m_Interpreter;
	};

} // namespace caustica
