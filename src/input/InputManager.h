#pragma once

#include "util/helpers/RcuCell.h"

#include "input/api/Libretro/LibretroControllerProvider.h"

#include "input/emulated/VPADController.h"
#include "input/emulated/WPADController.h"

#include "util/helpers/Singleton.h"

class InputManager : public Singleton<InputManager>
{
	friend class Singleton<InputManager>;
	InputManager();
	~InputManager();

	friend class MainWindow;
	friend class PadViewFrame;

public:
	constexpr static size_t kMaxController = 8;
	constexpr static size_t kMaxVPADControllers = 2;
	constexpr static size_t kMaxWPADControllers = 7;
	
	void load() noexcept;
	bool load(size_t player_index, std::string_view filename = {});

	bool migrate_config(const fs::path& file_path);

	void save() noexcept;
	bool save(size_t player_index, std::string_view filename = {});

	bool is_gameprofile_set(size_t player_index) const;

	void Shutdown(); 
	
	EmulatedControllerPtr set_controller(EmulatedControllerPtr controller);
	EmulatedControllerPtr set_controller(size_t player_index, EmulatedController::Type type);
	EmulatedControllerPtr set_controller(size_t player_index, EmulatedController::Type type, const std::shared_ptr<ControllerBase>& controller);

	EmulatedControllerPtr delete_controller(size_t player_index, bool delete_profile = false);
	
	EmulatedControllerPtr get_controller(size_t player_index) const;
	std::shared_ptr<VPADController> get_vpad_controller(size_t index) const;
	std::shared_ptr<WPADController> get_wpad_controller(size_t index) const;
	std::pair<size_t, size_t> get_controller_count() const;

	bool is_api_available(InputAPI::Type api) const { return !m_api_available[api].empty(); }
	
	ControllerProviderPtr get_api_provider(std::string_view api_name) const;
	ControllerProviderPtr get_api_provider(InputAPI::Type api) const;
	// will create the provider with the given settings if it doesn't exist yet
	ControllerProviderPtr get_api_provider(InputAPI::Type api, const ControllerProviderSettings& settings);

	const auto& get_api_providers() const
	{
		return m_api_available;
	}

	void apply_game_profile();
	void on_device_changed();


	static std::vector<std::string> get_profiles();
	static bool is_valid_profilename(const std::string& name);

	// One pass over every emulated controller: what retro_run does once a
	// frame, right after the frontend's input is polled (upstream's update
	// thread did it every millisecond)
	void update_controllers();

private:

	std::array<std::vector<ControllerProviderPtr>, InputAPI::MAX> m_api_available{ };

	// The writers' copy - set_controller and delete_controller, which only
	// the code that sets controllers up calls - and what they publish for
	// the game's threads to read with no lock (RcuCell.h)
	std::array<EmulatedControllerPtr, kMaxVPADControllers> m_vpad;
	std::array<EmulatedControllerPtr, kMaxWPADControllers> m_wpad;
	struct Pads
	{
		std::array<EmulatedControllerPtr, kMaxVPADControllers> vpad;
		std::array<EmulatedControllerPtr, kMaxWPADControllers> wpad;
	};
	RcuCell<Pads> m_pads;
	template<typename TChange>
	auto ChangePads(TChange change)
	{
		using R = decltype(change());
		if constexpr (std::is_void_v<R>)
		{
			m_pads.Write([&] { change(); return Pads{m_vpad, m_wpad}; });
		}
		else
		{
			R result{};
			m_pads.Write([&] { result = change(); return Pads{m_vpad, m_wpad}; });
			return result;
		}
	}

	std::array<bool, kMaxController> m_is_gameprofile_set{};

	template<std::derived_from<ControllerProviderBase> TProvider>
	void create_provider()
	{
		try
		{
			auto controller = std::make_shared<TProvider>();
			m_api_available[controller->api()] = std::vector<ControllerProviderPtr>{controller};
		} catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, ex.what());
		}
	}
};
