#pragma once

#include "input/api/ControllerState.h"

namespace WindowSystem
{
	struct WindowHandleInfo
	{
		enum class Backend
		{
			X11,
			Wayland,
			Cocoa,
			Windows,
		} backend;
		void* display = nullptr;
		void* surface = nullptr;
	};

	enum struct PlatformKeyCodes : uint32
	{
		LCONTROL,
		RCONTROL,
		TAB,
		ESCAPE,
	};

	struct WindowInfo
	{
		std::atomic_bool app_active; // our app is active/has focus

		std::atomic_int32_t width, height;			 // client size of main window
		std::atomic_int32_t phys_width, phys_height; // client size of main window in physical pixels
		std::atomic<double> dpi_scale;

		std::atomic_bool pad_open;							 // if separate pad view is open
		std::atomic_int32_t pad_width, pad_height;			 // client size of pad window
		std::atomic_int32_t phys_pad_width, phys_pad_height; // client size of pad window in physical pixels
		std::atomic<double> pad_dpi_scale;

		std::atomic_bool pad_maximized = false;
		std::atomic_int32_t restored_pad_x = -1, restored_pad_y = -1;
		std::atomic_int32_t restored_pad_width = -1, restored_pad_height = -1;

		std::atomic_bool is_fullscreen;
		std::atomic_bool debugger_focused;

		// Keys held, a bit each for the first kKeyCount key codes, set and read
		// from any thread with no lock
		static constexpr uint32 kKeyCount = 512;

		void set_keystate(uint32 keycode, bool state)
		{
			if (keycode >= kKeyCount)
				return;
			const uint32 bit = 1u << (keycode & 31);
			if (state)
				m_keydown[keycode >> 5].fetch_or(bit, std::memory_order_relaxed);
			else
				m_keydown[keycode >> 5].fetch_and(~bit, std::memory_order_relaxed);
		}

		bool get_keystate(uint32 keycode)
		{
			if (keycode >= kKeyCount)
				return false;
			return (m_keydown[keycode >> 5].load(std::memory_order_relaxed) >> (keycode & 31)) & 1;
		}

		void set_keystatesup()
		{
			for (auto& word : m_keydown)
				word.store(0, std::memory_order_relaxed);
		}

		WindowHandleInfo window_main;
		WindowHandleInfo window_pad;

		// canvas
		WindowHandleInfo canvas_main;
		WindowHandleInfo canvas_pad;

	  private:
		std::atomic<uint32> m_keydown[kKeyCount / 32] = {};
	};

	enum class ErrorCategory
	{
		KEYS_TXT_CREATION = 0,
		GRAPHIC_PACKS = 1,
	};

	void ShowErrorDialog(std::string_view message, std::string_view title, std::optional<ErrorCategory> errorCategory = {});
	inline void ShowErrorDialog(std::string_view message, std::optional<ErrorCategory> errorCategory = {})
	{
		ShowErrorDialog(message, "", errorCategory);
	}

	void Create();

	WindowInfo& GetWindowInfo();

	void UpdateWindowTitles(bool isIdle, bool isLoading, double fps);
	void GetWindowSize(int& w, int& h);
	void GetPadWindowSize(int& w, int& h);
	void GetWindowPhysSize(int& w, int& h);
	void GetPadWindowPhysSize(int& w, int& h);
	double GetWindowDPIScale();
	double GetPadDPIScale();
	bool IsPadWindowOpen();
	bool IsKeyDown(uint32 key);
	bool IsKeyDown(PlatformKeyCodes key);
	std::string GetKeyCodeName(uint32 key);

	bool InputConfigWindowHasFocus();

	void NotifyGameLoaded();
	void NotifyGameExited();

	void RefreshGameList();

	bool IsFullScreen();

	void CaptureInput(const ControllerState& currentState, const ControllerState& lastState);
}; // namespace WindowSystem
