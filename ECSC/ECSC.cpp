/*
 * Based off of OpenChips65550Framebuffer, by Ryan Rempel and Other World Computing
 */

#include <ppc/proc_reg.h>
#include "xnu-201.42.3_mem.h"
#include <IOKit/IOLib.h>
#include <IOKit/ndrvsupport/IOMacOSVideo.h>
#include "ECSC.h"

#define VERBOSE 1

#ifndef kIOVRAMSaveAttribute
	#define kIOVRAMSaveAttribute 'vrsv'		// IOGraphicsTypesPrivate.h
#endif

#if VERBOSE
    #define VerboseIOLog(x...) IOLog(x)
//    #define VerboseIOLog(x...) kprintf(x)
#else
    #define VerboseIOLog(x...) {}
#endif

// 10.0 headers don't include this
#ifndef kIOPMPowerStateVersion1
    #define kIOPMPowerStateVersion1 1
#endif

#define kIOFBGammaWidthKey	"IOFBGammaWidth"
#define kIOFBGammaCountKey	"IOFBGammaCount"


#define kDepth8Bit 0
#define kDepth16Bit 1
#define kDepthMax kDepth16Bit

#define ECSC_fb_width 800
#define ECSC_fb_height 600
#define ECSC_vram_base 0x60000000
#define ECSC_vram_size 0x00100000
#define ECSC_display_mode 1

#define ECSC_powerstate_off 0
#define ECSC_powerstate_on 1

// Pretty much everything here are guesses

// register numbers
#define BIT_DEPTH 0x12
// CLUT/gamma registers
#define CLUT_WINDEX 0x40
#define CLUT_DATA 0x42
#define CLUT_RINDEX 0x46


// register 0x8 bits
enum {
    r0x8_lcd_power = 0x8	// 1 = LCD on; 0 = LCD off  (power for the LCD itself, not the backlight)
};

// register 0x12 values
#define ECSC_depth_1bit 0
#define ECSC_depth_2bit 1
#define ECSC_depth_4bit 2
#define ECSC_depth_8bit 3
#define ECSC_depth_16bit 4

// register 0x14 bits
enum {
				// bit 0x8 can't be cleared
    r0x14_output_en = 0x4,	// 0 = video output disabled; 1 = video output enabled
    r0x14_vbl_int_flag = 0x2,	// Writing a 1 to this bit clears it; it is set when a "VBL" occurs
    r0x14_vbl_int_en = 0x1	// 1 = enable VBL interrupt, 0 = disable interrupt
};


#define super IOFramebuffer

OSDefineMetaClassAndStructors(ECSC, IOFramebuffer)

bool ECSC::start(IOService *myProvider)
{
    VerboseIOLog("ECSC::start()");
    provider = myProvider;
    
#if ECSC_CACHE_VRAM
    configureBAT();
#endif

    return super::start(myProvider);
}

IOReturn ECSC::enableController()
{
//	mach_timespec_t timeout;

	VerboseIOLog("ECSC::enableController()\n");
	
	(void) getVRAMRange();
	if (vramMem)
		vramMem->release();			// since getVRAMRange() does a retain()
	else
		IOLog("enableCtrllr: could not create vramMem\n");        
	
	regMap = provider->mapDeviceMemoryWithIndex(0);
	if (!regMap) {
		IOLog("ECSC: could not map in registers\n");
		return kIOReturnError;				// memory will be freed in stop()
	}
	regs = (volatile UInt8 *) regMap->getVirtualAddress();
	
	VerboseIOLog("enableCtrllr: regs @ %x, vram: [%x,%x]\n", (unsigned int) regs, ECSC_vram_base, ECSC_vram_size);
	
        setProperty(kIOFBGammaWidthKey, 8, 32);
        setProperty(kIOFBGammaCountKey, 256, 32);

	lcdEnabled = true;

	// Save off registers so that lcdSave is valid.
	saveRegisters();
	
	// Set up power management
	PMinit();

	IOPMPowerState powerStates [] = {
		{kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		{kIOPMPowerStateVersion1, IOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
	};
	
	registerPowerDriver(this, powerStates, 2);
	temporaryPowerClampOn();
	changePowerStateTo(ECSC_powerstate_on);
	
	provider->setProperty("IOPMIsPowerManaged", true);

	return kIOReturnSuccess;
}

void ECSC::free()
{
	VerboseIOLog("ECSC::free()\n");

	if (regMap)
		regMap->release();
	if (vramMem)
		vramMem->release();

	super::free();
}

#if ECSC_CACHE_VRAM
// If video memory is BAT-mapped (by BAT3), configure it to be mapped write-through
void ECSC::configureBAT()
{
	bat_t dbat3;
	boolean_t istate;
	
	mfdbatl(dbat3.lower.word, 3);

	VerboseIOLog("ECSC: configureBAT: DBAT3L is %x\n", dbat3.lower.word);
	
	if ((dbat3.lower.word & 0xFFFE0000) == ECSC_vram_base) {
		dbat3.lower.bits.wimg = 0xA;				// WiMg
		dbat3.lower.bits.pp = 0x2;
		
		// Cut interrupts while we manipulate processor/OS state
		istate = ml_set_interrupts_enabled(false);
		
		shadow_BAT.DBATs[3].lower = dbat3.lower.word;		// update shadow BAT also.
		sync();
		isync();
		mtdbatl(3, dbat3.lower.word);
		isync();
		
		(void) ml_set_interrupts_enabled(istate);
		
	}

	VerboseIOLog("ECSC: configureBAT: DBAT3L is now %x\n", dbat3.lower.word);
}
#endif

UInt32 ECSC::getApertureSize()
{
    UInt32 bpp;
 
    switch (currentDepth) {
	case kDepth8Bit:
	    bpp = 1;
	    break;
	case kDepth16Bit:
	    bpp = 2;
	    break;
	default:
	    return ECSC_vram_size;
    }

    return ECSC_fb_width * ECSC_fb_height * bpp;
}

IODeviceMemory *ECSC::getApertureRange(IOPixelAperture aperture)
{
	if (aperture == kIOFBSystemAperture)
		return IODeviceMemory::withRange(ECSC_vram_base, getApertureSize());
	else
		return NULL;
}

IODeviceMemory *ECSC::getVRAMRange(void)
{
	if (!vramMem)
		vramMem = IODeviceMemory::withRange(ECSC_vram_base, ECSC_vram_size);
	if (!vramMem)
		return NULL;

	vramMem->retain();
	
	return vramMem;
}

IOReturn ECSC::getAttribute(IOSelect attribute, UInt32 *value)
{
	VerboseIOLog("ECSC::getAttribute attribute = %u\n", (unsigned int) attribute);

	if (!value)
		return kIOReturnBadArgument;

	switch (attribute) {
		case kIOHardwareCursorAttribute:
			*value = false;
			break;
		
		case kIOPowerAttribute:
			*value = lcdEnabled != 0;
			break;
		    
		case kIOVRAMSaveAttribute:
			*value = true;
			break;
			
		default:
			return kIOReturnUnsupported;
			break;
	}
	
	return kIOReturnSuccess;
}

IOReturn ECSC::setAttribute(IOSelect attribute, UInt32 value)
{
	VerboseIOLog("ECSC::setAttribute attribute = %u, value = %u\n", (unsigned int) attribute, (unsigned int) value);
	
	if (attribute == kIOPowerAttribute)
	    return setPower(value);
	
	return super::setAttribute(attribute, value);
}

#ifndef MAC_OS_10_1
// The power-state changing interface for 10.0:
IOReturn ECSC::setPowerState(unsigned long powerStateOrdinal, IOService *whichDevice)
{
    IOReturn err;
    
    err = setPower(powerStateOrdinal != 0);
    if (err != kIOReturnSuccess)
	return err;
    
    return IOPMAckImplied;
}
#endif

/*
static void copy_nc(void *src, void *dst, unsigned int len)
{
    unsigned int i;
    unsigned long *s, *d;
    
    s = (unsigned long *) src;
    d = (unsigned long *) dst;
    
    for (i = 0; i < (len >> 2); i++)
	*d++ = *s++;
}

static void fill_nc(void *dst, unsigned int len, unsigned char fill)
{
    unsigned int i;
    unsigned char *d;
    unsigned long *l;
    
    d = (unsigned char *) dst;
    
    for (i = 0; i < len; i++)
	*d++ = fill;
}
*/

IOReturn ECSC::setPower(bool powerOn)
{
    if (lcdEnabled != powerOn) {
	#ifdef MAC_OS_10_1
	handleEvent(powerOn ? kIOFBNotifyWillPowerOn : kIOFBNotifyWillPowerOff);
	#endif
	
	if (powerOn) {
	    setLCDEnable(true);

	    // May need to reconfigure this...
//	    #if ECSC_CONFIGURE_BAT
//	    configureBAT();
//	    #endif
	    
//		IOSleep(50);
//		setBacklight(true);
	} else {
//		    setBacklight(false);
//		    IOSleep(50);

	    setLCDEnable(false);
	}

	#ifdef MAC_OS_10_1
	handleEvent(powerOn ? kIOFBNotifyDidPowerOn : kIOFBNotifyDidPowerOff);
	#endif
    }
    
    return kIOReturnSuccess;
}

IOReturn ECSC::getCurrentDisplayMode(IODisplayModeID *displayMode, IOIndex *depth)
{
    if (displayMode)
		*displayMode = ECSC_display_mode;
    if (depth)
		*depth = currentDepth;

    return kIOReturnSuccess;
}

IOReturn ECSC::getTimingInfoForDisplayMode(IODisplayModeID displayMode, IOTimingInformation *info)
{
	if (!info)
		return kIOReturnBadArgument;
	
	info->appleTimingID = timingApple_FixedRateLCD;
	info->flags = 0;
	
	return kIOReturnSuccess;
}

IOItemCount ECSC::getConnectionCount()
{
	return 1;
}

IOReturn ECSC::setAttributeForConnection (IOIndex connectIndex, IOSelect attribute, UInt32 value)
{
	VerboseIOLog("ECSC::setAttributeForConnection attribute = %u, value = %u\n", (unsigned int) attribute, (unsigned int) value);
	return kIOReturnUnsupported;
}

IOReturn ECSC::getAttributeForConnection (IOIndex connectIndex, IOSelect attribute, UInt32 *value)
{
	VerboseIOLog("ECSC::getAttributeForConnection attribute = %u\n", (unsigned int) attribute);
	switch (attribute) {
		case kConnectionFlags:
			if (!value) return kIOReturnBadArgument;
			*value = kIOConnectionBuiltIn;
			break;
			
		case kConnectionEnable:
			if (!value) return kIOReturnBadArgument;
			*value = true;
			break;
		
		case kConnectionSupportsAppleSense:
			return kIOReturnSuccess;
			break;
					
		default:
			return kIOReturnUnsupported;
			break;
	}
	
	return kIOReturnSuccess;
}

IOReturn ECSC::getStartupDisplayMode(IODisplayModeID *displayMode, IOIndex *depth)
{
	*displayMode = ECSC_display_mode;
	*depth = kDepth8Bit;
	
	return kIOReturnSuccess;
}

const char *ECSC::getPixelFormats()
{
    static const char *pixelFormats = IO8BitIndexedPixels "\0" IO16BitDirectPixels "\0\0";
	
    return pixelFormats;
}

IOItemCount ECSC::getDisplayModeCount()
{
	return 1;
}

IOReturn ECSC::getDisplayModes(IODisplayModeID *allDisplayModes)
{
	allDisplayModes[0] = ECSC_display_mode;
        return kIOReturnSuccess;
}

IOReturn ECSC::getInformationForDisplayMode(IODisplayModeID displayMode, IODisplayModeInformation *info)
{
	if (!info)
		return kIOReturnBadArgument;

	bzero(info, sizeof(IODisplayModeInformation));

	info->maxDepthIndex = kDepthMax;
	info->nominalWidth = ECSC_fb_width;
	info->nominalHeight = ECSC_fb_height;
	info->refreshRate = 0x0;

	return kIOReturnSuccess;
}

UInt64 ECSC::getPixelFormatsForDisplayMode(IODisplayModeID /* displayMode */, IOIndex /* depth */)
{
    return 0;
}

IOReturn ECSC::getPixelInformation(IODisplayModeID displayMode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *info)
{
	bzero(info, sizeof(IOPixelInformation));
	
	info->activeWidth = ECSC_fb_width;
	info->activeHeight = ECSC_fb_height;

    switch (depth) {
        case kDepth8Bit:
            strcpy(info->pixelFormat, IO8BitIndexedPixels);
            info->pixelType = kIOCLUTPixels;
            info->componentMasks[0] = 0xFF;
            info->bitsPerPixel = 8;
            info->componentCount = 1;
            info->bitsPerComponent = 8;
            info->bytesPerRow = 800;
            break;

        case kDepth16Bit:
            strcpy(info->pixelFormat, IO16BitDirectPixels);
            info->pixelType = kIORGBDirectPixels;
            info->componentMasks[0] = 0x7C00;
            info->componentMasks[1] = 0x03E0;
            info->componentMasks[2] = 0x001F;
            info->bitsPerPixel = 16;
            info->componentCount = 3;
            info->bitsPerComponent = 5;
            info->bytesPerRow = 1600;
            break;
	    
	default:
	    return kIOReturnUnsupported;
    }

    return kIOReturnSuccess;
}

bool ECSC::isConsoleDevice(void)
{
    return provider->getProperty("AAPL,boot-display");
}

IOReturn ECSC::setGammaTable(UInt32 channelCount, UInt32 dataCount, UInt32 dataWidth, void *data)
{
	VerboseIOLog("ECSC::setGammaTable()\n");
	
	if (dataCount != 0x100) {
		VerboseIOLog("ECSC::setGammaTable unsupported; dataCount: %lu dataWidth: %lu\n", dataCount, dataWidth);
		return kIOReturnUnsupported;
	};

	if ((channelCount != 3) && (channelCount != 1)) {
	    VerboseIOLog("ECSC::setGammaTable bad channelCount (%u)\n", (unsigned) channelCount);
	    return kIOReturnUnsupported;
	}

	if (dataWidth == 8) {
		UInt8 *gammaData8 = (UInt8 *) data;
			
		if (channelCount == 3) {
			for (int x = 0; x < 0x100; x++) {
				gamma[x].red = gammaData8[x];
				gamma[x].green = gammaData8[x + 0x100];
				gamma[x].blue = gammaData8[x + 0x200];
			}
		} else if (channelCount == 1) {
			for (int x = 0; x < 0x100; x++)
				gamma[x].red = gamma[x].green = gamma[x].blue = gammaData8[x];
		} else
			return kIOReturnUnsupported;
	} else if (dataWidth == 16) {
		UInt16 *gammaData16 = (UInt16 *) data;
		
		if (channelCount == 3) {
			for (int x = 0; x < 0x100; x++) {
				gamma[x].red = gammaData16[x] >> 8;
				gamma[x].green = gammaData16[x + 0x100] >> 8;
				gamma[x].blue = gammaData16[x + 0x200] >> 8;
			}
		} else if (channelCount == 1)
			for (int x = 0; x < 0x100; x++)
				gamma[x].red = gamma[x].green = gamma[x].blue = gammaData16[x] >> 8;
		else
			return kIOReturnUnsupported;
		
	} else {
		VerboseIOLog("ECSC::setGammaTable bad dataWidth (%u)\n", (unsigned) dataWidth);
		return kIOReturnUnsupported;
	}
	
	haveGamma = true;
	setGammaAndCLUT();
	
    return kIOReturnSuccess;
}

IOReturn ECSC::setCLUTWithEntries(IOColorEntry *colors, UInt32 index, UInt32 numEntries, IOOptionBits options)
{
	UInt32 dex, last, lumval;
	UInt32 x;
	bool setByValue = options & kSetCLUTByValue;
	bool setByLuminance = options & kSetCLUTWithLuminance;

//	VerboseIOLog("ECSC::setCLUTWithEntries, options = %x\n", (int) options);

        // if kSetCLUTImmediately is not set, we may want to wait for VBL
        //  This is a lengthy task (up to 16ms)

	last = setByValue ? numEntries : index + numEntries;
	
	for (x = setByValue ? 0 : index; x < last; x++) {
		dex = setByValue ? colors[x].index : x;
		if (dex < 0x100)
			if (setByLuminance) {
				lumval = ((UInt32) colors[x].red) * 3;
				lumval += ((UInt32) colors[x].green) * 4;
				lumval += ((UInt32) colors[x].blue) * 1;
				clut[dex].red = clut[dex].green = clut[dex].blue = lumval >> 11;
			} else {
				clut[dex].red = colors[x].red >> 8;
				clut[dex].green = colors[x].green >> 8;
				clut[dex].blue = colors[x].blue >> 8;
			}
	}
	
	haveCLUT = true;
	setGammaAndCLUT();

    return kIOReturnSuccess;	
}

IOReturn ECSC::setDisplayMode(IODisplayModeID displayMode, IOIndex depth)
{
	if (displayMode != ECSC_display_mode)
		return kIOReturnUnsupported;
	
	// Blank out the screen
//	bzero((void *) ECSC_vram_base, ECSC_vram_size);
	
	switch (depth) {
		case kDepth8Bit:
			set8Bit();
			break;
		case kDepth16Bit:
			set16Bit();
			break;
		default:
			return kIOReturnUnsupported;
	}
	
	currentDepth = depth;

	return kIOReturnSuccess;
}

IOReturn ECSC::getAppleSense(IOIndex connectIndex, UInt32 *senseType, UInt32 *primary, UInt32 *extended, UInt32 *displayType) 
{
	if (connectIndex != 0)
		return kIOReturnBadArgument;
	if (senseType)
		*senseType = 0;
	if (primary)
		*primary = 7;
	if (extended)
		*extended = 0;
	if (displayType)
		*displayType = kPanelConnect;

	return kIOReturnSuccess;
}

IOReturn ECSC::connectFlags(IOIndex connectIndex, IODisplayModeID displayMode, IOOptionBits *flags)
{
	if (flags)
		*flags = (displayMode == ECSC_display_mode) ? kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag : 0;

	return kIOReturnSuccess;
}

void ECSC::setBacklight(bool backlightOn)
{
    // This should be handled by AppleG3SeriesDisplay / ApplePMUPanel
    #if 0

    IOReturn ret;
    
    if (!pmuObject)
	return;
    
    ret = pmuObject->callPlatformFunction("setLCDPower", true, (void *) backlightOn, NULL, NULL, NULL);
    if (ret != kIOReturnSuccess)
	VerboseIOLog("ECSC::setBacklight pmuObject->setLCDPower() failed (%ud)\n", (unsigned int) ret);

    #endif
}

// HW interface

void ECSC::set8Bit()
{
    setDepth(ECSC_depth_8bit);
}

void ECSC::set16Bit()
{
    setDepth(ECSC_depth_16bit);
}

void ECSC::setDepth(unsigned char hwDepthIndex)
{
// Display_Video_Apple_CSC waits for VBL, but it doesn't seem necessary
// It makes things look worse rather than better!
//    waitVBL();
    
    regs[BIT_DEPTH] = hwDepthIndex;
    OSSynchronizeIO();
}

void ECSC::setLCDEnable(bool enable)
{
    // From Display_Video_Apple_CSC
    
    VerboseIOLog("ECSC::setLCDEnable, enable = %d\n", enable);
    
#if 1
    if (enable) {
	regs[0x44] = 0;		// ?
	
	restoreRegisters();
	
	setGammaAndCLUT();

	regs[0x44] = 0xFF;	// ?
	
	regs[0x14] |= r0x14_output_en;
	regs[0x8] |= r0x8_lcd_power;	
    } else {
	regs[CLUT_RINDEX] = 0;	// ?
	regs[CLUT_DATA] = 0x7F;	// ?
	regs[CLUT_DATA] = 0x7F;	// ?
	regs[CLUT_DATA] = 0x7F;	// ?
	
	regs[0x14] &= ~r0x14_output_en;
	IOSleep(50);
	waitVBL();
	regs[0x8] &= ~r0x8_lcd_power;

	saveRegisters();
    }
    
    lcdEnabled = enable;
#endif
}

void ECSC::saveRegisters()
{
    unsigned int i;
    char *lcdSavePtr = lcdSave;

    // From MacOS code
    
    VerboseIOLog("ECSC::saveRegisters()\n");

    for (i = 0x6; i <= 0x3C; i++)
	*lcdSavePtr++ = regs[i];
}

void ECSC::restoreRegisters()
{
    unsigned int i;
    char *lcdSavePtr = lcdSave;

    // From MacOS code
    
    VerboseIOLog("ECSC::restoreRegisters()\n");

    for (i = 0x6; i <= 0x38; i++)
		regs[i] = *lcdSavePtr++;
    
    regs[0x3C] = lcdSave[(0x3C - 0x6) >> 1];
}

void ECSC::setGammaAndCLUT()
{
	UInt32 i;
	
	VerboseIOLog("ECSC::setGammaAndClut()\n");
	
	switch (currentDepth) {
		case kDepth8Bit:
			if (!(haveGamma && haveCLUT)) return;
			regs[CLUT_WINDEX] = 0;
			for (i = 0; i < 0x100; i++) {
				regs[CLUT_DATA] = gamma[clut[i].red].red;
				regs[CLUT_DATA] = gamma[clut[i].green].green;
				regs[CLUT_DATA] = gamma[clut[i].blue].blue;
			}
			break;
			
		case kDepth16Bit:
			if (!haveGamma) return;

			regs[CLUT_WINDEX] = 0;
			for (i = 0; i < 0x100; i++) {
				regs[CLUT_DATA] = gamma[i].red;
				regs[CLUT_DATA] = gamma[i].green;
				regs[CLUT_DATA] = gamma[i].blue;
			}
			break;
	}
}

void ECSC::waitVBL()
{
    // Should occur after <= 16 2/3 ms
    #define MAX_WAITVBL_US 30000
    
    unsigned i;
    
    regs[0x14] |= r0x14_vbl_int_flag;
    
    for (i = 0; i < MAX_WAITVBL_US; i++) {
		if (regs[0x14] & r0x14_vbl_int_flag)
			return;
		IODelay(1);
    }
    
    return;
}