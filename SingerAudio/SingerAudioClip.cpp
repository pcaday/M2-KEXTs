#include "SingerAudioEngine.h"
#include "PCMBlitterLibPPC.h"


IOReturn SingerAudioEngine::clipOutputSamples(const void *mixBuf, void *sampleBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream)
{
	Float32ToNativeInt16(&(((float *) mixBuf)[firstSampleFrame]), &(((int16_t *) sampleBuf)[firstSampleFrame]), numSampleFrames * streamFormat->fNumChannels);

	return kIOReturnSuccess;
}

IOReturn SingerAudioEngine::convertInputSamples(const void *sampleBuf, void *destBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat, IOAudioStream *audioStream)
{
	return kIOReturnSuccess;
}