#include <core/console/ConsoleInterpreter.h>
#include <core/console/ConsoleObjects.h>
#include <core/string_utils.h>
#include <core/log.h>

#include <cassert>
#include <mutex>

namespace caustica::console
{
	//
	// Lexer
	//

	class Lexer
	{
	public:

		Lexer(std::string_view stream);

		bool hasNextToken() { return !m_Eof; }

		Token nextToken();

		std::string const& getErrorString() const;

	private:

		void advance();

		void parseSpace();

		Token parseToken();

	private:

		enum class Error {
			NONE = 0,
			MISSING_QUOTE_ENDING,
			MISSING_ESCAPED_CHARACTER,
			UNEXPECTED_STRING_ENDING,
			READING_PAST_END,
		} m_Error = Error::NONE;

		char m_Next = 0;
		bool m_Eof = false;
		std::string_view m_Stream;
	};

	Lexer::Lexer(std::string_view stream) : m_Stream(stream)
	{
		if (!stream.empty())
			advance();
		parseSpace();
	}

	Token Lexer::nextToken()
	{
		if (!m_Eof)
			return parseToken();

		m_Error = Error::READING_PAST_END;
		return Token();
	}

	std::string const& Lexer::getErrorString() const
	{
		static std::string errs[] = {
			"unexpected lexer error",
			"unexpected end of stream after escape character",
			"missing closing quote",
			"characters after quote ending"
			"unexpected end of stream",
		};

		switch (m_Error)
		{
		case Error::MISSING_ESCAPED_CHARACTER: return errs[0];
		case Error::MISSING_QUOTE_ENDING: return errs[1];
		case Error::UNEXPECTED_STRING_ENDING: return errs[2];
		case Error::READING_PAST_END: return errs[3];
		default:
			return errs[0];
		}
	}

	void Lexer::advance()
	{
		if (!m_Stream.empty())
		{
			m_Next = m_Stream.front();
			m_Stream.remove_prefix(1);
		}	
		else
			m_Eof = true;
	}

	void Lexer::parseSpace()
	{
		while (!m_Eof && std::isspace(m_Next))
			advance();
	}

	Token Lexer::parseToken()
	{
		Token token;

		bool inString = true;
		bool inEscape = false;
		bool inQuotes = false;

		for ( ;!m_Eof && inString && !std::isspace(m_Next); advance())
		{
			if (inEscape)
			{
				token.value.push_back(m_Next);
				inEscape = false;
			}
			else
			{
				switch (m_Next)
				{
				case '\\': inEscape = true; break;
				case '\'':
				case '\"': inString = inQuotes = !inQuotes; break;
				default:
					token.value.push_back(m_Next);
				}
			}
		}

		if (!m_Eof && !std::isspace(m_Next))
			m_Error = Error::UNEXPECTED_STRING_ENDING;
		if (inEscape)
			m_Error = Error::MISSING_ESCAPED_CHARACTER;
		if (inQuotes)
			m_Error = Error::MISSING_QUOTE_ENDING;

		if (m_Error == Error::NONE)
		{
			token.type = TokenType::STRING;
			parseSpace();
			return token;
		}
		return Token();
	}

	//
	// Interpreter implementation
	//

	typedef Command::Args Args;

	static void initializeDefaultCommands();

	Interpreter::Interpreter()
	{
		initializeDefaultCommands();
	}
		
	Interpreter::Result Interpreter::execute(
		std::string_view const cmdline,
		VariableState::SetBy origin)
	{
		if (cmdline.empty())
			return { false };


		// Super-simple parser
		Command::Args args;
		for (Lexer lexer(cmdline); lexer.hasNextToken(); )
		{
			if (Token token = lexer.nextToken(); token.type != TokenType::INVALID)
				args.push_back(std::move(token.value));
			else
			{
				std::string err = "syntax error";
				err += args.empty() ? "" : " near token \"" + args.back() + '\"';
				err += " : " + lexer.getErrorString();
				caustica::error(err.c_str());
				return { false };
			}
		}

		if (args.empty())
			return {false};

		if (auto * cobj = findObject(args[0]))
		{
			if (auto * cmd = cobj->asCommand())
			{
				auto [status, output] = cmd->execute(args);
				return { status, output };
			}
			else if (auto * var = cobj->asVariable())
			{
				if (args.size() == 1)
				{
					return { true, var->getValueAsString() };
				}
				else
				{
					std::string value = args[1];
					for (size_t i = 2; i < args.size(); ++i)
					{
						value += ' ';
						value += args[i];
					}
					if (var->setValueFromString(value, origin))
						return { true, var->getName() + " = " + var->getValueAsString() };
					return { false, "failed to set " + var->getName() };
				}
			}
		}
		else
		{
			std::string const message =
				"no console object with name '" + std::string(args[0]) + "' found";
			caustica::error("%s", message.c_str());
			return { false, message };
		}

		return {false};
	}

	std::vector<std::string> Interpreter::suggest(std::string_view const cmdline, size_t cursor_pos)
	{
		if (cmdline.empty() || (cursor_pos > cmdline.size()))
			return {};

		auto tokens = ds::split(cmdline);

		if (!tokens.empty())
		{
			char const* token_start = tokens[0].data();
			char const* cursor = cmdline.data() + cursor_pos;
			char const* token_end = tokens[0].data() + tokens[0].size();			
			if ((tokens.size() == 1) && (token_start <= cursor) && (cursor <= token_end))
			{		
				// user is looking for a command
				auto names = matchObjectNames(("^" + std::string(token_start, cursor) + ".*").c_str());
				return {names.begin(), names.end()};
			}
			else
			{
				// user is looking for the command's arguments
				if (auto* cobj = findCommand(tokens[0]))
					return cobj->suggest(cmdline, cursor_pos);
				if (auto* variable = findVariable(tokens[0]))
					return variable->getValueSuggestions();
			}
		}
		return {};
	}

	// Register various commands

	static CommandDesc help_cmd = {
		// name
		"help",
		// description
		"usage: \n"
		"   help [name]\n"
		"       returns the description of console objects.\n"
		"   help --list [regex pattern]\n"
		"       returns a list of console objects matching the regex.\n",
		// on exec
		[](Command::Args const& args) -> Command::Result {
			if (args.size() >= 2)
			{
				if (args[1] == "--list")
				{
					Command::Result r;
					for (auto name : matchObjectNames(args.size() > 2 ? std::string(args[2]).c_str() : ".*"))
					{
						r.output += name;
						r.output += '\n';
					}
					r.status = true;
					return r;
				}
			else
			{
				if (auto cobj = findObject(args[1]))
				{
					std::string output = cobj->getDescription();
					if (Variable* variable = cobj->asVariable())
					{
						output += "\nCurrent: ";
						output += variable->getValueAsString();
						if (!variable->getDefaultValueAsString().empty())
						{
							output += "\nDefault: ";
							output += variable->getDefaultValueAsString();
						}
						if (!variable->getValueSuggestions().empty())
						{
							output += "\nValues: ";
							for (size_t i = 0; i < variable->getValueSuggestions().size(); ++i)
							{
								if (i > 0)
									output += ", ";
								output += variable->getValueSuggestions()[i];
							}
						}
					}
					return { true, output };
				}
				else
					return { false, std::string("no console object with name '") + std::string(args[1]) + "' found" };
			}
		}
			else
				return { true, help_cmd.description };
		},
		// on suggest
		[](std::string_view cmdline, size_t cursor_pos) -> std::vector<std::string> {

			auto tokens = ds::split(cmdline);

			assert(tokens[0] == "help");

			char const* cursor = cmdline.data() + cursor_pos;

			for (auto& token : tokens)
			{
				if ((token.data() <= cursor) && (cursor <= (token.data() + token.size())))
				{
					auto names = matchObjectNames(("^" + std::string(token.data(), cursor) + ".*").c_str());
					return {names.begin(), names.end()};
			}
			}
			return {};
		}
	};

	static CommandDesc cvar_list_cmd = {
		"cvar.list",
		"cvar.list [regex]\nLists console variables, current values, defaults and sources.",
		[](Command::Args const& args) -> Command::Result {
			return { true, dumpVariables(args.size() > 1 ? args[1].c_str() : ".*") };
		},
		{}
	};

	static CommandDesc cvar_reset_cmd = {
		"cvar.reset",
		"cvar.reset [regex]\nResets matching console variables to their registered defaults.",
		[](Command::Args const& args) -> Command::Result {
			size_t const count = resetVariables(args.size() > 1 ? args[1].c_str() : ".*");
			return { true, "reset " + std::to_string(count) + " variable(s)" };
		},
		{}
	};

	static CommandDesc cvar_export_cmd = {
		"cvar.export",
		"cvar.export [regex]\nPrints ARCHIVE console variables as name=value configuration lines.",
		[](Command::Args const& args) -> Command::Result {
			return { true, exportVariables(args.size() > 1 ? args[1].c_str() : ".*") };
		},
		{}
	};

	static void initializeDefaultCommands()
	{
		static std::once_flag initialized;
		std::call_once(initialized, []()
		{
			for (auto const& cmd : { help_cmd, cvar_list_cmd, cvar_reset_cmd, cvar_export_cmd })
				registerCommand(cmd);
		});
	}

}
