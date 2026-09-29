#include <core/console/ConsoleObjects.h>
#include <core/log.h>
#include <core/StringUtils.h>

#include <cassert>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>

using namespace caustica::math;

namespace caustica::console
{

	static std::string const emptyString;

	// helper : compile regex safely & catch user errors
	inline std::optional<std::regex> regex_from_char(char const* s)
	{
		std::regex rx;
		if (s)
		{
			try { rx = s; }
			catch (std::regex_error const& err)
			{
				caustica::error(err.what());
				return std::nullopt;
			}
		}
		return rx;
	}

	// Variable types conversions
	template <> inline VariableType::Type VariableType::isA<bool>() { return TYPE_BOOL; }
	template <> inline VariableType::Type VariableType::isA<int>() { return TYPE_INT; }
	template <> inline VariableType::Type VariableType::isA<float>() { return TYPE_FLOAT; }
	template <> inline VariableType::Type VariableType::isA<math::int2>() { return TYPE_INT2; }
	template <> inline VariableType::Type VariableType::isA<math::int3>() { return TYPE_INT3; }
	template <> inline VariableType::Type VariableType::isA<math::float2>() { return TYPE_FLOAT2; }
	template <> inline VariableType::Type VariableType::isA<math::float3>() { return TYPE_FLOAT3; }
	template <> inline VariableType::Type VariableType::isA<math::float4>() { return TYPE_FLOAT4; }
	template <> inline VariableType::Type VariableType::isA<std::string>() { return TYPE_STRING; }

	static char const* asString(VariableType::Type type)
	{
		switch (type)
		{
		case VariableType::TYPE_BOOL: return "bool";
		case VariableType::TYPE_INT: return "int";
		case VariableType::TYPE_INT2: return "int2";
		case VariableType::TYPE_INT3: return "int3";
		case VariableType::TYPE_FLOAT: return "float";
		case VariableType::TYPE_FLOAT2: return "float2";
		case VariableType::TYPE_FLOAT3: return "float3";
		case VariableType::TYPE_FLOAT4: return "float4";
		case VariableType::TYPE_STRING: return "string";
		default:
			return "unknown";
		}
	}

	static char const* asString(VariableState::SetBy setby)
	{
		switch (setby)
		{
		case VariableState::CODE: return "CODE";
		case VariableState::CONSOLE: return "CONSOLE";
		case VariableState::INI: return "INI";
		case VariableState::COMMAND_LINE: return "COMMAND_LINE";
		default:
			return setby == VariableState::CONSOLE ? "CONSOLE" : "UNSET";
		}
	}

	bool VariableState::isInitalized() const
	{
		return type != VariableType::TYPE_UNKNOWN && setby != UNSET;
	}

	bool VariableState::canSetValue(SetBy origin) const
	{
		if (!isInitalized() || read_only)
			return false;
		if (cheat && (setby <= CODE) && (origin > CODE))
			return false;
		if (origin < setby)
			return false;
		return true;
	}

	static std::atomic_bool g_StartupVariablesLocked = false;

	//
	// Console Object Dictionary
	//

	class ObjectDictionary
	{
	public:
		typedef VariableState::SetBy SetBy;

		static inline bool IsValidName(char const* name)
		{
			return (name && (std::strlen(name) > 0)) ? true : false;
		}

		Object* registerCommand(CommandDesc const& desc)
		{
			if (IsValidName(desc.name))
			{
				if (!desc.on_execute)
				{
					caustica::fatal("attempting to register console command '%s' with no execution function", desc.name);
				}

				std::lock_guard<std::mutex> lock(m_mutex);
				if (auto it = m_dictionary.find(desc.name); it == m_dictionary.end())
				{
					auto* cmd = new Command(desc.description, desc.on_execute, desc.on_suggest);
					m_dictionary[desc.name] = cmd;
					return cmd;
				}
				else
					caustica::error("console command with name '%s' already exists", desc.name);
			}
			else
				caustica::error("attempting to register a console command with invalid name '%s'", desc.name);
			return nullptr;
		}

		bool unregisterCommand(std::string_view name)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (auto it = m_dictionary.find(name); it != m_dictionary.end())
			{
				if (it->second->asCommand())
				{
					m_dictionary.erase(it);
					return true;
				}
				else
					caustica::error("unregister command '%s'; object is not a console command");
			}
			else
				caustica::error("unregister command '%s'; command does not exist", name);
			return false;
		}

		template <typename T> Object* RegisterVariable(char const* name, char const* description, T const& value, VariableState state)
		{
			// Static registration (from code) cannot return nullptr and therefore has to trigger a fatal errors.
			// Dynamic registration (from .ini or console) reports errors, but can return nullptr
			auto handleError = [&](char const* message) -> void {
				if (state.setby > SetBy::CODE)
					caustica::error(message, name);
				else
					caustica::fatal(message, name);
			};

			if (IsValidName(name))
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (auto it = m_dictionary.find(name); it != m_dictionary.end())
				{
					if (VariableImpl<T>* cvar = (VariableImpl<T>*)it->second->asVariable())
					{
						if (cvar->getState().type == VariableType::isA<T>())
						{
							// cvar may have been referenced elsewhere but not be initialized yet
							if (cvar->m_description.empty() && IsValidName(description))
								cvar->m_description = description;

							// override the value
							cvar->setData(value, (SetBy)state.setby);

							return cvar;
						}
						else
							handleError("console variable '%s' already exists but is a different type");
					}
					else
						handleError("console variable with name '%s' already exists");
				}
				else
				{
					assert(state.type == VariableType::isA<T>());
					state.type = VariableType::isA<T>(); // force type to be correct

					VariableImpl<T>* cvar = new VariableImpl<T>(value, description, state);
					m_dictionary[name] = cvar;
					return cvar;
				}
			}
			else
				handleError("attempting to register a console variable with invalid name '%s'");
			return nullptr;
		}

		template <typename T> Object* RegisterBoundVariable(BoundVariableDesc<T> const& desc)
		{
			if (!IsValidName(desc.name))
			{
				caustica::error("attempting to register a bound console variable with invalid name");
				return nullptr;
			}

			std::lock_guard<std::mutex> lock(m_mutex);
			if (auto it = m_dictionary.find(desc.name); it != m_dictionary.end())
			{
				auto* variable = dynamic_cast<VariableImpl<T>*>(it->second->asVariable());
				if (!variable)
				{
					caustica::error("console object '%s' already exists with a different type", desc.name);
					return nullptr;
				}
				variable->configureBinding(desc);
				return variable;
			}

			VariableState state(
				desc.readOnly || !desc.setter,
				desc.cheat,
				VariableType::isA<T>(),
				VariableState::CODE);
			auto* variable = new VariableImpl<T>(desc.defaultValue, desc.description, state);
			variable->configureBinding(desc);
			m_dictionary[desc.name] = variable;
			return variable;
		}

		Object* findObject(std::string_view name)
		{
			if (!name.empty())
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				auto it = m_dictionary.find(name);
				if (it != m_dictionary.end())
					return it->second;
			}
			return nullptr;
		}

		std::vector<std::string_view> FindObjectNames(char const* regex)
		{
			std::vector<std::string_view> matches;
			if (auto rx = regex_from_char(regex))
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				for (auto& it : m_dictionary)
					if (std::regex_match(it.first, *rx))
						matches.push_back(std::string_view(it.first));
			}
			return matches;
		}

		std::vector<Object*> FindObjects(char const* regex)
		{
			std::vector<Object*> matches;
			if (auto rx = regex_from_char(regex))
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				for (auto& it : m_dictionary)
					if (std::regex_match(it.first, *rx))
						matches.push_back(it.second);
			}
			return matches;
		}

		std::string const& GetObjectName(Object const* cobj)
		{
			// slow linear search under the assumption that this is only called very rarely
			if (cobj)
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				for (auto const& it : m_dictionary)
				{
					if (it.second == cobj)
						return it.first;
				}
				caustica::error("unregistered object");
			}
			return emptyString;
		}

		void reset()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_dictionary.clear();
		}

	private:

		// note: the dictionary deliberately leaks its ConsoleObjects* in order to
		// guarantee that any reference to the memory will still be valid when the
		// application shuts down and implicit destructors are invoked. The "correct"
		// implementation would to own lifespan with shared/weak_ptr, but this adds
		// a lot of atomic & error checking burdens which were not deemed to be worth it.
		std::mutex m_mutex;
		std::map<std::string, Object*, std::less<>> m_dictionary;

	} objectsDictionary;

	//
	// Implementation
	//


	//
	// Console Object
	//

	std::string const& Object::getName() const
	{
		return objectsDictionary.GetObjectName(this);
	}

	//
	// Console Command
	//

	Command::Command(char const* description, OnExecuteFunction onexec, OnSuggestFunction onsuggest)
		: Object(description), m_onExecute(onexec), m_onSuggest(onsuggest)
	{ }

	Command::Result Command::execute(Args const& args)
	{
		if (m_onExecute)
			return m_onExecute(args);
		else
			caustica::error("console command '%s' has no function", this->getName().c_str());
		return Result();
	}

	std::vector<std::string> Command::suggest(std::string_view cmdline, size_t cursor_pos)
	{
		if (m_onSuggest)
			return m_onSuggest(cmdline, cursor_pos);
		else
			return {};
	}

	//
	// Console Variable
	//

	void Variable::setOnChangeCallback(Callback onChange)
	{
		m_onChange = onChange;
	}

	void Variable::executeOnChangeCallback()
	{
		if (m_onChange)
			m_onChange(*this);
		else
			caustica::error("no callback set for CVar '%s'", this->getName().c_str());
	}

	//
	// Console Variable Implementation
	//

    template <typename T> class AutoVariable;

	template <typename T>
	bool valuesEqual(T const& a, T const& b)
	{
		if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, std::string>)
		{
			return a == b;
		}
		else
		{
			for (int i = 0; i < T::DIM; ++i)
				if (a[i] != b[i])
					return false;
			return true;
		}
	}

	template <typename T> class VariableImpl : public Variable
	{
	public:

		VariableImpl(T const& data, char const* description, VariableState state)
			: Variable(description ? description : "", state), m_data(data), m_defaultData(data) { }

		virtual Variable* asVariable() override { return this; }

		inline T getData() const
		{
			if (m_getter)
				m_data = m_getter();
			return m_data;
		}

		inline T const& getDataRef() const
		{
			if (m_getter)
				m_data = m_getter();
			return m_data;
		}

		void configureBinding(BoundVariableDesc<T> const& desc)
		{
			m_defaultData = desc.defaultValue;
			m_data = desc.defaultValue;
			m_getter = {};
			m_setter = desc.setter;
			m_validator = desc.validator;
			m_choices = desc.choices;
			m_flags = desc.flags;
			m_description = desc.description ? desc.description : "";
			m_state.read_only = desc.readOnly || !desc.setter;
			m_state.cheat = desc.cheat;
			m_valueSuggestions.clear();
			for (auto const& choice : m_choices)
				m_valueSuggestions.push_back(choice.first);
			m_defaultValue = getValueAsString();
			m_getter = desc.getter;
			m_onChange = desc.onChanged
				? [callback = desc.onChanged](Variable&) { callback(); }
				: Callback{};
		}

		inline bool setData(T const& value, SetBy setby)
		{
			VariableState flags = this->getState();
			if (hasFlag(m_flags, VariableFlags::STARTUP_ONLY)
				&& g_StartupVariablesLocked.load(std::memory_order_acquire)
				&& setby > SetBy::CODE)
			{
				caustica::error("cvar '%s' is startup-only - value not set", this->getName().c_str());
				return false;
			}
			if (flags.canSetValue(setby))
			{
				T const previous = getData();
				T validated = value;
				if (m_validator)
				{
					std::string error;
					if (!m_validator(validated, error))
					{
						caustica::error("cvar '%s' rejected value: %s",
							this->getName().c_str(), error.c_str());
						return false;
					}
				}

				if (m_setter)
					m_setter(validated);
				m_data = validated;
				this->m_state.setby = setby;

				if (m_onChange && !valuesEqual(previous, validated))
					m_onChange(*this);
				return true;
			}
			else
			{
				VariableState state = this->getState();
				if (state.read_only)
					caustica::error("cvar '%s' is read-only - value not set", this->getName().c_str());
				else
					caustica::error("cvar '%s' not enough privilege with '%s' to override '%s' - value not set",
						this->getName().c_str(), asString(setby), asString((SetBy)state.setby));
			}
			return false;
		}

		virtual bool setValueFromString(std::string_view s, SetBy setby) override
		{
			if (!s.empty())
			{
				for (auto const& choice : m_choices)
				{
					if (ds::caseInsensitiveEquals(std::string(s), choice.first))
						return this->setData(choice.second, setby);
				}
				if (auto value = ds::parse<T>(s))
					return this->setData(*value, setby);
			}

			caustica::error("cvar '%s' failed parsing value string '%s' (expected a %s) - value not set",
				this->getName().c_str(), std::string(s).c_str(), asString((VariableType::Type)m_state.type));
			return false;
		}

		virtual bool setValueFromString(std::string const& s, SetBy setby) override
		{
			return setValueFromString(std::string_view(s), setby);
		}

		virtual std::string getValueAsString() const override
		{
			T const value = m_getter ? m_getter() : m_data;
			for (auto const& choice : m_choices)
				if (valuesEqual(choice.second, value))
					return choice.first;
			char buff[16] = { 0 };
			if (auto [p, ec] = std::to_chars(buff, buff+16, value); ec == std::errc())
				return std::string(buff);
			return std::string();
		}

		virtual bool resetToDefault(SetBy setby) override
		{
			return setData(m_defaultData, setby);
		}

		// default accessors

		#define DEFINE_TYPED_ACCESSORS_IMPLEMENTATION(name, type) \
			virtual bool is##name() const override { return false; } \
			virtual type get##name() const override { \
				caustica::error("cvar '%s' is not a "#type" (cannot get)", this->getName().c_str()); \
				return type(); \
			} \
			virtual void set##name(type value, SetBy) override { \
				caustica::error("cvar '%s' is not a "#type" (cannot set)", this->getName().c_str()); \
			}

		DEFINE_TYPED_ACCESSORS_IMPLEMENTATION(Bool, bool);
		DEFINE_TYPED_ACCESSORS_IMPLEMENTATION(Int, int);
		DEFINE_TYPED_ACCESSORS_IMPLEMENTATION(Int2, int2);
		DEFINE_TYPED_ACCESSORS_IMPLEMENTATION(Int3, int3);
		DEFINE_TYPED_ACCESSORS_IMPLEMENTATION(Float, float);
		DEFINE_TYPED_ACCESSORS_IMPLEMENTATION(Float2, float2);
		DEFINE_TYPED_ACCESSORS_IMPLEMENTATION(Float3, float3);
		DEFINE_TYPED_ACCESSORS_IMPLEMENTATION(Float4, float4);


		#define DEFINE_TYPED_REF_ACCESSORS_IMPLEMENTATION(name, type) \
			virtual bool is##name() const override { return false; } \
			virtual type const& get##name() const override { \
				caustica::error("cvar '%s' is not a "#type" (cannot get)", this->getName().c_str()); \
				return emptyString; \
			} \
			virtual void set##name(type const& value, SetBy) override { \
				caustica::error("cvar '%s' is not a "#type" (cannot set)", this->getName().c_str()); \
			}

		DEFINE_TYPED_REF_ACCESSORS_IMPLEMENTATION(String, std::string);

	private:
		friend class AutoVariable<T>;

		mutable T m_data;
		T m_defaultData{};
		std::function<T()> m_getter;
		std::function<void(T const&)> m_setter;
		std::function<bool(T&, std::string&)> m_validator;
		std::vector<std::pair<std::string, T>> m_choices;
	};

	// specialisations

	template<> bool VariableImpl<bool>::isBool() const { return true; }
	template<> bool VariableImpl<int>::isInt() const { return true; }
	template<> bool VariableImpl<int2>::isInt() const { return true; }
	template<> bool VariableImpl<int3>::isInt() const { return true; }
	template<> bool VariableImpl<float>::isFloat() const { return true; }
	template<> bool VariableImpl<float2>::isFloat2() const { return true; }
	template<> bool VariableImpl<float3>::isFloat3() const { return true; }
	template<> bool VariableImpl<float4>::isFloat4() const { return true; }
	template<> bool VariableImpl<std::string>::isString() const { return true; }

	template<> bool VariableImpl<bool>::getBool() const { return getData(); }
	template<> int VariableImpl<bool>::getInt() const { return getData()==true ? 1 : 0; }
	template<> float VariableImpl<bool>::getFloat() const { return getData()==true ? 1.f : 0.f; }
	template<> std::string const& VariableImpl<bool>::getString() const
	{
		static const std::string _true = "true", _false = "false";
		return getData()==true ? _true : _false;
	}

	template<> bool VariableImpl<int>::getBool() const { return getData() != 0; }
	template<> int VariableImpl<int>::getInt() const { return getData(); }
	template<> float VariableImpl<int>::getFloat() const { return (float)getData(); }

	template<> int2 VariableImpl<int2>::getInt2() const { return getData(); }
	template<> int3 VariableImpl<int3>::getInt3() const { return getData(); }

	template<> bool VariableImpl<float>::getBool() const { return getData() != 0.f; }
	template<> int VariableImpl<float> ::getInt() const { return (int)getData(); }
	template<> float VariableImpl<float>::getFloat() const { return getData(); }

	template<> float2 VariableImpl<float2>::getFloat2() const { return getData(); }
	template<> float3 VariableImpl<float3>::getFloat3() const { return getData(); }
	template<> float4 VariableImpl<float4>::getFloat4() const { return getData(); }

	template<> int VariableImpl<std::string>::getInt() const { return std::atoi(getDataRef().c_str()); }
	template<> float VariableImpl<std::string>::getFloat() const { return (float)std::atof(getDataRef().c_str()); }
	template<> std::string const& VariableImpl<std::string>::getString() const { return getDataRef(); }

	template<> void VariableImpl<bool>::setBool(bool value, SetBy setby) { setData(value, setby); }
	template<> void VariableImpl<int>::setInt(int value, SetBy setby) { setData(value, setby); }
	template<> void VariableImpl<int2>::setInt2(int2 value, SetBy setby) { setData(value, setby); }
	template<> void VariableImpl<int3>::setInt3(int3 value, SetBy setby) { setData(value, setby); }
	template<> void VariableImpl<float>::setFloat(float value, SetBy setby) { setData(value, setby); }
	template<> void VariableImpl<float2>::setFloat2(float2 value, SetBy setby) { setData(value, setby); }
	template<> void VariableImpl<float3>::setFloat3(float3 value, SetBy setby) { setData(value, setby); }
	template<> void VariableImpl<float4>::setFloat4(float4 value, SetBy setby) { setData(value, setby); }
	template<> void VariableImpl<std::string>::setString(std::string const& value, SetBy setby) { setData(value, setby); }

	template <typename T> std::string vector_to_string(T v)
	{
		std::string result;
		for (int i = 0; i < T::DIM; ++i)
		{
			char buff[16] = { 0 };
			if (auto [p, ec] = std::to_chars(buff, buff + 16, v[i]); ec == std::errc())
			{
				if (i < (T::DIM - 1))
					*p = ' ';

				result += buff;
			}
		}
		return result;
	}

	// A specialization of vector_to_string to work around std::to_chars being unavailable for float on clang
	template <int N> std::string float_vector_to_string(math::vector<float, N> v)
	{
		std::string result;
		for (int i = 0; i < N; ++i)
		{
			char buff[16] = { 0 };
			int c = snprintf(buff, sizeof(buff), "%f", v[i]);

			if (c < sizeof(buff))
			{
				if (i < (N - 1))
					buff[strlen(buff)] = ' ';

				result += buff;
			}
		}
		return result;
	}

	std::string float_to_string(float v)
	{
		char buff[16] = { 0 };
		snprintf(buff, sizeof(buff), "%f", v);
		return buff;
	}

	template <> std::string VariableImpl<bool>::getValueAsString() const
	{
		bool const value = getData();
		for (auto const& choice : m_choices)
			if (choice.second == value)
				return choice.first;
		return value ? "true" : "false";
	}
	template <> std::string VariableImpl<int2>::getValueAsString() const { return vector_to_string(getData()); }
	template <> std::string VariableImpl<int3>::getValueAsString() const { return vector_to_string(getData()); }
    template <> std::string VariableImpl<float>::getValueAsString() const { return float_to_string(getData()); }
    template <> std::string VariableImpl<float2>::getValueAsString() const { return float_vector_to_string(getData()); }
	template <> std::string VariableImpl<float3>::getValueAsString() const { return float_vector_to_string(getData()); }
	template <> std::string VariableImpl<float4>::getValueAsString() const { return float_vector_to_string(getData()); }
	template <> std::string VariableImpl<std::string>::getValueAsString() const { return getData(); }

	//
	// Console Variable Reference
	//

#define DEFINE_CVARREF_IMPLEMENTATION(type) \
	template <> AutoVariable<type>::AutoVariable(char const* name, char const* description, type const& value, bool ronly, bool cheat) \
	: m_variable(*(VariableImpl<type>*)objectsDictionary.RegisterVariable<type>( \
	    name, description, value, VariableState(ronly, cheat, VariableType::isA<type>(), VariableState::CODE))) { } \
    \
	template <> std::string const& AutoVariable<type>::getName() const { return objectsDictionary.GetObjectName(&m_variable); } \
	template <> std::string const& AutoVariable<type>::getDescription() const { return m_variable.getDescription(); } \
	template <> void AutoVariable<type>::setDescription(std::string const& description) { m_variable.setDescription(description); } \
	template <> VariableState AutoVariable<type>::getState() const { return m_variable.getState(); } \
	template <> type AutoVariable<type>::getValue() const { return m_variable.getData(); } \
	template <> void AutoVariable<type>::setValue(type const& value) { m_variable.setData(value, VariableState::CODE); } \
    template <> void AutoVariable<type>::setOnChangeCallback(Variable::Callback onChange) { m_variable.setOnChangeCallback(onChange); } \
	template <> void AutoVariable<type>::executeOnChangeCallback() { m_variable.executeOnChangeCallback(); } \
	template <> Variable* AutoVariable<type>::operator &() { return &m_variable; } \
	template <> AutoVariable<type>::operator type() const { return getValue(); } \
	template <> AutoVariable<type>& AutoVariable<type>::operator=(const type& value) { setValue(value); return *this; }


	DEFINE_CVARREF_IMPLEMENTATION(bool);
	DEFINE_CVARREF_IMPLEMENTATION(int);
	DEFINE_CVARREF_IMPLEMENTATION(int2);
	DEFINE_CVARREF_IMPLEMENTATION(int3);
	DEFINE_CVARREF_IMPLEMENTATION(float);
	DEFINE_CVARREF_IMPLEMENTATION(float2);
	DEFINE_CVARREF_IMPLEMENTATION(float3);
	DEFINE_CVARREF_IMPLEMENTATION(float4);
	DEFINE_CVARREF_IMPLEMENTATION(std::string);

	//
	// Dictionary implementation
	//

	bool registerCommand(CommandDesc const& desc)
	{
		return objectsDictionary.registerCommand(desc) != nullptr;
	}

#define DEFINE_BOUND_VARIABLE_REGISTRATION(type) \
	template <> Variable* registerBoundVariable<type>(BoundVariableDesc<type> const& desc) \
	{ \
		if (Object* object = objectsDictionary.RegisterBoundVariable<type>(desc)) \
			return object->asVariable(); \
		return nullptr; \
	}

	DEFINE_BOUND_VARIABLE_REGISTRATION(bool);
	DEFINE_BOUND_VARIABLE_REGISTRATION(int);
	DEFINE_BOUND_VARIABLE_REGISTRATION(float);
	DEFINE_BOUND_VARIABLE_REGISTRATION(math::int2);
	DEFINE_BOUND_VARIABLE_REGISTRATION(math::int3);
	DEFINE_BOUND_VARIABLE_REGISTRATION(math::float2);
	DEFINE_BOUND_VARIABLE_REGISTRATION(math::float3);
	DEFINE_BOUND_VARIABLE_REGISTRATION(math::float4);
	DEFINE_BOUND_VARIABLE_REGISTRATION(std::string);

#undef DEFINE_BOUND_VARIABLE_REGISTRATION

	void lockStartupVariables()
	{
		g_StartupVariablesLocked.store(true, std::memory_order_release);
	}

	bool startupVariablesLocked()
	{
		return g_StartupVariablesLocked.load(std::memory_order_acquire);
	}

	std::string dumpVariables(char const* regex, bool includeDeveloper)
	{
		std::string output;
		for (Object* object : matchObjects(regex))
		{
			Variable* variable = object->asVariable();
			if (!variable || (!includeDeveloper && hasFlag(variable->getFlags(), VariableFlags::DEVELOPER)))
				continue;

			output += variable->getName();
			output += " = ";
			output += variable->getValueAsString();
			if (!variable->getDefaultValueAsString().empty())
			{
				output += " (default: ";
				output += variable->getDefaultValueAsString();
				output += ')';
			}
			output += " [";
			output += asString(
				static_cast<VariableState::SetBy>(variable->getState().setby));
			output += "]\n";
		}
		return output;
	}

	size_t resetVariables(char const* regex, VariableState::SetBy setby)
	{
		size_t count = 0;
		for (Object* object : matchObjects(regex))
		{
			if (Variable* variable = object->asVariable())
				count += variable->resetToDefault(setby) ? 1u : 0u;
		}
		return count;
	}

	std::string exportVariables(char const* regex, bool archiveOnly)
	{
		std::string output;
		for (Object* object : matchObjects(regex))
		{
			Variable* variable = object->asVariable();
			if (!variable || (archiveOnly && !hasFlag(variable->getFlags(), VariableFlags::ARCHIVE)))
				continue;
			output += variable->getName();
			output += '=';
			output += variable->getValueAsString();
			output += '\n';
		}
		return output;
	}

	bool unregisterCommand(std::string_view name)
	{
		return objectsDictionary.unregisterCommand(name);
	}

	Object* findObject(std::string_view name)
	{
		return objectsDictionary.findObject(name);
	}

	std::vector<std::string_view> matchObjectNames(char const* regex)
	{
		return objectsDictionary.FindObjectNames(regex);
	}

	std::vector<Object*> matchObjects(char const* regex)
	{
		return objectsDictionary.FindObjects(regex);
	}

	Command* findCommand(std::string_view name)
	{
		if (Object* cobj = findObject(name))
			return cobj->asCommand();
		return nullptr;
	}

	Variable* findVariable(std::string_view name)
	{
		if (Object* cobj = findObject(name))
			return cobj->asVariable();
		return nullptr;
	}

	void resetAll()
	{
		objectsDictionary.reset();
	}

	void parseIniFile(char const* inidata, char const* filename)
	{
		if (!filename)
			filename = "<nullptr name>";

		std::stringstream inifile(inidata);

		uint32_t lineno = 0;
		for (std::string linestr; std::getline(inifile, linestr); ++lineno)
		{
			std::string_view line(linestr);

			// trim comments
			if (size_t pos = line.find('#'); pos != std::string::npos)
				line.remove_suffix(line.length() - pos);
			ds::trim(line);

			if (line.length() == 0)
				continue;

			auto tokens = ds::split(line, "[=]");
			if (tokens.size() == 0)
			{
				caustica::error(" % s: % d parse error : cannot find '=' - skipped line", filename, lineno);
				continue;
			}
			if (tokens.size() != 2)
			{
				caustica::error(" % s: % d parse error : invalid '<token> = <value>' format - skipped line", filename, lineno);
				continue;
			}

			std::string cvarname(tokens[0]);
			ds::trim(cvarname);

			std::string_view cvarvalue = tokens[1];
			ds::trim(cvarvalue);

			assert(cvarname.length() > 0 && cvarvalue.length() > 0);

			if (cvar* var = findVariable(cvarname.data()))
			{
				if (!var->setValueFromString(cvarvalue, VariableState::INI))
				{
					caustica::error("%s:%d parse error : cannot set value for variable '%s'", filename, lineno, cvarname.data());
				}
			}
			else
			{
				caustica::error("%s:%d parse error : unknown console variable name '%s'", filename, lineno, cvarname.data());
			}
		}
	}
} // end namespace caustica::console
