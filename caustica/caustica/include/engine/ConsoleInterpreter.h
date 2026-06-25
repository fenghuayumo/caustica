#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace caustica
{
	class TextureLoader;

	namespace console
	{
		// Commnad-line lexer (splits a command line into a vector of tokens)
		//
		// Valid tokens:
		//   - identifiers: none
		//   - keywords   : none
		//   - separator  : none
		//   - operator   : none
		//   - literals   : strings
		//
		// Lexical grammar:
		//
		//   - strings: valid strings are sequences of characters separated by
		//     white-space characters. Single quotes (') and double quotes (")
		//     can be used as delimiters. Backslash (\) can be used to escape
		//     quotes and space.
		//  

		enum class TokenType
		{
			INVALID = 0,
			STRING
		};

		struct Token
		{
			TokenType type = TokenType::INVALID;
			std::string value;
		};

		class Lexer;

		// Command-line interpreter

		class Interpreter
		{
		public:

			Interpreter();

			struct Result
			{
				bool status = false;
				std::string output;
			};

			Result Execute(std::string_view const cmdline);

			// parse incomplete command line & return auto-completion suggestions
			std::vector<std::string> Suggest(std::string_view const cmdline, size_t cursor_pos);

			bool RegisterCommands(std::shared_ptr<TextureLoader> textureCache);

		private:

			std::shared_ptr<TextureLoader> m_TextureLoader;
		};

	} // end namespace console

} // end namespace caustica
