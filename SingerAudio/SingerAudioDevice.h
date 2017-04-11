#ifndef __SINGER_AUDIO_DEVICE_H
#define __SINGER_AUDIO_DEVICE_H 1

#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/IOLib.h>
#include "SingerAudioDefines.h"


#define SingerAudioDevice com_pac_driver_SingerAudioDevice

class SingerAudioDevice : public IOAudioDevice
{
	OSDeclareDefaultStructors(SingerAudioDevice);

public:
	IOService *myProvider;
	IOMemoryMap *regsMap;
	void *singerRegs;
	
	unsigned char gVolLeft, gVolRight;
	
	bool initHardware(IOService *provider);
	void free();

	bool createAudioEngine();
	UInt8 VolumeToPRAMValue(UInt32 leftVol, UInt32 rightVol);
	void WritePRAMVol(UInt32 leftVol, UInt32 rightVol);
	IOReturn volumeChangeHandler(IOAudioControl *volumeControl, SInt32 oldValue, SInt32 newValue);
	IOReturn outputMuteChangeHandler(IOAudioControl *muteControl, SInt32 oldValue, SInt32 newValue);
	unsigned int convertLocalVolToHW(unsigned int localVol);
        void initSinger();
};

#endif  /* __SINGER_AUDIO_DEVICE_H */