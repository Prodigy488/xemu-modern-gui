#pragma once
#include "common.hh"

class InputManager
{
protected:
	ImVec2 m_last_mouse_pos;
	bool m_navigating_with_controller;
	uint32_t m_buttons;
	uint32_t m_triggers;
	bool m_mouse_moved;

public:
	InputManager();
	void Update();
	inline bool IsNavigatingWithController() { return m_navigating_with_controller; }
	inline bool MouseMoved() { return m_mouse_moved; }
	inline uint32_t CombinedButtons() { return m_buttons; }
	// Triggers pulled more than halfway: bit 0 LT, bit 1 RT
	inline uint32_t CombinedTriggers() { return m_triggers; }
};

extern InputManager g_input_mgr;
