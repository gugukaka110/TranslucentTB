#include "error.hpp"
#include <debugapi.h>
#include <intrin.h>
#include <roerrorapi.h>
#include <spdlog/spdlog.h>
#include <string>
#include <processthreadsapi.h>
#include <wil/resource.h>
#include <winnt.h>

#include "../log.hpp"
#include "std.hpp"
#include "win32.hpp"

bool Error::ShouldLog(spdlog::level::level_enum level)
{
	if (level == spdlog::level::err || level == spdlog::level::critical)
	{
		return true;
	}

	// implicitly checks if logging is initialized.
	if (const auto sink = Log::GetSink())
	{
		return sink->should_log(level) || IsDebuggerPresent();
	}

	return false;
}

void Error::impl::Log(const fmt::wmemory_buffer &msg, spdlog::level::level_enum level, Util::null_terminated_string_view file, int line, Util::null_terminated_string_view function)
{
	spdlog::log({ file.c_str(), line, function.c_str() }, level, Util::ToStringView(msg));
}

void Error::impl::GetLogMessage(fmt::wmemory_buffer &out, std::wstring_view message, std::wstring_view error_message, std::wstring_view err_message_fmt, std::wstring_view message_fmt)
{
	if (!error_message.empty())
	{
		fmt::format_to(out, err_message_fmt, message, error_message);
	}
	else
	{
		fmt::format_to(out, message_fmt, message);
	}
}

template<>
void Error::impl::Handle<spdlog::level::err>(std::wstring_view message, std::wstring_view error_message, Util::null_terminated_string_view file, int line, Util::null_terminated_string_view function, HRESULT, IRestrictedErrorInfo*)
{
	fmt::wmemory_buffer buf;

	// allow calls to err handling without needing to initialize logging
	if (Log::IsInitialized())
	{
		GetLogMessage(buf, message, error_message);
		Log(buf, spdlog::level::err, file, line, function);
	}

	if (!IsDebuggerPresent())
	{
		buf.clear();
		GetLogMessage(buf, message, error_message, ERROR_MESSAGE L"\n\n{}\n\n{}", ERROR_MESSAGE L"\n\n{}");

		CreateMessageBoxThread(buf, ERROR_TITLE, MB_ICONWARNING).detach();
	}
	else
	{
		DebugBreak();
	}
}

template<>
void Error::impl::Handle<spdlog::level::critical>(std::wstring_view message, std::wstring_view error_message, Util::null_terminated_string_view file, int line, Util::null_terminated_string_view function, HRESULT err, IRestrictedErrorInfo *errInfo)
{
	fmt::wmemory_buffer buf;

	// allow calls to critical handling without needing to initialize logging
	const bool initialized = Log::IsInitialized();
	if (initialized)
	{
		GetLogMessage(buf, message, error_message);
		Log(buf, spdlog::level::critical, file, line, function);
	}

	if (!IsDebuggerPresent())
	{
		buf.clear();
		GetLogMessage(buf, message, error_message, FATAL_ERROR_MESSAGE L"\n\n{}\n\n{}", FATAL_ERROR_MESSAGE L"\n\n{}");

		CreateMessageBoxThread(buf, FATAL_ERROR_TITLE, MB_ICONERROR | MB_TOPMOST).join();
	}

	if (errInfo)
	{
		if (const HRESULT hr = SetRestrictedErrorInfo(errInfo); SUCCEEDED(hr))
		{
			// This gives much better error reporting if the error came from a WinRT module:
			// the stack trace in the dump, debugger and telemetry is unaffected by our error handling,
			// giving us better insight into what went wrong.
			RoFailFastWithErrorContext(err);
		}
		else if (initialized)
		{
			HresultHandle(hr, spdlog::level::warn, L"Failed to set restricted error info");
		}
	}

	__fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

std::thread Error::impl::CreateMessageBoxThread(const fmt::wmemory_buffer &buf, Util::null_terminated_wstring_view title, unsigned int type)
{
	return std::thread([title, type, body = fmt::to_string(buf)]() noexcept
	{
#ifdef _DEBUG
		SetThreadDescription(GetCurrentThread(), APP_NAME L" Message Box Thread"); // ignore error
#endif
		MessageBoxEx(nullptr, body.c_str(), title.c_str(), type | MB_OK | MB_SETFOREGROUND, MAKELANGID(LANG_ENGLISH, SUBLANG_NEUTRAL));
	});
}
