#include "SingerAudioDefines.h"
#include "SingerAudioDevice.h"
#include "SingerAudioEngine.h"

#include <IOKit/IOLib.h>

// How many frames we give to Singer every interrupt.
#define SAMPLE_FRAMES_PER_INTERRUPT	0x300

// Frames per buffer; must be a multiple of SAMPLE_FRAMES_PER_INTERRUPT
#define NUM_SAMPLE_FRAMES		0x1E00

#define NUM_CHANNELS			2
#define BIT_DEPTH			16

#define BYTES_PER_FRAME		(NUM_CHANNELS * BIT_DEPTH / 8)
#define BUFFER_SIZE		(NUM_SAMPLE_FRAMES * BYTES_PER_FRAME)

#define super IOAudioEngine

OSDefineMetaClassAndStructors(SingerAudioEngine, IOAudioEngine)

bool SingerAudioEngine::init(SingerAudioDevice *dev)
{
    bool result = false;
    
    VerboseIOLog("SingerAudioEngine[%p]::init(%p)\n", this, dev);

    if (!dev) {
	VerboseIOLog("init: NULL SingerAudioDevice\n");
        goto Done;
    }

    if (!super::init(NULL)) {
	VerboseIOLog("init: super init failed\n");
        goto Done;
    }
    
	audDev = dev;

	// We're guaranteed that singerRegs is always
	//  accessible, because SingerAudioDevice doesn't free the
	//  register mapping until its free() routine, and, as
	//  its client, we'll be destroyed by then.

	singerRegs = dev->singerRegs;

	if (!singerRegs) {
		VerboseIOLog("init: NULL registers\n");
		goto Done;
	}

    result = true;

Done:

    return result;
}

bool SingerAudioEngine::initHardware(IOService *provider)
{
    bool result = false;
    IOAudioSampleRate initialSampleRate;
    IOAudioStream *audioStream;
    IOPhysicalAddress fifoBufP;

    VerboseIOLog("SingerAudioEngine[%p]::initHardware(%p)\n", this, provider);
    
    if (!super::initHardware(provider)) {
		VerboseIOLog("initHW: super initHW failed, returning false\n");
        goto Done;
    }

    	setDescription("Singer2 Audio Engine");

	// We must have an entire frameset ready ahead of time.
	setSampleOffset(SAMPLE_FRAMES_PER_INTERRUPT);
    
	// Latency?
	// setOutputSampleLatency(

	// Setup the initial sample rate for the audio engine
	initialSampleRate.whole = 44100;
	initialSampleRate.fraction = 0;
        setSampleRate(&initialSampleRate);
    
	// Set the number of sample frames in each buffer
	setNumSampleFramesPerBuffer(NUM_SAMPLE_FRAMES);


	// Create an interrupt event source through which to receive interrupt callbacks
	// Unlike the sample code, we register a primary interrupt handler directly.

	if (audDev->getProvider()->registerInterrupt(0, this, (IOInterruptAction) &SingerAudioEngine::interruptHandler)
			!= kIOReturnSuccess)
	        goto Done;
    
    soundBuffer = (SInt16 *) IOMalloc(BUFFER_SIZE);
    if (!soundBuffer) {
        goto Done;
    }
        
    // Create an IOAudioStream for each buffer and add it to this audio engine
    audioStream = createNewAudioStream(kIOAudioStreamDirectionOutput, soundBuffer, BUFFER_SIZE);
    if (!audioStream) {
        goto Done;
    }
    
    addAudioStream(audioStream);
    audioStream->release();

	// Allocate 2 consecutive pages of memory on a 2-page boundary.
	//  This is where data stuffed into the FIFOs is stored by Singer.
	fifoBuf = (char *) IOMallocContiguous(FIFO_BUF_SIZE, FIFO_BUF_ALIGN, &fifoBufP);
	if (!fifoBuf) {
		VerboseIOLog("initHardware: could not allocate FIFO buffer, failing\n");
		goto Done;
	}

	fifoBufPhys(singerRegs) = (void *) fifoBufP;

        // Now that the FIFO is set up, initialize the hardware.
        audDev->initSinger();

	result = true;
Done:

    return result;
}

void SingerAudioEngine::free()
{  
	if (soundBuffer) {
        	IOFree(soundBuffer, BUFFER_SIZE);
	        soundBuffer = NULL;
	}
    
	if (fifoBuf) {
		IOFreeContiguous(fifoBuf, FIFO_BUF_SIZE);
		fifoBuf = NULL;
	}
    
	super::free();
}

IOAudioStream *SingerAudioEngine::createNewAudioStream(IOAudioStreamDirection direction, void *sampleBuffer, UInt32 sampleBufferSize)
{
    IOAudioStream *audioStream;
    
    audioStream = new IOAudioStream;
    if (audioStream) {
        if (!audioStream->initWithAudioEngine(this, direction, 1)) {
            audioStream->release();
        } else {
            IOAudioSampleRate rate;
            IOAudioStreamFormat format = {
                2,						// num channels
                kIOAudioStreamSampleFormatLinearPCM,		// sample format
                kIOAudioStreamNumericRepresentationSignedInt,	// numeric format
                BIT_DEPTH,										// bit depth
                BIT_DEPTH,										// bit width
                kIOAudioStreamAlignmentHighByte,				// high byte aligned - unused because bit depth == bit width
                kIOAudioStreamByteOrderBigEndian,				// big endian
                true,											// format is mixable
                0												// driver-defined tag - unused by this driver
            };
            
            // As part of creating a new IOAudioStream, its sample buffer needs to be set
            // It will automatically create a mix buffer should it be needed
            audioStream->setSampleBuffer(sampleBuffer, sampleBufferSize);
            
		// Singer can do 11kHz, 22kHz, and 44kHz. Let's only worry about 44kHz.
            rate.fraction = 0;
            rate.whole = 44100;
            audioStream->addAvailableFormat(&format, &rate, &rate);
            
            // Finally, the IOAudioStream's current format needs to be indicated
            audioStream->setFormat(&format);
        }
    }
    
    return audioStream;
}

void SingerAudioEngine::stop(IOService *provider)
{
    VerboseIOLog("SingerAudioEngine[%p]::stop(%p)\n", this, provider);
    
	// Unregister the interrupt since we no longer need it
	if (kIOReturnSuccess != audDev->getProvider()->unregisterInterrupt(0)) {
		// It's OK; maybe the interrupt was never registered.
		VerboseIOLog("SingerAudioEngine::stop could not unregister interrupt, continuing\n");
	}
    
    super::stop(provider);
}

IOReturn SingerAudioEngine::performAudioEngineStart()
{
	VerboseIOLog("SingerAudioEngine[%p]::performAudioEngineStart()\n", this);
    
	// First time through sound buffer
	firstLoop = true;
	bfrOffset = 0;

	// Enable the output interrupt
	outIntEnable(singerRegs) = 0;

	// Enable the Singer interrupt in software and wait for it to happen.
	audDev->getProvider()->enableInterrupt(0);

	// We don't take a timestamp because the interrupt handler will do that for
	//  us.
	    
	return kIOReturnSuccess;
}

IOReturn SingerAudioEngine::performAudioEngineStop()
{
	VerboseIOLog("SingerAudioEngine[%p]::performAudioEngineStop()\n", this);
    
	// Disable the interrupt in software
	audDev->getProvider()->disableInterrupt(0);

	// Disable the output interrupt
	outIntEnable(singerRegs) = 1;

	return kIOReturnSuccess;
}
    
UInt32 SingerAudioEngine::getCurrentSampleFrame()
{
    // We're safely past this point:
    return bfrOffset - (SAMPLE_FRAMES_PER_INTERRUPT * BYTES_PER_FRAME);
}

IOReturn SingerAudioEngine::interruptHandler(void * /*refCon*/, IOService * /*nub*/, int /*source*/)
{
    // This is a primary handler. Be careful!

    if ((r0x800(singerRegs) & 0xF0 == 0xB0) && (r0x804(singerRegs) & r0x804_fifoOutEmpty))
        return fifoInterrupt();
    
    return kIOReturnSuccess;
}

IOReturn SingerAudioEngine::fifoInterrupt()
{
    UInt32 i;
    register volatile UInt32 *fifos = &lrFifo16(singerRegs);
    UInt32 *sbPtr;

    // Disable output interrupt
    outIntEnable(singerRegs) = 1;

    // We're about to read from the beginning of the buffer. Take a timestamp, and update the
    //  loop counter unless this is our first time through.
    if (bfrOffset == 0)
        takeTimeStamp(!firstLoop);

    firstLoop = false;

    sbPtr = (UInt32 *) (bfrOffset + ((volatile char *) soundBuffer));

    for (i = 0; i < (SAMPLE_FRAMES_PER_INTERRUPT >> 2); i++) {
        // We stuff left and right FIFOs at once
        *fifos = sbPtr[0];
        *fifos = sbPtr[1];
        *fifos = sbPtr[2];
        *fifos = sbPtr[3];
        sbPtr += 4;
    }

    bfrOffset += (SAMPLE_FRAMES_PER_INTERRUPT * BYTES_PER_FRAME);
    if (bfrOffset >= BUFFER_SIZE) {
        bfrOffset = 0;
    }
    
    // Renable output interrupt
    outIntEnable(singerRegs) = 0;
    
    return kIOReturnSuccess;
}
