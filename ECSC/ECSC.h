#ifndef __ECSC_H
#define __ECSC_H

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/IOService.h>


// Configuration options
#define ECSC_CACHE_VRAM 0			// 1 = enable caching of VRAM

typedef struct {
	UInt8 red;
	UInt8 green;
	UInt8 blue;
} ECSCCTEntry;

class ECSC : public IOFramebuffer
{
OSDeclareDefaultStructors(ECSC)
protected:
	volatile UInt8 *regs;
	IOIndex currentDepth;
	IOMemoryMap *regMap;
	IODeviceMemory *vramMem;
        IOService *provider;

	ECSCCTEntry clut[0x100];
	ECSCCTEntry gamma[0x100];
        
	bool haveCLUT, haveGamma, lcdEnabled;
        
        char lcdSave[0x1C];
        
public:
        virtual bool start(IOService *myProvider);
	virtual IOReturn enableController();
	virtual void free();

	virtual UInt32 getApertureSize();
	virtual IODeviceMemory *getApertureRange(IOPixelAperture aperture);
	virtual IODeviceMemory *getVRAMRange(void);
	virtual IOReturn getAttribute(IOSelect attribute, UInt32 *value);
        virtual IOReturn setAttribute(IOSelect attribute, UInt32 value);
	virtual IOReturn getCurrentDisplayMode (IODisplayModeID *displayMode, IOIndex *depth);
	virtual IOReturn getTimingInfoForDisplayMode (IODisplayModeID displayMode, IOTimingInformation *info);
	virtual IOItemCount getConnectionCount();
	virtual IOReturn setAttributeForConnection (IOIndex connectIndex, IOSelect attribute, UInt32 value);
	virtual IOReturn getAttributeForConnection (IOIndex connectIndex, IOSelect attribute, UInt32 *value);
	virtual IOReturn getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth);
	virtual const char *getPixelFormats();
	virtual IOItemCount getDisplayModeCount();
	virtual IOReturn getDisplayModes(IODisplayModeID *allDisplayModes);
	virtual IOReturn getInformationForDisplayMode(IODisplayModeID displayMode, IODisplayModeInformation *info);
	virtual UInt64 getPixelFormatsForDisplayMode(IODisplayModeID /* displayMode */, IOIndex /* depth */);
	virtual IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *info);
	virtual bool isConsoleDevice(void);
	virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount, UInt32 dataWidth, void *data);
	virtual IOReturn setCLUTWithEntries(IOColorEntry *colors, UInt32 index, UInt32 numEntries, IOOptionBits options);
	virtual IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth);
	virtual IOReturn getAppleSense(IOIndex connectIndex, UInt32 *senseType, UInt32 *primary, UInt32 *extended, UInt32 *displayType);
	virtual IOReturn connectFlags(IOIndex connectIndex, IODisplayModeID displayMode, IOOptionBits *flags);

#ifndef MAC_OS_10_1
        virtual IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whichDevice);
#endif

protected:
#if ECSC_CACHE_VRAM
	virtual void configureBAT();
#endif

        virtual IOReturn setPower(bool powerOn);
        
	virtual void set8Bit();
	virtual void set16Bit();
        virtual void setDepth(unsigned char hwDepthIndex);
        virtual void setBacklight(bool backlightOn);
        virtual void setLCDEnable(bool enable);
	virtual void setGammaAndCLUT();

        virtual void saveRegisters();
        virtual void restoreRegisters();
        
        virtual void waitVBL();
};

#endif  /* __ECSC_H */