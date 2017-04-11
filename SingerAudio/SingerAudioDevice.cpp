#include "SingerAudioDevice.h"
#include "SingerAudioEngine.h"
#include "IOKit/IOPlatformExpert.h"

#define super IOAudioDevice

OSDefineMetaClassAndStructors(SingerAudioDevice, IOAudioDevice)

/* from AppleOnboardAudio/AudioHardwareConstants.h */

enum {
    kMaximumPRAMVolume 	= 7,
    kMinimumPRAMVolume	= 0,
    KNumPramVolumeSteps	= (kMaximumPRAMVolume- kMinimumPRAMVolume+1),
    kPRamVolumeAddr	= 8,
};

/* end from AppleOnboardAudio/AudioHardwareConstants.h */

#pragma mark * High-Level Interface

bool SingerAudioDevice::initHardware(IOService *provider)
{
	VerboseIOLog("SingerAudioDevice::initHardware()\n");
    
	if (!super::initHardware(provider)) {
		VerboseIOLog("initHardware: super init failed\n");
		return false;
	}
    
	myProvider = provider;
	
	setDeviceName("Built-in audio controller");
	setDeviceShortName("Built-in");
	setManufacturerName("Apple");
	
	if (!(regsMap = myProvider->mapDeviceMemoryWithIndex(0))) {
		VerboseIOLog("initHardware: could not map registers, failing\n");
		return false;
	}
	
	if (!(singerRegs = (void *) regsMap->getVirtualAddress())) {
		VerboseIOLog("initHardware: NULL virtual address for registers, failing\n");
		return false;
	}

	VerboseIOLog("initHardware: calling createAudioEngine\n");
	return createAudioEngine();
}

void SingerAudioDevice::free()
{
	// Freeing the regsMap here guarantees that the registers are always accessible by SingerAudioEngine
	if (regsMap)
		regsMap->release();
	
	super::free();
}

bool SingerAudioDevice::createAudioEngine()
{
    bool result = false;
    SingerAudioEngine *audioEngine = NULL;
    IOAudioControl *control;
    
    VerboseIOLog("SingerAudioDevice::createAudioEngine\n");
    
    audioEngine = new SingerAudioEngine;

    if (!audioEngine->init(this))
		goto Done;
    
    // Once each control is added to the audio engine, they should be released
    // so that when the audio engine is done with them, they get freed properly
    control = IOAudioLevelControl::createVolumeControl(singerMaxVolumeLocal,	// Initial value
                                                        singerMinVolumeLocal,	// min value
                                                        singerMaxVolumeLocal,	// max value
                                                        (-22 << 16) + (32768),	// -22.5 in IOFixed (16.16) -- I don't know if this is right for Singer2, check the ASCO 3555O data sheet
                                                        0,		// max 0.0 in IOFixed
                                                        kIOAudioControlChannelIDDefaultLeft,
                                                        kIOAudioControlChannelNameLeft,
                                                        0,		// control ID - driver-defined
                                                        kIOAudioControlUsageOutput);
    if (!control) {
        goto Done;
    }
    
	gVolLeft = singerMaxVolumeLocal;

    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler) &SingerAudioDevice::volumeChangeHandler, this);
    audioEngine->addDefaultAudioControl(control);
    control->release();			// We won't use this anymore (see comment above)
    
    control = IOAudioLevelControl::createVolumeControl(singerMaxVolumeLocal,	// Initial value
                                                        singerMinVolumeLocal,	// min value
                                                        singerMaxVolumeLocal,	// max value
                                                        (-22 << 16) + (32768),	// min -22.5 in IOFixed (16.16)
                                                        0,		// max 0.0 in IOFixed
                                                        kIOAudioControlChannelIDDefaultRight,	// Affects right channel
                                                        kIOAudioControlChannelNameRight,
                                                        0,		// control ID - driver-defined
                                                        kIOAudioControlUsageOutput);
    if (!control) {
        goto Done;
    }
        
	gVolRight = singerMaxVolumeLocal;

    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler) &SingerAudioDevice::volumeChangeHandler, this);
    audioEngine->addDefaultAudioControl(control);
    control->release();			// We won't use this anymore (see comment above)

    // Create an output mute control
    control = IOAudioToggleControl::createMuteControl(false,	// initial state - unmuted
                                                        kIOAudioControlChannelIDAll,	// Affects all channels
                                                        kIOAudioControlChannelNameAll,
                                                        0,		// control ID - driver-defined
                                                        kIOAudioControlUsageOutput);
                                
    if (!control) {
        goto Done;
    }
        
    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler) &SingerAudioDevice::outputMuteChangeHandler, this);
    audioEngine->addDefaultAudioControl(control);
    control->release();			// We won't use this anymore (see comment above)

    // Active the audio engine - this will cause the audio engine to have start() and initHardware() called on it
    // After this function returns, that audio engine should be ready to begin vending audio services to the system
    activateAudioEngine(audioEngine);
    // Once the audio engine has been activated, release it so that when the driver gets terminated,
    // it gets freed
    audioEngine->release();
    
    result = true;
    
Done:

    if (!result && (audioEngine != NULL)) {
        audioEngine->release();
    }

    return result;
}

/* HW access */
#pragma mark * HW Access

/** PRAM code very slightly modified from AppleOWScreamerAudio.cpp **/

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Calculates the PRAM volume value for stereo volume.
UInt8 SingerAudioDevice::VolumeToPRAMValue( UInt32 leftVol, UInt32 rightVol )
{ 
	UInt32			pramVolume;						// Volume level to store in PRAM
	UInt32 			averageVolume;					// summed volume
    const UInt32 	volumeRange = (singerMaxVolumeLocal - singerMinVolumeLocal + 1);     UInt32 			volumeSteps;
    
	averageVolume = leftVol + rightVol;		// sum the channel volumes and get an average
    volumeSteps = (volumeRange << 1) / kMaximumPRAMVolume;	// divide the range by the range of the pramVolume
    pramVolume = averageVolume / volumeSteps;
    
	// Since the volume level in PRAM is only worth three bits,
	// we round small values up to 1. This avoids SysBeep from
	// flashing the menu bar when it thinks sound is off and
	// should really just be very quiet.

	if ((pramVolume == 0) && (leftVol != 0 || rightVol !=0 ))
		pramVolume = 1;
		
	return (pramVolume & 0x07);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Writes the given unsignedfixed volume out to PRAM.
void SingerAudioDevice::WritePRAMVol(  UInt32 leftVol, UInt32 rightVol  )
{
	UInt8				pramVolume;
	UInt8 				curPRAMVol;
	IODTPlatformExpert * 		platform = NULL;
		
	platform = OSDynamicCast(IODTPlatformExpert,getPlatform());
        	



        if (platform)
	{
		pramVolume = VolumeToPRAMValue(leftVol,rightVol);
		
		// get the old value to compare it with
		platform->readXPRAM((IOByteCount)kPRamVolumeAddr,&curPRAMVol, (IOByteCount)1);
		
		
                    // Update only if there is a change
		if (pramVolume != (curPRAMVol & 0x07))
		{
			// clear bottom 3 bits of volume control byte from PRAM low memory image
			curPRAMVol = (curPRAMVol & 0xF8) | pramVolume;
			
			
			// write out the volume control byte to PRAM
			platform->writeXPRAM((IOByteCount)kPRamVolumeAddr, &curPRAMVol,(IOByteCount) 1);
		}
	}
}
/** end from AppleOWScreamerAudio.cpp **/

IOReturn SingerAudioDevice::volumeChangeHandler(IOAudioControl *volumeControl, SInt32 oldValue, SInt32 newValue)
{
	UInt16 a;

	VerboseIOLog("SingerAudioDevice::volumeChangeHandler(%p, %d, %d)\n", volumeControl, (int) oldValue, (int) newValue);
    
	if (!volumeControl) {
		VerboseIOLog("volumeChanged: NULL volumeControl, failing\n");
		return kIOReturnBadArgument;
	}

        VerboseIOLog("\t-> Channel %ld\n", volumeControl->getChannelID());

	switch (volumeControl->getChannelID()) {
		case kIOAudioControlChannelIDDefaultLeft:
			a = auxDataB(singerRegs);
			a &= ~auxDataB_leftVolMask;
			a |= convertLocalVolToHW(newValue) << auxDataB_leftVolShift;
			auxDataB(singerRegs) = a;

			gVolLeft = newValue;
			WritePRAMVol(gVolLeft, gVolRight);
			break;
		case kIOAudioControlChannelIDDefaultRight:
			a = auxDataB(singerRegs);
			a &= ~auxDataB_rightVolMask;
			a |= convertLocalVolToHW(newValue) << auxDataB_rightVolShift;
			auxDataB(singerRegs) = a;

			gVolRight = newValue;
			WritePRAMVol(gVolLeft, gVolRight);
			break;
		default:
			VerboseIOLog("volumeChanged: bad channel ID, failing\n");
			return kIOReturnBadArgument;
	}
    
	return kIOReturnSuccess;
}

IOReturn SingerAudioDevice::outputMuteChangeHandler(IOAudioControl *muteControl, SInt32 oldValue, SInt32 newValue)
{
	VerboseIOLog("SingerAudioDevice::outputMuteChangeHandler(%ld, %ld)\n", oldValue, newValue);
    
	if (!muteControl) {
		VerboseIOLog("outputMuteChangeHandler: NULL muteControl, failing\n");
		return kIOReturnBadArgument;
	}

	if (newValue != oldValue) {
		if (newValue)
			auxDataA(singerRegs) |= auxDataA_muteMask;
		else
			auxDataA(singerRegs) &= ~auxDataA_muteMask;
		
		if (newValue)
			WritePRAMVol(gVolLeft, gVolRight);
		else
			WritePRAMVol(0, 0);
	} else
		VerboseIOLog("outputMuteChangeHandler: no change\n");

	return kIOReturnSuccess;
}

unsigned int SingerAudioDevice::convertLocalVolToHW(unsigned int localVol)
{
	return 15 - localVol;		// For the ASCO, 0 is maximum volume and F is minimum volume
}

void SingerAudioDevice::initSinger()
{
	// Initialize the hardware, following the ROM (0x40010008)
	VerboseIOLog("initSinger: initializing Singer, a la ROM\n");

	auxDataB(singerRegs) = 0xFFC;
	r0xF40(singerRegs) = 0x20;

	r0x803(singerRegs) |= 0x80;
        OSSynchronizeIO();
	r0x803(singerRegs) &= 0x7F;

	lFifo16(singerRegs) = 0;
	rFifo16(singerRegs) = 0;

	auxDataA(singerRegs) = 0x400;

	inIntEnable(singerRegs) = 1;
	outIntEnable(singerRegs) = 1;

	r0x80A(singerRegs) = 0;
	r0x801(singerRegs) = 1;

	auxDataA(singerRegs) = 0x000;
	auxDataB(singerRegs) = 0x008;
        r0x806(singerRegs) = 0x80;

	VerboseIOLog("initSinger: <----\n");
}



#if 0
/* Utility */
// Couldn't get CF and I/O Kit to work together

static const char *SingerAudioDevice::LocalizedCString(CFStringRef *unlocalized)
{
	static char locStrBuf[0x200];
	CFStringRef *locStrCF;
	bool success;
	
	if (!unlocalized)
		return NULL;
	locStrCF = CFLocalizedString(unlocalized, "");
	if (!locStrCF)
		return NULL;
	success = CFStringGetCString(locStrCF, &locStrBuf[0], 0x200, CFStringGetSystemEncoding());
	
	return success ? &locStrBuf[0] : NULL;
}
#endif