#pragma once

// Ending a std::thread from deep inside what it runs, as if its function had
// returned: ThreadExit_Unwind() unwinds the thread's stack - running every
// destructor on the way, as a return would - to ThreadExit_Run(), which the
// thread's function is started through, and the thread ends there.
//
// Not pthread_exit: glibc ends a thread by a forced unwind, which the C++
// runtime's thread entry (execute_native_thread_routine) does not let
// through when the runtime is linked in statically, as the core links it -
// abort(), on every close of a title (test/thread_exit). Not on Windows
// either: _endthreadex there gives back what the C runtime took when it
// started the thread, and a std::thread returning does the same.
struct ThreadExitUnwind
{
};

[[noreturn]] inline void ThreadExit_Unwind()
{
	throw ThreadExitUnwind{};
}

// fn on the calling thread, until it returns or ThreadExit_Unwind() is called
// under it
template<typename TFn>
void ThreadExit_Run(TFn&& fn)
{
	try
	{
		fn();
	}
	catch (const ThreadExitUnwind&)
	{
	}
}
