#pragma once


template<typename T>
class SingletonClass
{
public:
	static T* getInstance()
	{
		static T instance;
		return &instance;
	}

protected:
	SingletonClass() = default;
};
