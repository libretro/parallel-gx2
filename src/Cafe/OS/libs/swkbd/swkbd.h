#include "Cafe/OS/RPL/COSModule.h"

bool swkbd_hasKeyboardInputHook();
void swkbd_keyInput(uint32 keyCode);
std::wstring swkbd_getInputText();

namespace swkbd
{
	COSModule* GetModule();
}
