#include <IOKit/nvram/IONVRAMController.h>

// A class providing PMU XPRAM access for machines which only have XPRAM (i.e. M2)
//  Emulates an Old World NVRAM, but only the XPRAM part

#define kNVRAMSize 0x2000

class OpenPMU;

class OpenPMUXPRAMController : public IONVRAMController
{
    OSDeclareDefaultStructors(OpenPMUXPRAMController)
	
public:
	virtual bool init(OSDictionary *regEntry, OpenPMU *driver);
	virtual bool start(IOService *provider);
	virtual void sync();
	
	virtual IOReturn read(IOByteCount offset, UInt8 *buffer, IOByteCount length);
	virtual IOReturn write(IOByteCount offset, UInt8 *buffer, IOByteCount length);

	OpenPMU *openPMUDriver;

private:
	bool writeOutXPRAM();

	UInt8 nvramBuf[kNVRAMSize];
};