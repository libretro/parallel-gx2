#pragma once

#include <glm/vec2.hpp>
#include <array>
#include <span>
#include <vector>

enum class PositionVisibility {
	NONE = 0,
	FULL = 1,
	PARTIAL = 2
};

// The buttons pressed in one controller state: a bit per button id. A plain
// value - no lock - because a ControllerState belongs to one thread at a time:
// the one building it, and then everyone through ControllerBase's published
// copy (SeqlockValue), which they read whole. Was a vector of ids under a
// spinlock every reader of every button took.
struct ControllerButtonState
{
	static constexpr uint32 kMaxButtons = 256;

	void SetButtonState(uint32 buttonId, bool isPressed)
	{
		if (buttonId >= kMaxButtons)
			return;
		if (isPressed)
			m_bits[buttonId / 64] |= 1ull << (buttonId % 64);
		else
			m_bits[buttonId / 64] &= ~(1ull << (buttonId % 64));
	}

	// set multiple buttons at once
	void SetPressedButtons(std::span<uint32> buttonList)
	{
		for (auto& buttonId : buttonList)
			SetButtonState(buttonId, true);
	}

	// returns true if pressed
	bool GetButtonState(uint32 buttonId) const
	{
		if (buttonId >= kMaxButtons)
			return false;
		return (m_bits[buttonId / 64] >> (buttonId % 64)) & 1;
	}

	// remove pressed state for all pressed buttons in buttonsToUnset
	void UnsetButtons(const ControllerButtonState& buttonsToUnset)
	{
		for (size_t i = 0; i < m_bits.size(); i++)
			m_bits[i] &= ~buttonsToUnset.m_bits[i];
	}

	// returns true if no buttons are pressed
	bool IsIdle() const
	{
		for (uint64 w : m_bits)
		{
			if (w)
				return false;
		}
		return true;
	}

	std::vector<uint32> GetButtonList() const
	{
		std::vector<uint32> list;
		for (uint32 id = 0; id < kMaxButtons; id++)
		{
			if (GetButtonState(id))
				list.emplace_back(id);
		}
		return list;
	}

	bool operator==(const ControllerButtonState& other) const { return m_bits == other.m_bits; }

private:
	std::array<uint64, kMaxButtons / 64> m_bits{};
};

struct ControllerState
{
	// when does a axis counts as pressed
	constexpr static float kAxisThreshold = 0.1f;

	// on the real console the stick x or y values never really reach 0.0 if one of the axis is moved
	// some games rely on this due to incorrectly checking if the stick is tilted via if (vstick.x != 0 && vstick.y != 0)
	// here we simulate a slight bias if the axis is almost perfectly centered
	constexpr static float kMinAxisValue = 0.0000001f;

	// [-1; 1]
	glm::vec2 axis{ };
	glm::vec2 rotation{ };
	glm::vec2 trigger{ };

	ControllerButtonState buttons{};

	uint64 last_state = 0;
};
