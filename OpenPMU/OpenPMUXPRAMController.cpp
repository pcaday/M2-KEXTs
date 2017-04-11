#include "OpenPMUXPRAMController.h"
#include "OpenPMU.h"
#include "OpenPMUDebug.h"

#define kOWXPRAMPartitionOffset 0x1300
#define kOWXPRAMPartitionSize 0x100
#define kOWNRPartitionOffset 0x1400
#define kOWNRPartitionSize 0x400
#define kOWOFPartitionOffset 0x1800
#define kOWOFPartitionSize 0x800

struct OWVariablesHeader {
  UInt16   owMagic;
  UInt8    owVersion;
  UInt8    owPages;
  UInt16   owChecksum;
  UInt16   owHere;
  UInt16   owTop;
  UInt16   owNext;
  UInt32   owFlags;
  UInt32   owNumbers[9];
  struct {
    UInt16 offset;
    UInt16 length;
  }        owStrings[10];
};
typedef struct OWVariablesHeader OWVariablesHeader;


#define super IONVRAMController

OSDefineMetaClassAndStructors(OpenPMUXPRAMController, IONVRAMController)


bool OpenPMUXPRAMController::init(OSDictionary *regEntry, OpenPMU *driver)
{
	openPMUDriver = driver;
		
	return super::init(regEntry);
}

bool OpenPMUXPRAMController::start(IOService *provider)
{
	OWVariablesHeader *hdr;

#if DUMP_XPRAM
	int i;
	UInt8 *bp;
#endif

	if (!openPMUDriver) {
		Verbose_IOLog("OpenPMUXPRAMController::start() no PMU driver, failing\n");
		return false;
	}
	
	bzero(&nvramBuf[0], kNVRAMSize);
	
	// Read XPRAM in three chunks. I believe that the PMU driver cannot handle (return) lengths >= 0x80 bytes
	if (openPMUDriver->readXPRAM(0, &nvramBuf[kOWXPRAMPartitionOffset], 0x55) != kIOReturnSuccess)
		return false;
	if (openPMUDriver->readXPRAM(0x55, &nvramBuf[kOWXPRAMPartitionOffset + 0x55], 0x55) != kIOReturnSuccess)
		return false;
	if (openPMUDriver->readXPRAM(0xAA, &nvramBuf[kOWXPRAMPartitionOffset + 0xAA], 0x56) != kIOReturnSuccess)
		return false;

	hdr = (OWVariablesHeader *) &nvramBuf[kOWOFPartitionOffset];
	hdr->owChecksum = 0xBAD;					// Ensure IODTNVRAM doesn't try to load options from us

	#ifdef DUMP_XPRAM
	IOLog("OpenPMUXPRAMController::start() XPRAM is: \n");

	bp = &nvramBuf[kOWXPRAMPartitionOffset];
	for (i = 0; i < 0x100; i++)
		IOLog("%s %02X", (i & 0xF) ? "" : "\n", *bp++);
	#endif
	
	IOSleep(10000);

	return super::start(provider);
}

bool OpenPMUXPRAMController::writeOutXPRAM()
{
	if (openPMUDriver->writeXPRAM(0, &nvramBuf[kOWXPRAMPartitionOffset], 0x55) != kIOReturnSuccess)
		return false;
	if (openPMUDriver->writeXPRAM(0x55, &nvramBuf[kOWXPRAMPartitionOffset + 0x55], 0x55) != kIOReturnSuccess)
		return false;
	 return openPMUDriver->writeXPRAM(0xAA, &nvramBuf[kOWXPRAMPartitionOffset + 0xAA], 0x56)
			== kIOReturnSuccess;
}

void OpenPMUXPRAMController::sync()
{
	Verbose_IOLog("OpenPMUXPRAMController::sync() entered");
	if (!writeOutXPRAM()) {
		Verbose_IOLog("OpenPMUXPRAMController::sync() failed to write out XPRAM, retrying");
		if (!writeOutXPRAM()) {
			Verbose_IOLog("OpenPMUXPRAMController::sync() failed to write out XPRAM again, giving up");		
		}
	}
	Verbose_IOLog("OpenPMUXPRAMController::sync() exited");
}

IOReturn OpenPMUXPRAMController::read(IOByteCount offset, UInt8 *buffer, IOByteCount length)
{
	if (((offset + length) > kNVRAMSize) || (buffer == NULL)) {
		Verbose_IOLog("OpenPMUXPRAMController::read() bad args");
		return kIOReturnBadArgument;
	}

	bcopy(&nvramBuf[offset], buffer, length);
	
	return kIOReturnSuccess;
}

IOReturn OpenPMUXPRAMController::write(IOByteCount offset, UInt8 *buffer, IOByteCount length)
{
	if (((offset + length) > kNVRAMSize) || (buffer == NULL)) {
		Verbose_IOLog("OpenPMUXPRAMController::write() bad args");
		return kIOReturnBadArgument;
	}

	bcopy(buffer, &nvramBuf[offset], length);

	return kIOReturnSuccess;
}
