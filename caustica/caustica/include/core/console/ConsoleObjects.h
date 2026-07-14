#pragma once

#include <math/math.h>

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <string.h>

namespace caustica
{

	namespace console {

		class Command;
		class Variable;
		template <typename T> class AutoVariable;

		//
		// Base Console Object
		//

		class Object
		{
		public:

			virtual ~Object() {}

			std::string const& getName() const;

			std::string const& getDescription() const { return m_Description; }
			void setDescription(std::string const& description) { m_Description = description; }

			virtual Variable* asVariable() { return nullptr; }
			virtual Command* asCommand() { return nullptr; }

		protected:
			friend class ObjectDictionary;
			
			Object(char const* description) : m_Description(description) { }

			std::string m_Description;
		};

		//
		// Console Commands
		//

		class Command : public Object
		{
		public:

			virtual Command* asCommand() override { return this; }

			// execution callback

			struct Result
			{
				bool status = false;
				std::string output;
			};

			typedef std::vector<std::string> Args;
			typedef std::function<Result(Args const& args)> OnExecuteFunction;

			Result execute(Args const& args);

			// optional callback to suggest argument values

			typedef std::function<std::vector<std::string>(std::string_view const cmdline, size_t cursor_pos)> OnSuggestFunction;

			std::vector<std::string> suggest(std::string_view const cmdline, size_t cursor_pos);

		private:

			friend class ObjectDictionary;

			Command(char const* description, OnExecuteFunction on_exec, OnSuggestFunction on_suggest);

			OnExecuteFunction m_OnExecute;
			OnSuggestFunction m_OnSuggest;
		};

		//
		// Console Variables
		// 
		// Console variables are unique typed data elements associated to a given name.
		// Two common usage patterns:
		//
		//   - Static mode: the type of the variable is known and template traits
		//     specialization can be used to automatically access the data. This mode
		//     is implemented with the "AutoVariable" class. AutoVariables can be instanciated
		//     directly in code, typically as global/static variables. They are strong-typed,
		//     lightweight, incur negligible performance penalty, and can be freely copied.
		//
		//     ex:
		//        cvarInt var = AutoVariable("myvar", "my variable description", 123);
		//        int i = var.getValue();
		//     
		//     Convenience conversion operators are defined so that variables can be 
		//     used as if they were values:
		//        cvarInt var = ...;
		//        int i = var;
		//        var = 5;
		//
		//   - Dynamic mode: the type of the variable is not know to the code, so
		//     type-casting is implemented as an interface of virtual functions. The
		//     typical use case is the implementation of a console interpreter and any
		//     other run-time or user-driven access. This mode is implemented with the
		//     "Variable" class.
		//
		//     ex:
		//         if (cvar* var = findVariable("myvar"))
		//             if (var->isInt())
		//                 int i = var->getInt();
		//     note: some accessors have specializations to cast between types
		//     (ex. getString() on a bool typed variable returns "true" or "false" strings)
		//
		
		struct VariableType
		{
			enum Type : uint8_t {
				TYPE_UNKNOWN = 0,
				TYPE_BOOL,
				TYPE_INT,
				TYPE_INT2,
				TYPE_INT3,
				TYPE_FLOAT,
				TYPE_FLOAT2,
				TYPE_FLOAT3,
				TYPE_FLOAT4,
				TYPE_STRING
			};
			template <typename T> static Type isA() { return TYPE_UNKNOWN; }
		};

		struct VariableState
		{
			// Tracks where the origin of the most recent change to the
			// value of a console variable.
			enum SetBy : uint8_t
			{
				UNSET = 0,
				CODE,
				INI,
				CONSOLE
			};

			// XXXX mk: C++ 20 has in-line initialization for bit-fields...
			VariableState() { memset(this, 0, sizeof(VariableState)); } 
			VariableState(VariableType::Type type, SetBy setby) : read_only(false), cheat(false), type(type), setby(setby) {}
			VariableState(bool ronly, bool cheat, VariableType::Type type, SetBy setby) : read_only(ronly), cheat(cheat), type(type), setby(setby) {}

			bool operator == (VariableState const& other) const { return *((uint32_t const*)this) == *((uint32_t const*)(&other)); }
			bool operator != (VariableState const& other) const { return !(*this==other); }

			bool isInitalized() const;

			// Returns true if the setter is allowed to modify the value.
			// note: if the 'cheat' state is true, the variable can be initialized from 'CODE',
			// but it cannot be modified from either the 'CONSOLE' or 'INI' sources
			bool canSetValue(SetBy origin = CONSOLE) const;

			uint32_t read_only : 1;
			uint32_t cheat     : 1;
			uint32_t type      : 5;
			uint32_t setby     : 2;
		};

		class Variable : public Object
		{
		public:

			// state

			typedef VariableState::SetBy SetBy;

			VariableState getState() const { return m_State; }

			void setReadOnly(bool ronly) { m_State.read_only = ronly; }

			void setCheat() { m_State.cheat = true; }	

		public:

			// Value accessors for each type. Ex.
			//     bool isInt() const;
			//     int getInt() const;
			//     void setInt(int value, SetBy setby=SETBY_CODE);

#define DEFINE_TYPED_ACCESSORS(name, type) \
	virtual bool is##name() const = 0;   \
	virtual type get##name() const = 0;  \
	virtual void set##name(type value, SetBy setby=SetBy::CODE) = 0;

#define DEFINE_TYPED_REF_ACCESSORS(name, type)   \
	virtual bool is##name() const = 0;         \
	virtual type const& get##name() const = 0; \
	virtual void set##name(type const& value, SetBy setby=SetBy::CODE) = 0;

			DEFINE_TYPED_ACCESSORS(Bool, bool);

			DEFINE_TYPED_ACCESSORS(Int, int);
			DEFINE_TYPED_ACCESSORS(Int2, dm::int2);
			DEFINE_TYPED_ACCESSORS(Int3, dm::int3);
			
			DEFINE_TYPED_ACCESSORS(Float, float);
			DEFINE_TYPED_ACCESSORS(Float2, dm::float2);
			DEFINE_TYPED_ACCESSORS(Float3, dm::float3);
			DEFINE_TYPED_ACCESSORS(Float4, dm::float4);

			DEFINE_TYPED_REF_ACCESSORS(String, std::string);

			// attenmpt to parse value from string
			virtual bool setValueFromString(std::string_view s, SetBy setby = SetBy::CODE) = 0;
			virtual bool setValueFromString(std::string const& s, SetBy setby = SetBy::CODE) = 0;

			virtual std::string getValueAsString() const = 0;

		public:

			// callback

			typedef std::function<void(Variable & cvar)> Callback;
			
			void setOnChangeCallback(Callback onChange);

			void executeOnChangeCallback();

		protected:

			friend class ObjectDictionary;

			Variable(char const* description, VariableState state) : Object(description), m_State(state) {}

			Callback m_OnChange;
			VariableState m_State;
		};

		template <typename TVar> class VariableImpl;

		template <typename T> class AutoVariable
		{
		public:
			
			// note: registering a console object with null or empty name string will result in fatal error
			AutoVariable(char const* name, char const* description, T const& defaultValue, bool read_only=false, bool cheat=false);

			std::string const& getName() const;

			void setDescription(std::string const& description);

			std::string const& getDescription() const;

			VariableState getState() const;

			T getValue() const;

			void setValue(T const& value);

			void setOnChangeCallback(Variable::Callback onChange);

			void executeOnChangeCallback();

			Variable* operator & ();

			operator T() const;

			AutoVariable<T>& operator =(const T& value);

		private:
			friend class VariableImpl<T>;
			
			VariableImpl<T>& m_Variable;
		};

		//
		// Object functions
		//

		struct CommandDesc
		{
			char const* name = nullptr;
			char const* description = nullptr;
			Command::OnExecuteFunction on_execute;
			Command::OnSuggestFunction on_suggest;
		};

		bool registerCommand(CommandDesc const& desc);

		Object* findObject(std::string_view name);

		std::vector<std::string_view> matchObjectNames(char const* regex = ".*");

		std::vector<Object*> matchObjects(char const* regex = ".*");

		Command* findCommand(std::string_view name);

		Variable* findVariable(std::string_view name);
		
		// note: ini files can only modify values of existing consolve variables
		void parseIniFile(char const* inidata, char const* filename);

		// nuclear option: removes all console objects from dictionary
		void resetAll(); 

	} // end namespace console

	typedef console::Variable cvar;

	typedef console::AutoVariable<bool> cvarBool;
	typedef console::AutoVariable<int> cvarInt;
	typedef console::AutoVariable<float> cvarFloat;
	typedef console::AutoVariable<dm::int2> cvarInt2;
	typedef console::AutoVariable<dm::int3> cvarInt3;
	typedef console::AutoVariable<dm::uint2> cvarUint2;
	typedef console::AutoVariable<dm::uint3> cvarUint3;
	typedef console::AutoVariable<dm::float2> cvarFloat2;
	typedef console::AutoVariable<dm::float3> cvarFloat3;
	typedef console::AutoVariable<dm::float4> cvarFloat4;
	typedef console::AutoVariable<std::string> cvarString;

} // end namespace caustica