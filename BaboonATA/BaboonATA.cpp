#include "BaboonATA.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <ppc/proc_reg.h>

#define VERBOSE_BABOON 1
#define VERBOSE_BABOON_ATA 1

#if VERBOSE_BABOON
	#define Verbose_IOLog(x...) IOLog(x)
#else
	#define Verbose_IOLog(x...) { }
#endif

#ifdef BABOON_IC_LOG
// DEBUG
// Routine to write characters directly to console -- never disabled
extern "C" {
	extern void vc_putchar(char ch);		/* xnu/osfmk/ppc/POWERMAC/video_console.c in 10.0, 10.1 */
	extern void _disable_preemption(void);
	extern void _enable_preemption(void);
}

static void vc_putchar_intf(char ch)
{
	vc_putchar(ch);
	if (ch == '\n')
		vc_putchar('\r');
}

void BaboonICLog(const char *format, ...)
{
	va_list	listp;
	boolean_t state;

	state = ml_set_interrupts_enabled(FALSE);	
//	disable_preemption();
	
	va_start(listp, format);
	_doprnt(format, &listp, vc_putchar_intf, 16);
	va_end(listp);
	
//	enable_preemption();
	ml_set_interrupts_enabled(state);
}
#endif /* BABOON_IC_LOG */

//#undef Verbose_IOLog
//#define Verbose_IOLog(x...) BaboonICLog(x)

#define VTRED      "\033[31m"
#define VTRESET    "\033[0m"


// Offsets to Baboon registers

#define BABOON_DATA 0x0			// can be accessed as long, word, or byte
#define BABOON_FEATURE 0x4
#define BABOON_SCOUNT 0x8
#define BABOON_SNUM 0xC
#define BABOON_CYLLO 0x10
#define BABOON_CYLHI 0x14
#define BABOON_SDH 0x18
#define BABOON_STATUS 0x1C
#define BABOON_ALTSTATUS 0x38
#define BABOON_TIMING 0x40		// word

#define BABOON_VERSION1 (0x50 >> 1)	// word
#define BABOON_VERSION2 (0x54 >> 1)	// word

#define BABOON_CONTROLS (0xD0 >> 1)	// word
#define BABOON_INT_CAUSE (0xD4 >> 1)	// word (I think equal to +0xDC)
#define BABOON_INT_PENDING (0xD8 >> 1)	// word

// Number of channels per chip
#define baboonChannels 2

// +D0: Control / Interrupt Enable (read/write)
struct baboon_controls_reg {
	unsigned : 1;
	unsigned floppy_enabled : 1;
	unsigned ata1_enabled : 1;
	unsigned ata_enabled : 1;		// clearing this bit also clears ata1_en
	unsigned power_int_enabled : 1;
	unsigned mb_int_enabled : 1;
	unsigned ata0_int_enabled : 1;
	unsigned ata1_int_enabled : 1;
};

// +D4: Determines more precisely the reason for the last interrupt (read only)
struct int_cause_reg {
	unsigned : 4;
	unsigned power_on : 1;		// 0 = MB was powered off, 1 = MB was powered on
	unsigned mb_removed : 1;	// 0 = inserted, 1 = removed
	unsigned ata0_int : 1;
	unsigned ata1_int : 1;
};

// +D8:
// On read: the pending interrupts
// On write: acknowledge interrupts (see int_pending_reg_w)
// Note that the interrupt will continue to be signalled until acknowledged.
struct int_pending_reg_r {
	unsigned : 4;
	unsigned power_int : 1;
	unsigned mb_int : 1;
	unsigned ata0_int : 1;
	unsigned ata1_int : 1;
};
struct int_pending_reg_w {
	unsigned : 2;
	unsigned power_int_ack : 1;	// write zero to clear interrupt - same with all other ack bits
	unsigned mb_int_ack : 1;
	unsigned : 2;
	unsigned ata0_int_ack : 1;	// guess from ROM ATAManager
	unsigned ata1_int_ack : 1;	// guess from ROM ATAManager
};

// baboon_int_pending_ack: 
//   in: readVal: interrupts to ack
//  out: value to write to int pending register to acknowledge these interrupts
static inline UInt16 baboon_int_pending_ack(UInt16 readVal) { return ~(readVal | (readVal << 2)); }

enum {
	kPowerIntMask = 0x8,
	kMBIntMask = 0x4,
	kATA0IntMask = 0x2,
	kATA1IntMask = 0x1,

	baboonIntMasks = 0xF
};

enum {
	kPowerInt = 3,
	kMBInt = 2,
	kATA0Int = 1,
	kATA1Int = 0
};

enum {
	kATA1Enable = 0x20,
	kATAEnable = 0x10
};

// Values for mbLastIntKinds
enum {
	kPowerOnEvent = 8,
	kPowerOffEvent = 4,
	kMBInsertedEvent = 2,
	kMBRemovedEvent = 1
};

// Values returned by the PMU function readMBHWID (also see AppleMediaBay.h)
enum {
	deviceAutoFloppy = 0,
	deviceATA = 3,
	deviceNone = 7
};

// Guess at timing register format, based on later models:
// 0000rrrr|rraaaaaa
//  with fields
//     r = recovery
//     a = access
// guess: recovery time = (r * 20ns) + 40ns extra
// guess: access time = a * 40ns
//  (note 40ns is cycle time on the 1400's 25MHz I/O bus)
//
// Then:
//  PIO0(600ns) = 0x485 = 360+40rec + 200acc
//  PIO1(383ns) = 0x284 = 200+40rec + 160acc
//  PIO2(240ns) = 0x143 = 100+40rec + 120acc
//  PIO3(180ns) = 0x142 = 100+40rec + 80acc



#define super IOService

OSDefineMetaClassAndStructors(Baboon, IOService)

bool Baboon::start(IOService *provider)
{
	mach_timespec_t timeout;
	const OSSymbol *icName;
	IORegistryIterator *iter;
	IORegistryEntry *child;
	OSArray *controller;
	OSArray *specifier;
	OSData *tmpData;
	unsigned long tmpLong;
	
	Verbose_IOLog("Baboon::start() entered\n");
	
	if (!super::start(provider))
		return false;
	
	// Variable initialization
//	mbDriver = NULL;
//	mbHasPower = false;
//	mbATAPresent = false;
//	ata1Nub = NULL;
	baboonPower = true;

	// Create the command gate
	commandGate = IOCommandGate::commandGate(this);
	if (commandGate == NULL) {
		Verbose_IOLog("Baboon::start() could not create command gate\n");
		return false;
	}
	
	if (getWorkLoop()->addEventSource(commandGate) != kIOReturnSuccess) {
		Verbose_IOLog("Baboon::start() could not attach command gate to WL\n");
		return false;
	}
	
	// OSSymbols we use
	symReadMBHWID = OSSymbol::withCStringNoCopy("readMBHWID");
	symSetMediaBayPower = OSSymbol::withCStringNoCopy("setMediaBayPower");
	
	regsMap = provider->mapDeviceMemoryWithIndex(0);
	if (!regsMap) {
		Verbose_IOLog("Baboon: could not map registers\n");
		return false;
	}

	baboonRegs = (volatile UInt16 *) regsMap->getVirtualAddress();

	// Find the PMU, which we'll need to inquire about the media bay
	Verbose_IOLog("Baboon::start() waiting on PMU\n");

	timeout.tv_sec = 30;
	timeout.tv_nsec = 0;
	pmu = waitForService(serviceMatching("ApplePMU"), &timeout);
	
	if (!pmu) {
		Verbose_IOLog("Baboon::start() could not find PMU");
		return false;
	}

	// Create the media bay interrupt event source. BaboonInterruptController calls this
	//  when there is a media bay or power interrupt.
	mbIntSrc = IOInterruptEventSource::interruptEventSource((OSObject *) this, (IOInterruptEventAction) &Baboon::mediaBayInterrupt, NULL, 0); 

	if (!mbIntSrc || getWorkLoop()->addEventSource(mbIntSrc)) {
		Verbose_IOLog("Baboon: could not create/add media bay interrupt source\n");
		return false;
	}
	
	mbIntSrc->enable();

	// Set up controls register and clear any pending interrupts
	baboonRegs[BABOON_INT_PENDING] = 0;
	OSSynchronizeIO();
	baboonRegs[BABOON_CONTROLS] = kATAEnable;
	OSSynchronizeIO();
	
	// Create and initialize interrupt controller
	interruptController = new BaboonInterruptController;
	if (!interruptController)
		return false;

	if (interruptController->initInterruptController(provider, baboonRegs, mbIntSrc) != kIOReturnSuccess)
		return false;
	
	// set up interrupt controller to handle interrupts
	provider->registerInterrupt(0, interruptController, interruptController->getInterruptHandlerAddress(), 0);

	provider->enableInterrupt(0);

	icName = OSSymbol::withCString("BaboonInterruptController");
	
	// Register the interrupt controller so clients can find it.
	getPlatform()->registerInterruptController((OSSymbol *) icName, interruptController);
	
	// Add interrupt properties to child ATA nodes	
	// Create the interrupt controller array - same for all nodes
	controller = OSArray::withCapacity(1);
	if (!controller) {
		Verbose_IOLog("BaboonATA: could not create a property\n");
		return false;
	}
	controller->setObject(icName);

	iter = IORegistryIterator::iterateOver(provider, gIODTPlane);
	
	while (child = iter->getNextObject()) {
		tmpData = OSDynamicCast(OSData, child->getProperty("channel"));
		if (!tmpData)
			continue;
	
		// Create the interrupt specifer array.
		specifier = OSArray::withCapacity(1);
		if (!specifier) {
			Verbose_IOLog("BaboonATA: could not create a property\n");
			continue;
		}
					
		tmpLong = 1 - *((unsigned long *) tmpData->getBytesNoCopy());
		tmpData = OSData::withBytes(&tmpLong, sizeof(tmpLong));
		specifier->setObject(tmpData);
		
		// Put the two arrays into the property table.
		child->setProperty(gIOInterruptControllersKey, controller);
		child->setProperty(gIOInterruptSpecifiersKey, specifier);
		
		// Release the array after being added to the property table.
		specifier->release();
	}
	
	// Release some things that have been hanging around
	controller->release();
	icName->release();

	// Power management stuff
	static const IOPMPowerState myPowerStates[2] =
	{
		{ 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ 1, IOPMPowerOn, IOPMPowerOn, IOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
	};
	
	// initialize our PM superclass variables
	PMinit();
	// register as the controlling driver
	registerPowerDriver(this, (IOPMPowerState *) myPowerStates, 2);
	// add ourselves into the PM tree
	provider->joinPMtree(this);
	// set current pm state
	changePowerStateTo(1);

	// Now power down the media bay	
	if (pmu->callPlatformFunction(symSetMediaBayPower, false, (void *) false, NULL, NULL, NULL)
		!= kIOReturnSuccess) {
		Verbose_IOLog("Baboon::start() setMediaBayPower(false) returned error\n");
	}
	
	// Let the PMU power off
	IOSleep(100);
	
	// Now examine the media bay as if we received an insertion notification.
	//  At this point ata1Nub is NULL (because ata1Nub is set by registerATADriver, which cannot
	//  happen until the ATA1 driver finds us, which cannot happen before my registerService())
	//  so it won't call registerService() on the ata1 nub
	commandGate->runAction((IOCommandGate::Action) &Baboon::mbInsertion);

	registerService();
		
	Verbose_IOLog("Baboon::start() exited\n");

	return true;
}

void Baboon::free()
{
	Verbose_IOLog("Baboon::free() entered\n");

	PMstop();

	if (interruptController)
		interruptController->release();
	if (mbIntSrc)
		mbIntSrc->release();
	if (regsMap)
		regsMap->release();
	if (symReadMBHWID)
		symReadMBHWID->release();
	if (symSetMediaBayPower)
		symSetMediaBayPower->release();
		
	super::free();

	Verbose_IOLog("Baboon::free() exited\n");
}

IOWorkLoop *Baboon::getWorkLoop()
{
	if (!workLoop)
		workLoop = IOWorkLoop::workLoop();

	return workLoop;
}

bool Baboon::registerMBDriver(IOService *driver, const char *mbDevType)
{
	if (strcmp(mbDevType, "ata"))		// We only deal with ATA currently
		return false;

	if (driver) {
		if (!ata1Nub) {
			ata1Nub = driver->getProvider();	// Get the ATA1 nub so we can attach/detach drivers
								//  upon insertion or removal
		}
		
		if (!mbATAPresent)			// Don't let driver start if no ATA device present
			return false;
		
		if (!waitForMBPower())
			return false;			// If no power, device disappeared or something-- don't attach
	}
	
	mbDriver = driver;		// All set!
	
	return true;
}

void Baboon::mediaBayInterrupt(IOInterruptEventSource *evtSrc, int count)
{
	UInt16 intKinds;
	
	IOSleep(50);		// Wait for Baboon to figure out what's happening.
				// The interrupt signals are not debounced (enough),
				//  so that upon media bay device insertion, say,
				//  we may get insert, then remove, then insert interrupts
				// Similarly, when the media bay device is removed, we get
				//  a power-on, then a power-off interrupt (consistently)
	
	interruptController->disableVectorHard(kMBInt, NULL);	// disables both MB and power interrupts.
	intKinds = interruptController->mbLastIntKinds;
	interruptController->mbLastIntKinds = 0;
	interruptController->enableVector(kMBInt, NULL);
	
	Verbose_IOLog("Baboon::mediaBayInterrupt() entered, reasons = %x\n", intKinds);
	
	if (intKinds & kMBRemovedEvent) {
		// The media bay device was removed by the user. The PMU has automatically turned off
		//  media bay power and Baboon has disabled ATA1, so we only need to terminate
		//  the ATA1 driver.
		
		mbRemoval();
		return;		// Do not process any other events -- they could be stale
	} else if (intKinds & kMBInsertedEvent) {
		// A media bay device inserted. We must ask the PMU for the media bay ID, then
		//  power on the media bay and enable ATA1 if an ATA device is in the media bay

		mbInsertion();
	}
	
	if (intKinds & kPowerOnEvent)
		mbGotPower();
	else if (intKinds & kPowerOffEvent) {
		mbHasPower = false;
		mbATAPresent = false;
	}
}

// Handle a media bay device removal. Includes dummy arguments and return value so it can be called by the
//  command gate.
IOReturn Baboon::mbRemoval(void *arg0 = NULL, void *arg1 = NULL, void *arg2 = NULL, void *arg3 = NULL)
{
	Verbose_IOLog("Baboon::mbRemoval() entered\n");
	mbATAPresent = false;
	mbHasPower = false;
	
	if (mbDriver) {
		Verbose_IOLog("Baboon::mbRemoval() sending removal message\n");
		if (messageClient(kATARemovedEvent, mbDriver,
					(void *) OSString::withCString(kATAMediaBaySocketString), 0)
				!= kIOReturnSuccess) {
			Verbose_IOLog("Baboon::mbRemoval() could not send removal message, continuing\n");
		}

		mbDriver = NULL;
	}
	
	return kIOReturnSuccess;
}

// Handle a media bay device insertion
IOReturn Baboon::mbInsertion(void *arg0 = NULL, void *arg1 = NULL, void *arg2 = NULL, void *arg3 = NULL)
{
	UInt8 newMBID;

	Verbose_IOLog("Baboon::mbInsertion() entered\n");
	
	IOSleep(500);			// Wait for media bay to stabilize
	
	newMBID = readMBID();
	
	Verbose_IOLog("Baboon::mbInsertion() readMBHWID returned %d\n", newMBID);
	
	mbATAPresent = (newMBID == deviceATA);
	
	if (mbATAPresent) {
		if (mbHasPower)		// Already powered on?
			mbGotPower();
		else {
			if (pmu->callPlatformFunction(symSetMediaBayPower, false, (void *) true, NULL, NULL, NULL)
				!= kIOReturnSuccess) {
				Verbose_IOLog("Baboon::mbInsertion() setMediaBayPower returned error\n");
			}
			// When media bay powers on, we will get an interrupt.
		}
	}
	
	return kIOReturnSuccess;
}

// The media bay has been powered on -- enable ATA1 and it's ready to go.
IOReturn Baboon::mbGotPower(void *arg0 = NULL, void *arg1 = NULL, void *arg2 = NULL, void *arg3 = NULL)
{
	boolean_t interruptState;
	
	Verbose_IOLog("Baboon::mbGotPower() entered\n");
	
	// Turn off interrupts whenever we change the controls register
	interruptState = ml_set_interrupts_enabled(false);
	baboonRegs[BABOON_CONTROLS] |= kATA1Enable;
	(void) ml_set_interrupts_enabled(interruptState);

	OSSynchronizeIO();
	
	// Check that the enable succeeded. If not, the device probably disappeared.
	if (!(baboonRegs[BABOON_CONTROLS] & kATA1Enable)) {
		IOLog("Baboon: could not enable ATA1\n");
		mbATAPresent = false;
		return kIOReturnSuccess;
	}
	
	mbHasPower = true;
	
	// Get a driver attached, unless one is already
	if (ata1Nub && (!mbDriver))
		ata1Nub->registerService();
	
	Verbose_IOLog("Baboon::mbGotPower() exited\n");
	
	return kIOReturnSuccess;
}

UInt8 Baboon::readMBID()
{
	UInt8 newMBID, lastMBID;

	newMBID = 0xFF;			// Keep reading ID until we get the same value twice
	do {
		lastMBID = newMBID;
		if (pmu->callPlatformFunction(symReadMBHWID, true, &newMBID, NULL, NULL, NULL) != kIOReturnSuccess) {
			Verbose_IOLog("Baboon::mbInsertion() readMBHWID returned error\n");
			return deviceNone;
		}
	} while (lastMBID != newMBID);
	
	return newMBID;
}

void Baboon::setMBPower(bool powerOn)
{
	if (pmu->callPlatformFunction(symSetMediaBayPower, false, (void *) powerOn, NULL, NULL, NULL) != kIOReturnSuccess)
		Verbose_IOLog("Baboon::mbInsertion() error powering on/off media bay\n");
}

bool Baboon::waitForMBPower()
{
	unsigned int i;
	
	for (i = 0; i < 200; i++) {	// Wait up to 2s for media bay to power up
		if (mbHasPower)
			return true;
		IOSleep(10);
	}
		
	return false;
}

IOReturn Baboon::setPowerState(UInt32 newState, IOService *device)
{
	IOReturn retval = IOPMAckImplied;
	
	Verbose_IOLog("Baboon::setPowerState(%u)", newState);

	if (baboonPower != newState) {
		Verbose_IOLog("Baboon: setting power state to %u\n", newState);
		
		if (!newState) {
			savedBaboonCtrls = baboonRegs[BABOON_CONTROLS];
			OSSynchronizeIO();
			
			// Enable interrupts as necessary -- they often seem to be disabled at this point
			if (savedBaboonCtrls & kATAEnable) {
				savedBaboonCtrls |= kATA0IntMask;
				if (savedBaboonCtrls & kATA1Enable)
					savedBaboonCtrls |= kATA1IntMask;
			}
		} else {
			// Set this first so we get MB/power interrupts
			baboonRegs[BABOON_CONTROLS] = savedBaboonCtrls;
			OSSynchronizeIO();

#if 0
			// Do the rest on the workloop
			//  XXX will not move to another thread - use a different event source!
			commandGate->runAction((IOCommandGate::Action) &Baboon::powerBaboonOn);

			// Allow 11s to start up
			retval = 11000000;
#endif
		}
		
		baboonPower = newState;
	}

	return retval;
}

void Baboon::powerBaboonOn(void *arg0 = NULL, void *arg1 = NULL, void *arg2 = NULL, void *arg3 = NULL)
{
	if (mbATAPresent) {
		if (readMBID() == deviceATA) {
			// Note: OpenPMU turns on media bay power on wake for us
//			setMBPower(true);
//			waitForMBPower();
		} else
			mbRemoval();
	} else
		mbInsertion();
	
	acknowledgeSetPowerState();
}

void Baboon::dumpRegs()
{
	Verbose_IOLog("Baboon: control regs = %02X, %02X, %02X\n", baboonRegs[BABOON_CONTROLS],
		baboonRegs[BABOON_INT_CAUSE], baboonRegs[BABOON_INT_PENDING]);
}

// --------------- BaboonInterruptController -----------------

#undef super
#define super IOInterruptController

// Baboon has at least 4 different interrupts, each of which may be enabled or disabled.
// However, we provide vectors for only the ATA0 and ATA1 device interrupts.
//  The other interrupts are handled by a direct line to Baboon::mediaBayInterrupt.
#define kNumVectors 2
#define kNumInterrupts 4


OSDefineMetaClassAndStructors(BaboonInterruptController, IOInterruptController);

IOReturn BaboonInterruptController::initInterruptController(IOService *provider, volatile UInt16 *base, IOInterruptEventSource *mb)
{
	int cnt;
	
	mbIntSrc = mb;
	
	// Allocate the memory for the vectors
	vectors = (IOInterruptVector *) IOMalloc(kNumVectors * sizeof(IOInterruptVector));
	if (!vectors)
		return kIOReturnNoMemory;

	bzero(vectors, kNumVectors * sizeof(IOInterruptVector));
	
	// Allocate locks for the vectors
	for (cnt = 0; cnt < kNumVectors; cnt++) {
		vectors[cnt].interruptLock = IOLockAlloc();
		if (vectors[cnt].interruptLock == NULL) {
			for (cnt = 0; cnt < kNumVectors; cnt++)
				if (vectors[cnt].interruptLock != NULL)
					IOLockFree(vectors[cnt].interruptLock);
			return kIOReturnNoResources;
		}
	}

	// Setup the accessors for the registers
	bControls = (volatile UInt16 *)(base + BABOON_CONTROLS);
	bCause = (volatile UInt16 *)(base + BABOON_INT_CAUSE);
 	bPending = (volatile UInt16 *)(base + BABOON_INT_PENDING);

	// Set up HW now that accessors are initialized
	clearAllInterrupts();

	return kIOReturnSuccess;
}

void BaboonInterruptController::clearAllInterrupts(void)
{
	mbLastIntKinds = 0;
	
	*bPending = 0;
	OSSynchronizeIO();
	
	// Always enable power and media bay interrupts
	*bControls &= ~baboonIntMasks;
	OSSynchronizeIO();
	*bControls |= kPowerIntMask | kMBIntMask;
	OSSynchronizeIO();
}

// Must be compiled with -fpermissive
IOInterruptAction BaboonInterruptController::getInterruptHandlerAddress(void)
{
	return (IOInterruptAction) &BaboonInterruptController::handleInterrupt;
}

IOReturn BaboonInterruptController::handleInterrupt(void * /*refCon*/, IOService * /*nub*/, int /*source*/)
{
	UInt16 pending, causes;
	unsigned int vectorNumber;
	IOInterruptVector *vector;
	
	while (true) {
		pending = *bPending & *bControls & baboonIntMasks;
		OSSynchronizeIO();
		
		if (!pending)
			break;
		
		for (vectorNumber = 0; vectorNumber < kNumVectors; vectorNumber++) {
			if (pending & (1 << vectorNumber)) {
				vector = &vectors[vectorNumber];
				
				vector->interruptActive = 1;
				sync();
				isync();
				if (!vector->interruptDisabledSoft) {
					isync();
			
					// Call the handler if it exists.
					if (vector->interruptRegistered)
						vector->handler(vector->target, vector->refCon, vector->nub, vector->source);
				} else {
					// Hard disable the source.
					vector->interruptDisabledHard = 1;
					disableVectorHard(vectorNumber, vector);
				}
				
				vector->interruptActive = 0;
			}
		}

		if (pending & (kMBIntMask | kPowerIntMask)) {
			causes = *bCause;
			
			if (pending & kMBIntMask)
				if (causes & kMBIntMask) {
					mbLastIntKinds |= kMBRemovedEvent;
					mbLastIntKinds &= ~kMBInsertedEvent;
				} else {
					mbLastIntKinds |= kMBInsertedEvent;
					mbLastIntKinds &= ~kMBRemovedEvent;
				}
			
			if (pending & kPowerIntMask)
				if (causes & kPowerIntMask) {
					mbLastIntKinds |= kPowerOnEvent;
					mbLastIntKinds &= ~kPowerOffEvent;
				} else {
					mbLastIntKinds |= kPowerOffEvent;
					mbLastIntKinds &= ~kPowerOnEvent;
				}
			
			mbIntSrc->interruptOccurred(NULL, NULL, 0);
		}

		*bPending = baboon_int_pending_ack(pending);	// *bPending = 0;
		OSSynchronizeIO();
	}

	return kIOReturnSuccess;
}

bool BaboonInterruptController::vectorCanBeShared(long /*vectorNumber*/, IOInterruptVector */*vector*/)
{
	return true;
}

void BaboonInterruptController::disableVectorHard(long vectorNumber, IOInterruptVector */*vector*/)
{
	boolean_t interruptState;
	unsigned int x;
	
	if ((unsigned long) vectorNumber >= kNumInterrupts)
		return;
	
	if ((unsigned long) vectorNumber >= kNumVectors)
		x = kMBIntMask | kPowerIntMask;
	else
		x = (1 << vectorNumber);
	
	// heavy-handed but difficult to avoid since the Baboon interrupt is non-maskable
	interruptState = ml_set_interrupts_enabled(false);

	*bControls &= ~x;
	OSSynchronizeIO();

	(void) ml_set_interrupts_enabled(interruptState);
}

void BaboonInterruptController::enableVector(long vectorNumber, IOInterruptVector * /* vector */)
{
	boolean_t interruptState;
	unsigned int x;
	
	if ((unsigned long) vectorNumber >= kNumInterrupts)
		return;
	
	if ((unsigned long) vectorNumber >= kNumVectors)
		x = kMBIntMask | kPowerIntMask;
	else
		x = (1 << vectorNumber);

	
	// heavy-handed but difficult to avoid since the Baboon interrupt is non-maskable
	interruptState = ml_set_interrupts_enabled(false);

	*bControls |= x;
	OSSynchronizeIO();

	(void) ml_set_interrupts_enabled(interruptState);
}

int BaboonInterruptController::getVectorType(long /*vectorNumber*/, IOInterruptVector */*vector*/)
{
	return kIOInterruptTypeLevel;
}

// necessary?
#if 0
IOReturn BaboonInterruptController::getInterruptType(int source, int *interruptType)
{
	if ((unsigned) source >= kNumVectors)
		return kIOReturnBadArgument;
	
	*interruptType = getVectorType(source, NULL);
	
	return kIOReturnSuccess;
}
#endif

// --------------- BaboonATA -----------------

#undef super
#define super IOATAController

#undef Verbose_IOLog
#if VERBOSE_BABOON_ATA
	#define Verbose_IOLog(x...) IOLog(x)
#else
	#define Verbose_IOLog(x...)
#endif

// DEBUG
//#undef Verbose_IOLog
//#define Verbose_IOLog(x...) BaboonICLog(x)

// Baboon supports PIO0 - 4 nominally (according to Mac OS driver)
//  We'll allow PIO4 (even though timing no different from that for PIO3)
//  HeathrowATA, KeyLargoATA (33Mhz version) do this, and their PIO timing minimum is 300ns --
//  Ours is perhaps 220ns (this is just a guess).
#define baboon_pio_modes 0x1F


OSDefineMetaClassAndStructors(BaboonATA, IOATAController)

bool BaboonATA::start(IOService *provider)
{
	UInt32 i;
	ATADeviceNub *childNub = NULL;

	Verbose_IOLog("BaboonATA::start() entered\n");
	
	// Initialize the timing parameters to safe values for probing
	//  and in case getConfig() is called before selectConfig().	
	busTimings[0].pioTimerVal = busTimings[1].pioTimerVal = 0x0485;
	busTimings[0].pioMode = busTimings[1].pioMode = 0x01;			// Mode 0
	busTimings[0].pioCycleTime = busTimings[1].pioCycleTime = 600;

	busOnline = true;
	
	// For bookkeeping purposes interrupts initially disabled once
	intDisableCnt = 1;

	regsMem = provider->getDeviceMemoryWithIndex(0);
	if (!regsMem) {
		IOLog("BaboonATA: could not get memory\n");
		return false;
	}
	
	channel = (regsMem->getPhysicalAddress() & 0xFF) == 0x80;
	
	// Find and register with Baboon first to see if we should attach
	Verbose_IOLog("BaboonATA::start() waiting on Baboon\n");
	
	baboon = (Baboon *) waitForService(serviceMatching("Baboon", NULL));	// Should never hang -- if Baboon weren't present, we shouldn't be either (unless Baboon did not start)
	
	if (!baboon)
		return false;			// should not happen

	Verbose_IOLog("BaboonATA::start() found Baboon\n");
	
	if (channel == 1) {
		Verbose_IOLog("BaboonATA::start() media bay checks/initialization\n");
		if (!baboon->registerMBDriver(this, "ata")) {
			Verbose_IOLog("BaboonATA: Baboon said not to start\n");
			return false;
		}
		Verbose_IOLog("BaboonATA::start() waiting 10s for media bay device to initialize\n");
		for (i = 0; i < 10000; i++) {
			IOSleep(1);
			if (isInactive())	// If we were terminated while sleeping, don't continue
				return false;
		}
	}

	if (!super::start(provider))
		return false;
		
	// Create the interrupt source
	intSrc = IOInterruptEventSource::interruptEventSource((OSObject *) this, (IOInterruptEventAction) &BaboonATA::deviceInterruptOccurred, provider); 

	if (!intSrc || (getWorkLoop()->addEventSource(intSrc) != kIOReturnSuccess)) {
		Verbose_IOLog("BaboonATA: could not create/add interrupt source\n");
		return false;
	}

	intDisableCnt = 0;
	intSrc->enable();
		
	for (i = 0; i < 2; i++)
		if (_devInfo[i].type != kUnknownATADeviceType) {
//			Verbose_IOLog("BaboonATA::start() creating device nub\n");
			childNub = NULL;
			childNub = ATADeviceNub::ataDeviceNub((IOATAController *) this, (ataUnitID) i, _devInfo[i].type);
			if (!childNub) {
				Verbose_IOLog("BaboonATA::start() could not create device nub\n");
				continue;
			}
		
			childNub->attach(this);
			
			_nub[i] = (IOATADevice *) childNub;
			
			childNub->registerService();
		}
	

	Verbose_IOLog("BaboonATA::start() exited\n");
	
	return true;
}

void BaboonATA::free()
{
	Verbose_IOLog("BaboonATA::free() entered\n");
	
	if (baboon && (channel == 1))
		(void) baboon->registerMBDriver(NULL, "ata");
	if (regsMap)
		regsMap->release();
	if (intSrc)
		intSrc->release();

	baboon = NULL;
	regsMap = NULL;
	intSrc = NULL;
	
	Verbose_IOLog("BaboonATA::free() calling super\n");

	super::free();	
}

bool BaboonATA::configureTFPointers(void)
{
	Verbose_IOLog("BaboonATA::configureTFPointers() entered\n");

	regsMap = regsMem->map();
	if (!regsMap) {
		Verbose_IOLog("BaboonATA: could not map registers\n");
		return false;
	}

	baseAddr = (volatile UInt8 *) regsMap->getVirtualAddress();
	if (!baseAddr) {
		Verbose_IOLog("BaboonATA: no base address\n");
		return false;
	}
	
	
	_tfFeatureReg = baseAddr + BABOON_FEATURE;
	_tfSCountReg = baseAddr + BABOON_SCOUNT;
	_tfSectorNReg = baseAddr + BABOON_SNUM;
	_tfCylLoReg = baseAddr + BABOON_CYLLO;
	_tfCylHiReg = baseAddr + BABOON_CYLHI;
	_tfSDHReg = baseAddr + BABOON_SDH;
	_tfStatusCmdReg = baseAddr + BABOON_STATUS;
	_tfDataReg = (volatile UInt16 *) (baseAddr + BABOON_DATA);
	_tfAltSDevCReg = baseAddr + BABOON_ALTSTATUS;

	timingReg = (volatile UInt16 *) (baseAddr + BABOON_TIMING);	

	selectIOTiming((ataUnitID) 0);				// Initalize timing register
	
//	Verbose_IOLog("BaboonATA::configureTFPointers() exited\n");
	
	return true;
}

IOWorkLoop *BaboonATA::getWorkLoop()
{
	if (!_workLoop)
		_workLoop = IOWorkLoop::workLoop();

	return _workLoop;
}

IOReturn BaboonATA::provideBusInfo( IOATABusInfo* infoOut)
{
	UInt8 units;

//	Verbose_IOLog("BaboonATA::provideBusInfo() entered\n");
	
	if (!infoOut)
		return -1;

	infoOut->zeroData();
	
	infoOut->setSocketType(channel ? kMediaBaySocket : kInternalATASocket);
	
	infoOut->setPIOModes(baboon_pio_modes);
	infoOut->setDMAModes(0x00);
	infoOut->setUltraModes(0x00);
	
	units = 0;
	if (_devInfo[0].type != kUnknownATADeviceType)
		units = 1;
	
	if (_devInfo[1].type != kUnknownATADeviceType)
		units++;

	infoOut->setUnits(units);

//	Verbose_IOLog("BaboonATA::provideBusInfo() exited\n");

	return kATANoErr;
}

IOReturn BaboonATA::getConfig(IOATADevConfig *configRequest, UInt32 unit)
{
//	Verbose_IOLog("BaboonATA::getConfig() entered\n");

	// param checks
	if (!configRequest || (unit > 1))
		return -1;

	// IOATADevConfig uses bitmask representation for mode fields
	configRequest->setPIOMode(busTimings[unit].pioMode);
	configRequest->setPIOCycleTime(busTimings[unit].pioCycleTime);
	configRequest->setPacketConfig(_devInfo[unit].packetSend);
	configRequest->setDMAMode(0x0);
	configRequest->setDMACycleTime(0);
	configRequest->setUltraMode(0x0);

//	Verbose_IOLog("BaboonATA::getConfig() exited\n");

	return kATANoErr;
}

static const UInt16 minPIOCycle[] =
{
	600, 				// Mode 0
	383, 				// Mode 1
	240, 				// Mode 2
	180, 				// Mode 3
	120				// Mode 4
};

IOReturn BaboonATA::selectConfig(IOATADevConfig *configRequest, UInt32 unit)
{
	if (!configRequest || (unit > 1))
		return -1;

	if (!(configRequest->getPIOMode() & baboon_pio_modes))
		return kATAModeNotSupported;
	if (configRequest->getDMAMode())
		return kATAModeNotSupported;
	if (configRequest->getUltraMode())
		return kATAModeNotSupported;		

	_devInfo[unit].packetSend = configRequest->getPacketConfig();

	return selectIOTimerValue(configRequest, unit);
}

IOReturn BaboonATA::selectIOTimerValue(IOATADevConfig *configRequest, UInt32 unit)
{
	UInt32 pioCycleTime, pioMode, pioTimerVal;
	UInt32 pioModeBitSig;

	pioModeBitSig = configRequest->getPIOMode();
	pioMode = bitSigToNumeric(pioModeBitSig);
	pioCycleTime = configRequest->getPIOCycleTime();

// Use default PIO cycle times if necessary.
	if (pioCycleTime < minPIOCycle[pioMode])
		pioCycleTime = minPIOCycle[pioMode];

	if (pioCycleTime > 383)
		pioTimerVal = 0x485;		// Run controller at PIO Mode 0
	else if (pioCycleTime > 240)
		pioTimerVal = 0x284;		// Run controller at PIO Mode 1
	else if (pioCycleTime > 180)
		pioTimerVal = 0x143;		// Run controller at PIO Mode 2
	else
		pioTimerVal = 0x142;		// Run controller at PIO Modes 3/4

	busTimings[unit].pioMode = pioModeBitSig;
	busTimings[unit].pioCycleTime = pioCycleTime;
	busTimings[unit].pioTimerVal = pioTimerVal;

// Store values back in configRequest
	return getConfig(configRequest, unit);
}

void BaboonATA::selectIOTiming(ataUnitID unit)
{
	*timingReg = busTimings[unit].pioTimerVal;
	OSSynchronizeIO();
}

void BaboonATA::deviceInterruptOccurred(IOInterruptEventSource *evtSrc, int count)
{
	volatile UInt8 status;

	if (intDisableCnt)			// Software interrupt disable? If so, return
		return;

	if (!_currentCommand) {
		status = *_tfStatusCmdReg;	// Read status register to acknowledge interrupt
		OSSynchronizeIO();
		status++;			// prevent compiler removal of unused variable.
		return;
	}
	
//	Verbose_IOLog("BaboonATA::deviceInterruptOccurred()\n");
	
	(void) super::handleDeviceInterrupt();
}

void BaboonATA::handleTimeout(void)
{
	Verbose_IOLog("BaboonATA::handleTimeout() entered\n");

	if (busOnline)
		super::handleTimeout();

	// A timeout may mean that the interrupts are disabled somewhere. Reenable
	//  the interrupt source if it should be enabled.
//	if (!intDisableCnt)
//		intSrc->enable();
}

IOReturn BaboonATA::message(UInt32 type, IOService* provider, void *argument)
{
	IOATABusCommand *cmdPtr;

	Verbose_IOLog("BaboonATA::message() entered, type = %d\n", (int) type);
	
	switch (type) {
		case kATARemovedEvent:
			Verbose_IOLog("BaboonATA: media bay removed\n");
			if (busOnline) {
				// soft disable interrupts
				intDisableCnt = 1;
				busOnline = false;
				// lock the queue, don't dispatch immediates or anything else.
				_queueState = IOATAController::kQueueLocked;

				// disable the interrupt source(s) and timers
				intSrc->disable();
				
				if (_workLoop) {
					if (intSrc) {
						intSrc->disable();
						_workLoop->removeEventSource(intSrc);
					}
					if (_timer) {
						stopTimer();
						_workLoop->removeEventSource(_timer);
					}
				}
				// flush the command queue
				while (cmdPtr = dequeueFirstCommand()) {	
					cmdPtr->setResult(kIOReturnOffline);
					cmdPtr->executeCallback();
				}

				// abort the current command, if any, within the workloop
				if (_currentCommand)
					_cmdGate->runAction((IOCommandGate::Action) &BaboonATA::cleanUpAction, 0, 0, 0, 0);
				if (_workLoop && _cmdGate)
					_workLoop->removeEventSource(_cmdGate);

				terminate();				
			}
			break;
		default:
			return super::message(type, provider, argument);
			break;
	}

	return kATANoErr;
}

bool BaboonATA::checkTimeout(void)
{
	if (busOnline)
		return super::checkTimeout();
	else
		return true;
}

IOReturn BaboonATA::executeCommand(IOATADevice* nub, IOATABusCommand* command)
{
	if (!busOnline) {
		Verbose_IOLog("BaboonATA::executeCommand() but bus offline\n");
		return kIOReturnOffline;
	}
	
//	Verbose_IOLog("BaboonATA::executeCommand() about to call super\n");

	return super::executeCommand(nub, command);

//	Verbose_IOLog("BaboonATA::executeCommand() exited\n");
}

IOReturn BaboonATA::cleanUpAction(void * /*arg0*/, void * /*arg1*/, void * /*arg2*/, void * /*arg3*/)
{
	if (_currentCommand) {
		_currentCommand->setResult(kIOReturnOffline);
		_currentCommand->executeCallback();
		_currentCommand = NULL;
	}
	
	return kIOReturnSuccess;
}

IOReturn BaboonATA::synchronousIO(void)
{
	IOReturn result;
	
	intSrc->disable();
//	intDisableCnt = 1;
	result = super::synchronousIO();
//	intDisableCnt = 0;
	intSrc->enable();
	
	return result;
}

IOReturn BaboonATA::selectDevice(ataUnitID unit)
{
	IOReturn result;

	intSrc->disable();
//	intDisableCnt = 1;
	result = super::selectDevice(unit);
//	intDisableCnt = 0;
	intSrc->enable();
	
	return result;
}

UInt32 BaboonATA::scanForDrives(void)
{
	IOReturn err;
	
//	Verbose_IOLog("BaboonATA::scanForDrives() entered\n");
	
	err = softResetBus(false);
	if (err) {
		Verbose_IOLog("BaboonATA::scanForDrives() soft reset failed, continuing\n");
	}
	
//	Verbose_IOLog("BaboonATA::scanForDrives() soft reset done\n");
	
	return super::scanForDrives();
}

IOReturn 
BaboonATA::txDataIn (IOLogicalAddress buf, IOByteCount length)
{
	register UInt32 *buf32 = (UInt32 *) buf;

	while (length >= 0x40) {
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		OSSynchronizeIO(); 
		*buf32++ = *((volatile UInt32 *) _tfDataReg);
		length -= 0x40;							// update the length count
	}

	register UInt16 *buf16 = (UInt16 *) buf32;

	while (length >= 2)
	{
		*buf16++ = *_tfDataReg;
		OSSynchronizeIO();									
		length -= 2;							// update the length count
	}

	UInt8 *buf8 = (UInt8 *) buf16;
	
	if (length)									// This is needed to handle odd byte transfer
	{
		*buf8++ = *(IOATARegPtr8Cast(_tfDataReg));
		OSSynchronizeIO();									
		length --;								
	}
	
	return kATANoErr;

}

IOReturn BaboonATA::txDataOut(IOLogicalAddress buf, IOByteCount length)
{
	register UInt32	*buf32 = (UInt32*)buf;

	while (length >= 0x40) {
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO();
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO();
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO();
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		*((volatile UInt32 *) _tfDataReg) = *buf32++;  
		OSSynchronizeIO(); 
		length -= 0x40;							
	}
	
	register UInt16	*buf16 = (UInt16*)buf32;

	while (length >= 2)
	{
		*_tfDataReg = *buf16++;
		OSSynchronizeIO(); 
		length -= 2;								
	}
	
	// Odd byte counts aren't really good on ATA, but we'll do it anyway.
	UInt8* buf8 = (UInt8*)buf16;

	if (length)									
	{
		*(IOATARegPtr8Cast(_tfDataReg)) = *((UInt8 *)*buf8++);
		OSSynchronizeIO(); 
		length --;									
	}
	
	return kATANoErr;
}