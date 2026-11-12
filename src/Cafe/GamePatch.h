// the HLE functions GamePatch_scan patches titles to call, once at start
void GamePatch_init();
void GamePatch_scan();
bool GamePatch_IsNonReturnFunction(uint32 hleIndex);