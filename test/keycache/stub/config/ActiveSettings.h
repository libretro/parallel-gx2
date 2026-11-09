#pragma once
// The user data folder, which is all KeyCache.cpp asks of ActiveSettings
class ActiveSettings
{
public:
	inline static fs::path s_userDataPath;
	template<typename... TArgs>
	static fs::path GetUserDataPath(TArgs&&... args) { return (s_userDataPath / ... / fs::path(args)); }
};
