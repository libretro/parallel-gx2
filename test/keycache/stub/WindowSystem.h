#pragma once
// The error dialog, which is all KeyCache.cpp asks of WindowSystem: counted
namespace WindowSystem
{
	enum class ErrorCategory
	{
		KEYS_TXT_CREATION = 0,
	};
	inline int s_errors = 0;
	inline void ShowErrorDialog(std::string_view message, std::optional<ErrorCategory> errorCategory = {}) { s_errors++; }
	inline void ShowErrorDialog(std::string_view message, std::string_view title, std::optional<ErrorCategory> errorCategory = {}) { s_errors++; }
}
