#include "input/emulated/VPADController.h"
#include "input/api/Controller.h"
#include "WindowSystem.h"
#include "input/InputManager.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/CafeSystem.h"

enum ControllerVPADMapping2 : uint32
{
	VPAD_A = 0x8000,
	VPAD_B = 0x4000,
	VPAD_X = 0x2000,
	VPAD_Y = 0x1000,

	VPAD_L = 0x0020,
	VPAD_R = 0x0010,
	VPAD_ZL = 0x0080,
	VPAD_ZR = 0x0040,

	VPAD_PLUS = 0x0008,
	VPAD_MINUS = 0x0004,
	VPAD_HOME = 0x0002,

	VPAD_UP = 0x0200,
	VPAD_DOWN = 0x0100,
	VPAD_LEFT = 0x0800,
	VPAD_RIGHT = 0x0400,

	VPAD_STICK_R = 0x00020000,
	VPAD_STICK_L = 0x00040000,

	VPAD_STICK_L_UP = 0x10000000,
	VPAD_STICK_L_DOWN = 0x08000000,
	VPAD_STICK_L_LEFT = 0x40000000,
	VPAD_STICK_L_RIGHT = 0x20000000,

	VPAD_STICK_R_UP = 0x01000000,
	VPAD_STICK_R_DOWN = 0x00800000,
	VPAD_STICK_R_LEFT = 0x04000000,
	VPAD_STICK_R_RIGHT = 0x02000000,

	// special flag
	VPAD_REPEAT = 0x80000000,
};

void VPADController::VPADRead(VPADStatus_t& status, const BtnRepeat& repeat)
{
	controllers_update_states();
	m_mic_active = false;
	m_screen_active = false;
	for (uint32 i = kButtonId_A; i < kButtonId_Max; ++i)
	{
		// axis will be aplied later
		if (is_axis_mapping(i))
			continue;

		if (is_mapping_down(i))
		{
			const uint32 value = get_emulated_button_flag(i);
			if (value == 0)
			{
				// special buttons
				if (i == kButtonId_Mic)
					m_mic_active = true;
				else if (i == kButtonId_Screen)
					m_screen_active = true;

				continue;
			}

			status.hold |= value;
		}
	}

	m_homebutton_down |= is_home_down();

	const auto axis = get_axis();
	status.leftStick.x = axis.x;
	status.leftStick.y = axis.y;

	constexpr float kAxisThreshold = 0.5f;
	constexpr float kHoldAxisThreshold = 0.1f;
	const uint32 last_hold = m_last_holdvalue;

	if (axis.x <= -kAxisThreshold || (HAS_FLAG(last_hold, VPAD_STICK_L_LEFT) && axis.x <= -kHoldAxisThreshold))
		status.hold |= VPAD_STICK_L_LEFT;
	else if (axis.x >= kAxisThreshold || (HAS_FLAG(last_hold, VPAD_STICK_L_RIGHT) && axis.x >= kHoldAxisThreshold))
		status.hold |= VPAD_STICK_L_RIGHT;

	if (axis.y <= -kAxisThreshold || (HAS_FLAG(last_hold, VPAD_STICK_L_DOWN) && axis.y <= -kHoldAxisThreshold))
		status.hold |= VPAD_STICK_L_DOWN;
	else if (axis.y >= kAxisThreshold || (HAS_FLAG(last_hold, VPAD_STICK_L_UP) && axis.y >= kHoldAxisThreshold))
		status.hold |= VPAD_STICK_L_UP;

	const auto rotation = get_rotation();
	status.rightStick.x = rotation.x;
	status.rightStick.y = rotation.y;

	if (rotation.x <= -kAxisThreshold || (HAS_FLAG(last_hold, VPAD_STICK_R_LEFT) && rotation.x <= -kHoldAxisThreshold))
		status.hold |= VPAD_STICK_R_LEFT;
	else if (rotation.x >= kAxisThreshold || (HAS_FLAG(last_hold, VPAD_STICK_R_RIGHT) && rotation.x >=
		kHoldAxisThreshold))
		status.hold |= VPAD_STICK_R_RIGHT;

	if (rotation.y <= -kAxisThreshold || (HAS_FLAG(last_hold, VPAD_STICK_R_DOWN) && rotation.y <= -kHoldAxisThreshold))
		status.hold |= VPAD_STICK_R_DOWN;
	else if (rotation.y >= kAxisThreshold || (HAS_FLAG(last_hold, VPAD_STICK_R_UP) && rotation.y >= kHoldAxisThreshold))
		status.hold |= VPAD_STICK_R_UP;

	// button repeat
	const auto now = std::chrono::high_resolution_clock::now();
	uint32 currentHold = status.hold;
	if (currentHold != m_last_holdvalue)
	{
		m_last_hold_change = m_last_pulse = now;
	}

	if (repeat.pulse > 0)
	{
		if (currentHold != 0 && m_last_hold_change + std::chrono::milliseconds(repeat.delay) <= now)
		{
			if ((m_last_pulse + std::chrono::milliseconds(repeat.pulse)) < now)
			{
				m_last_pulse = now;
				status.hold |= VPAD_REPEAT;
			}
		}
	}

	// general
	status.release = m_last_holdvalue & ~currentHold;
	status.trig = ~m_last_holdvalue & currentHold;
	m_last_holdvalue = currentHold;

	// touch
	update_touch(status);

	// motion
	status.dir.x = {1, 0, 0};
	status.dir.y = {0, 1, 0};
	status.dir.z = {0, 0, 1};
	status.accXY = {1.0f, 0.0f};
	update_motion(status);
}

void VPADController::update()
{
	EmulatedController::update();

	if (!CafeSystem::IsTitleRunning())
		return;

	const bool on = m_rumble && rumble_queue_step(m_rumble,
		std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	if (on == m_rumble_on)
		return;
	m_rumble_on = on;
	if (on)
		start_rumble();
	else
		stop_rumble();
}

void VPADController::update_touch(VPADStatus_t& status)
{
	status.tpData.touch = kTpTouchOff;
	status.tpData.validity = kTpInvalid;
	// keep x,y from previous update
	// NGDK (Neko Game Development Kit 2) games (e.g. Mysterios Cities of Gold) rely on x/y remaining intact after touch is released
	status.tpData.x = (uint16)m_last_touch_position.x;
	status.tpData.y = (uint16)m_last_touch_position.y;

	if (has_position())
	{
		const auto mouse = get_position();

		status.tpData.touch = kTpTouchOn;
		status.tpData.validity = kTpValid;
		status.tpData.x = (uint16)(mouse.x * 3883.0f + 92.0f);
		status.tpData.y = (uint16)(4095.0f - mouse.y * 3694.0f - 254.0f);

		m_last_touch_position = glm::ivec2{status.tpData.x, status.tpData.y};
	}

	status.tpProcessed1 = status.tpData;
	status.tpProcessed2 = status.tpData;
}

void VPADController::update_motion(VPADStatus_t& status)
{
	if (has_motion())
	{
		auto motionSample = get_motion_data();

		glm::vec3 acc;
		motionSample.getVPADAccelerometer(&acc[0]);
		//const auto& acc = motionSample.getVPADAccelerometer();
		status.acc.x = acc.x;
		status.acc.y = acc.y;
		status.acc.z = acc.z;
		status.accMagnitude = motionSample.getVPADAccMagnitude();
		status.accAcceleration = motionSample.getVPADAccAcceleration();

		glm::vec3 gyroChange;
		motionSample.getVPADGyroChange(&gyroChange[0]);
		//const auto& gyroChange = motionSample.getVPADGyroChange();
		status.gyroChange.x = gyroChange.x;
		status.gyroChange.y = gyroChange.y;
		status.gyroChange.z = gyroChange.z;

		//debug_printf("GyroChange %7.2lf %7.2lf %7.2lf\n", (float)status.gyroChange.x, (float)status.gyroChange.y, (float)status.gyroChange.z);

		glm::vec3 gyroOrientation;
		motionSample.getVPADOrientation(&gyroOrientation[0]);
		//const auto& gyroOrientation = motionSample.getVPADOrientation();
		status.gyroOrientation.x = gyroOrientation.x;
		status.gyroOrientation.y = gyroOrientation.y;
		status.gyroOrientation.z = gyroOrientation.z;

		float attitude[9];
		motionSample.getVPADAttitudeMatrix(attitude);
		status.dir.x.x = attitude[0];
		status.dir.x.y = attitude[1];
		status.dir.x.z = attitude[2];
		status.dir.y.x = attitude[3];
		status.dir.y.y = attitude[4];
		status.dir.y.z = attitude[5];
		status.dir.z.x = attitude[6];
		status.dir.z.y = attitude[7];
		status.dir.z.z = attitude[8];
	}
}


std::string_view VPADController::get_button_name(ButtonId id)
{
	switch (id)
	{
	case kButtonId_A: return "A";
	case kButtonId_B: return "B";
	case kButtonId_X: return "X";
	case kButtonId_Y: return "Y";
	case kButtonId_L: return "L";
	case kButtonId_R: return "R";
	case kButtonId_ZL: return "ZL";
	case kButtonId_ZR: return "ZR";
	case kButtonId_Plus: return "+";
	case kButtonId_Minus: return "-";
	case kButtonId_Up: return TR_NOOP("up");
	case kButtonId_Down: return TR_NOOP("down");
	case kButtonId_Left: return TR_NOOP("left");
	case kButtonId_Right: return TR_NOOP("right");
	case kButtonId_StickL: return TR_NOOP("click");
	case kButtonId_StickR: return TR_NOOP("click");
	case kButtonId_StickL_Up: return TR_NOOP("up");
	case kButtonId_StickL_Down: return TR_NOOP("down");
	case kButtonId_StickL_Left: return TR_NOOP("left");
	case kButtonId_StickL_Right: return TR_NOOP("right");
	case kButtonId_StickR_Up: return TR_NOOP("up");
	case kButtonId_StickR_Down: return TR_NOOP("down");
	case kButtonId_StickR_Left: return TR_NOOP("left");
	case kButtonId_StickR_Right: return TR_NOOP("right");
	case kButtonId_Home: return TR_NOOP("home");
	default:
		cemu_assert_debug(false);
		return "";
	}
}

void VPADController::clear_rumble()
{
	if (m_rumble)
		rumble_queue_clear(m_rumble);
}

bool VPADController::push_rumble(uint8* pattern, uint8 length)
{
	if (!m_rumble)
		return false;
	if (!rumble_queue_push(m_rumble, pattern, length))
	{
		cemuLog_logDebugOnce(LogType::Force, "VPADControlMotor(): Pattern too long");
		return false;
	}
	return true;
}

uint32 VPADController::get_emulated_button_flag(uint32 id) const
{
	switch (id)
	{
	case kButtonId_A: return VPAD_A;
	case kButtonId_B: return VPAD_B;
	case kButtonId_X: return VPAD_X;
	case kButtonId_Y: return VPAD_Y;
	case kButtonId_L: return VPAD_L;
	case kButtonId_R: return VPAD_R;
	case kButtonId_ZL: return VPAD_ZL;
	case kButtonId_ZR: return VPAD_ZR;
	case kButtonId_Plus: return VPAD_PLUS;
	case kButtonId_Minus: return VPAD_MINUS;
	case kButtonId_Up: return VPAD_UP;
	case kButtonId_Down: return VPAD_DOWN;
	case kButtonId_Left: return VPAD_LEFT;
	case kButtonId_Right: return VPAD_RIGHT;
	case kButtonId_StickL: return VPAD_STICK_L;
	case kButtonId_StickR: return VPAD_STICK_R;

	case kButtonId_StickL_Up: return VPAD_STICK_L_UP;
	case kButtonId_StickL_Down: return VPAD_STICK_L_DOWN;
	case kButtonId_StickL_Left: return VPAD_STICK_L_LEFT;
	case kButtonId_StickL_Right: return VPAD_STICK_L_RIGHT;

	case kButtonId_StickR_Up: return VPAD_STICK_R_UP;
	case kButtonId_StickR_Down: return VPAD_STICK_R_DOWN;
	case kButtonId_StickR_Left: return VPAD_STICK_R_LEFT;
	case kButtonId_StickR_Right: return VPAD_STICK_R_RIGHT;
	}
	return 0;
}

glm::vec2 VPADController::get_axis() const
{
	const auto left = get_axis_value(kButtonId_StickL_Left);
	const auto right = get_axis_value(kButtonId_StickL_Right);

	const auto up = get_axis_value(kButtonId_StickL_Up);
	const auto down = get_axis_value(kButtonId_StickL_Down);

	glm::vec2 result;
	result.x = (left > right) ? -left : right;
	result.y = (up > down) ? up : -down;
	return length(result) > 1.0f ? normalize(result) : result;
}

glm::vec2 VPADController::get_rotation() const
{
	const auto left = get_axis_value(kButtonId_StickR_Left);
	const auto right = get_axis_value(kButtonId_StickR_Right);

	const auto up = get_axis_value(kButtonId_StickR_Up);
	const auto down = get_axis_value(kButtonId_StickR_Down);

	glm::vec2 result;
	result.x = (left > right) ? -left : right;
	result.y = (up > down) ? up : -down;
	return length(result) > 1.0f ? normalize(result) : result;
}

glm::vec2 VPADController::get_trigger() const
{
	const auto left = get_axis_value(kButtonId_ZL);
	const auto right = get_axis_value(kButtonId_ZR);
	return {left, right};
}

bool VPADController::set_default_mapping(const std::shared_ptr<ControllerBase>& controller)
{
	std::vector<std::pair<uint64, uint64>> mapping;
	switch (controller->api())
	{
	case InputAPI::XInput:
	{
		mapping =
		{
			{kButtonId_A, kButton13},
			{kButtonId_B, kButton12},
			{kButtonId_X, kButton15},
			{kButtonId_Y, kButton14},

			{kButtonId_L, kButton8},
			{kButtonId_R, kButton9},
			{kButtonId_ZL, kTriggerXP},
			{kButtonId_ZR, kTriggerYP},

			{kButtonId_Plus, kButton4},
			{kButtonId_Minus, kButton5},

			{kButtonId_Up, kButton0},
			{kButtonId_Down, kButton1},
			{kButtonId_Left, kButton2},
			{kButtonId_Right, kButton3},

			{kButtonId_StickL, kButton6},
			{kButtonId_StickR, kButton7},

			{kButtonId_StickL_Up, kAxisYP},
			{kButtonId_StickL_Down, kAxisYN},
			{kButtonId_StickL_Left, kAxisXN},
			{kButtonId_StickL_Right, kAxisXP},

			{kButtonId_StickR_Up, kRotationYP},
			{kButtonId_StickR_Down, kRotationYN},
			{kButtonId_StickR_Left, kRotationXN},
			{kButtonId_StickR_Right, kRotationXP},
		};
		
		break;
	}
	}

	bool mapping_updated = false;
	std::for_each(mapping.cbegin(), mapping.cend(), [this, &controller, &mapping_updated](const auto& m)
		{
			if (m_mappings.find(m.first) == m_mappings.cend())
			{
				set_mapping(m.first, controller, m.second);
				mapping_updated = true;
			}
		});

	return mapping_updated;
}

void VPADController::load(const xml::Node& node)
{
	if (const auto value = node.child("toggle_display"))
		m_screen_active_toggle = ConvertString<bool>(value.child_value());
}

void VPADController::save(xml::Node& node)
{
	node.append_child("toggle_display").text().set(fmt::format("{}", (int)m_screen_active_toggle).c_str());
}
