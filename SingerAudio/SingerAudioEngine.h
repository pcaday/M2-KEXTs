#ifndef __SINGER_AUDIO_ENGINE_H
#define __SINGER_AUDIO_ENGINE_H 1

#include <IOKit/audio/IOAudioEngine.h>

#include "SingerAudioDevice.h"

#define SingerAudioEngine com_pac_driver_SingerAudioEngine

class SingerAudioEngine : public IOAudioEngine
{
    OSDeclareDefaultStructors(SingerAudioEngine)

public:
    void *singerRegs;

private:
    SingerAudioDevice *audDev;		// Parent SingerAudioDevice object
    SInt16 *soundBuffer;
    char *fifoBuf;
    bool firstLoop;
    UInt32 bfrOffset;
    
public:    
    virtual bool init(SingerAudioDevice *audDev);
    virtual void free();
    
    virtual bool initHardware(IOService *provider);
    virtual void stop(IOService *provider);
    
    virtual IOAudioStream *createNewAudioStream(IOAudioStreamDirection direction, void *sampleBuffer, UInt32 sampleBufferSize);

    virtual IOReturn performAudioEngineStart();
    virtual IOReturn performAudioEngineStop();
    
    virtual UInt32 getCurrentSampleFrame();
    
    virtual IOReturn performFormatChange(IOAudioStream *audioStream, const IOAudioStreamFormat *newFormat, const IOAudioSampleRate *newSampleRate);

    virtual IOReturn clipOutputSamples(const void *mixBuf, void *sampleBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream);
    virtual IOReturn convertInputSamples(const void *sampleBuf, void *destBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream);
    
private:
    IOReturn interruptHandler(void * /*refCon*/, IOService * /*nub*/, int /*source*/);
    IOReturn fifoInterrupt();
};

#endif  /* __SINGER_AUDIO_ENGINE_H */