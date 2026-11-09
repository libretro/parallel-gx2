#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// An error dialog with nothing of the core's drawn over the picture: what it
// says goes to the frontend's messages, and the controllers the game hands
// the viewer every frame answer it, A for its first button and B for its
// second, as on the console.

// The dialog as words: its error code (if it has one), title and text, and its
// buttons with the keys that pick them. button2 empty: a one-button dialog.
inline std::string ErrEulaPrompt(uint32_t errorCode, const std::string& title, const std::string& text,
								 const std::string& button1, const std::string& button2)
{
	std::string s;
	if (errorCode != 0)
	{
		char code[32];
		snprintf(code, sizeof(code), "Error-Code: %03u-%04u\n", (unsigned)(errorCode / 10000), (unsigned)(errorCode % 10000));
		s = code;
	}
	if (!title.empty())
		s += title + "\n";
	s += text;
	s += "\n(A) " + button1;
	if (!button2.empty())
		s += "   (B) " + button2;
	return s;
}

enum class ErrEulaPick
{
	None,
	First,
	Second,
};

// The button the controllers pick this frame from what was pressed on it:
// vpadTrig is the GamePad's (VPAD A 0x8000, B 0x4000), wpadTrig the Wii
// Remotes' together (WPAD A 0x800, B 0x400). B answers only a dialog with a
// second button; A wins when both are pressed.
inline ErrEulaPick ErrEulaAnswer(uint32_t vpadTrig, uint32_t wpadTrig, int buttons)
{
	if ((vpadTrig & 0x8000) || (wpadTrig & 0x800))
		return ErrEulaPick::First;
	if (buttons == 2 && ((vpadTrig & 0x4000) || (wpadTrig & 0x400)))
		return ErrEulaPick::Second;
	return ErrEulaPick::None;
}
