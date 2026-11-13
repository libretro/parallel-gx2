#pragma once
#include "Cafe/OS/RPL/COSModule.h"

namespace nlibcurl
{
	COSModule* GetModule();

	// the host threads transfers run on, for as long as the core is loaded;
	// unblock() ends the transfers they are in when they stop
	void StartWorkers();
	void StopWorkers(void (*unblock)());
}