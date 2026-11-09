// The Wii U's error viewer (erreula) with nothing drawn over the picture
// (src/Cafe/OS/libs/erreula/ErrEulaPrompt.h): the text it hands the frontend's
// messages names every button with the key that picks it, and the
// controllers the game passes the viewer answer it - A the first button, B
// the second and only where there is one - from the GamePad or any Wii Remote.
#include "Cafe/OS/libs/erreula/ErrEulaPrompt.h"

#include <cstdio>
#include <cstdlib>

static int s_failures = 0;

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

static void TestPrompt()
{
	CHECK(ErrEulaPrompt(1234567, "", "", "OK", "") == "Error-Code: 123-4567\n\n(A) OK");
	CHECK(ErrEulaPrompt(1020003, "", "", "OK", "") == "Error-Code: 102-0003\n\n(A) OK");
	CHECK(ErrEulaPrompt(0, "Title", "Save?", "Yes", "No") == "Title\nSave?\n(A) Yes   (B) No");
	CHECK(ErrEulaPrompt(0, "", "Update needed", "OK", "") == "Update needed\n(A) OK");
}

static void TestAnswer()
{
	// nothing pressed
	CHECK(ErrEulaAnswer(0, 0, 1) == ErrEulaPick::None);
	CHECK(ErrEulaAnswer(0, 0, 2) == ErrEulaPick::None);
	// A, from the GamePad or a Wii Remote, answers either dialog
	CHECK(ErrEulaAnswer(0x8000, 0, 1) == ErrEulaPick::First);
	CHECK(ErrEulaAnswer(0x8000, 0, 2) == ErrEulaPick::First);
	CHECK(ErrEulaAnswer(0, 0x800, 1) == ErrEulaPick::First);
	CHECK(ErrEulaAnswer(0, 0x800, 2) == ErrEulaPick::First);
	// B only a dialog with a second button
	CHECK(ErrEulaAnswer(0x4000, 0, 2) == ErrEulaPick::Second);
	CHECK(ErrEulaAnswer(0, 0x400, 2) == ErrEulaPick::Second);
	CHECK(ErrEulaAnswer(0x4000, 0, 1) == ErrEulaPick::None);
	CHECK(ErrEulaAnswer(0, 0x400, 1) == ErrEulaPick::None);
	// A wins when both are pressed
	CHECK(ErrEulaAnswer(0xC000, 0, 2) == ErrEulaPick::First);
	CHECK(ErrEulaAnswer(0x4000, 0x800, 2) == ErrEulaPick::First);
	// another key picks nothing: the bits are each pad's own - a Wii
	// Remote's 0x8000 is HOME, the GamePad's 0x800 is left on the +Control Pad
	CHECK(ErrEulaAnswer(0x0001, 0x0001, 2) == ErrEulaPick::None);
	CHECK(ErrEulaAnswer(0, 0x8000, 2) == ErrEulaPick::None);
	CHECK(ErrEulaAnswer(0x800, 0, 2) == ErrEulaPick::None);
}

int main()
{
	TestPrompt();
	TestAnswer();
	if (s_failures)
	{
		fprintf(stderr, "%d failed\n", s_failures);
		return 1;
	}
	printf("erreula: all passed\n");
	return 0;
}
