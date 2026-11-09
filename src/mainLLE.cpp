#include "util/crypto/aes128.h"
#include "util/helpers/MappedFile.h"
#include "WindowSystem.h"
#include "Common/FileStream.h"

void CemuCommonInit();

typedef struct  
{
	/* +0x000 */ uint32be magic;
}ppcAncastHeader_t;

void loadEncryptedPPCAncastKernel()
{
	MappedFile kernelData("kernel.img");
	if (kernelData.size() < sizeof(ppcAncastHeader_t))
		exit(-1);
	// check header
	const ppcAncastHeader_t* ancastHeader = (const ppcAncastHeader_t*)kernelData.data();
	if(ancastHeader->magic != (uint32be)0xEFA282D9)
		assert_dbg(); // invalid magic
	memcpy(memory_getPointerFromPhysicalOffset(0x08000000), kernelData.data(), kernelData.size());
}

void loadPPCBootrom()
{
	MappedFile bootromData("bootrom.bin");
	if (!bootromData.opened())
		exit(-1);
	memcpy(memory_getPointerFromPhysicalOffset(0x00000000), bootromData.data(), bootromData.size());
}

void mainEmulatorLLE()
{
	CemuCommonInit();
	// memory init
	memory_initPhysicalLayout();
	
	// start GUI thread
	WindowSystem::Create();
	// load kernel ancast image
	loadPPCBootrom();
	loadEncryptedPPCAncastKernel();
	
	PPCTimer_waitForInit();
	// begin execution
	PPCCoreLLE_startSingleCoreScheduler(0x00000100);
}

