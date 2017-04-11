extern "C" {
#include <machine/machine_routines.h>
}

#include <ppc/proc_reg.h>

#include <libkern/c++/OSContainers.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOPlatformExpert.h>

#include "Whitney.h"


#define VERBOSE 1
#if VERBOSE
	#define Verbose_IOLog(x) IOLog(x)
#else
	#define Verbose_IOLog(x)
#endif


// Layout of the ICR (interrupt control register)
struct {
    unsigned : 24;
    unsigned ack : 1;
    unsigned : 1;		// function unknown
    unsigned intLevel : 3;	// 68K interrupt level
    unsigned intPending : 3;	// 68K interrupt pending
} icr_type;

enum {
    icr_ack = 0x80
};


// Interrupt numbers
//  -- ICR ints
#define kVectorVIA1 1		// cascade interrupt - edge-triggered
#define kVectorVIA2 2		// cascade interrupt
#define kVectorSCC 4
#define kVectorNMI 7
//  -- VIA1 ints
#define kVectorOneSecond 8	// interrupts every second
#define kVectorTick 9		// interrupts every tick (1/60 sec.)
#define kVectorPMUSR 10		// PMU data interrupt
#define kVectorPMU 12		// PMU interrupt
#define kVectorTimer2 13	// VIA timer interrupts
#define kVectorTimer1 14
//  -- VIA2 ints
#define kVectorSCSIDRQ 16
#define kVectorSlot 17		// cascade interrupt to slot ints
#define kVectorSCSI 19
#define kVectorSound 20
#define kVectorFloppy 21
//  -- Slot ints
#define kVectorPCMCIA 24
#define kVectorDisplay 25	// internal display
#define kVectorBaboon 27
#define kVectorExpansionCard 29


// Many interrupts are level-triggered, but the ICR interrupts may be edge-triggered
//  (perhaps the 'mode bit' determines edge/level-triggered behavior for these??)

// kLevelMask is a bitmask, where a '1' bit represents a level-triggered interrupt 
//  The following are known to be level-triggered:
#define kLevelMask	((1 << kVectorSound) |		\
			(1 << kVectorBaboon) |		\
			(1 << kVectorPCMCIA))
			// also kVectorDisplay (ECSC)

#define super IOService

OSDefineMetaClassAndStructors(Whitney, IOService);

#if LOG_INT_COUNTS
UInt8 whitney_ps_icr;
UInt16 whitney_ps_xc;
#endif

bool Whitney::start(IOService *provider)
{
	IOInterruptAction handler;
	IOReturn error;
	M2InterruptController *interruptController;

	Verbose_IOLog("entering Whitney::start()\n");
	
        if (!super::start(provider))
		return false;

        if (!(ioMemoryMap = provider->mapDeviceMemoryWithIndex(0)))
		IOLog("Whitney: could not map IO memory\n");

	getPlatform()->setCPUInterruptProperties(provider);

	publishBelow(provider);

	// Allocate the interruptController instance.
	interruptController = new M2InterruptController;
	if (!interruptController)
		return false;

	// call the interruptController's init method.
	error = interruptController->initInterruptController(provider, (volatile UInt8 *) ioMemoryMap->getVirtualAddress());
	if (error != kIOReturnSuccess)
		return false;
	
	// set up M2InterruptController to handle interrupts
	handler = interruptController->getInterruptHandlerAddress();
	provider->registerInterrupt(0, interruptController, handler, 0);

	provider->enableInterrupt(0);
	
	// Register the interrupt controller so clients can find it.
	getPlatform()->registerInterruptController((OSSymbol *) gIODTDefaultInterruptController, interruptController);

	registerService();
  
	Verbose_IOLog("exiting Whitney::start()\n");

    return true;
}

void Whitney::stop(IOService *provider)
{
	Verbose_IOLog("Whitney::stop() entered\n");

	if (ioMemoryMap)
		ioMemoryMap->release();
	
	super::stop(provider);

	Verbose_IOLog("Whitney::stop() exited\n");
}

IOService *Whitney::createNub(IORegistryEntry * from)
{
    IOService *nub;

    nub = new WhitneyDevice;

	if (!nub)
		return NULL;
	if (!nub->init(from, gIODTPlane)) {
		nub->free();
		return NULL;
	}

    return nub;
}

const char *Whitney::deleteList(void)
{
    return("('keyboard', 'device', 'mouse')");
}

const char *Whitney::excludeList(void)
{
    return NULL;
}

void Whitney::publishBelow(IORegistryEntry *root)
{
    OSCollectionIterator *children;
    IORegistryEntry *next;
    IOService *nub;

    // delete OF nodes we don't use
    children = IODTFindMatchingEntries(root, kIODTRecursive, deleteList());
    if (children) {
		while (next = (IORegistryEntry *) children->getNextObject())
			next->detachAll(gIODTPlane);

		children->release();
    }

    // publish everything below, minus excludeList
    children = IODTFindMatchingEntries(root, kIODTRecursive | kIODTExclusive,
				excludeList());
    if (children) {
		while (next = (IORegistryEntry *) children->getNextObject()) {
			if (!(nub = createNub(next)))
				continue;

			nub->attach(this);			
			nub->registerService();
		}
		children->release();
    }
}

bool Whitney::compareNubName(const IOService *nub, OSString *name, OSString **matched) const
{
    return(IODTCompareNubName(nub, name, matched) || nub->IORegistryEntry::compareName(name, matched));
}

IOReturn Whitney::getNubResources(IOService *nub)
{
    if (nub->getDeviceMemory())
		return kIOReturnSuccess;

    IODTResolveAddressing(nub, "reg", getProvider()->getDeviceMemoryWithIndex(0));

    return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super AppleMacIODevice

OSDefineMetaClassAndStructors(WhitneyDevice, AppleMacIODevice);
OSMetaClassDefineReservedUnused(WhitneyDevice,  0);
OSMetaClassDefineReservedUnused(WhitneyDevice,  1);
OSMetaClassDefineReservedUnused(WhitneyDevice,  2);
OSMetaClassDefineReservedUnused(WhitneyDevice,  3);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

bool WhitneyDevice::compareName(OSString *name, OSString **matched) const
{
//    return ((Whitney *) getProvider())->compareNubName(this, name, matched);
	return (IODTCompareNubName(this, name, matched) ||
		IORegistryEntry::compareName(name, matched));
}

IOService *WhitneyDevice::matchLocation(IOService * /* client */)
{
	return this;
}

IOReturn WhitneyDevice::getResources(void)
{
//    return ((Whitney *) getProvider())->getNubResources(this);
	IOService *whitney = this;
	
	if (getDeviceMemory() != 0) return kIOReturnSuccess;
	
	while (whitney && ((whitney = whitney->getProvider()) != 0))
		if (strcmp("pbx-whitney", whitney->getName()) == 0) break;
	
	if (whitney == 0) return kIOReturnError;
	
	IODTResolveAddressing(this, "reg", whitney->getDeviceMemoryWithIndex(0));
	
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#undef super
#define super IOInterruptController


#define WHITNEY_ICR 0x2A000
#define WHITNEY_VIA1 0
#define WHITNEY_VIA2 0x2000

#define VIA_PCR 0x1800
#define VIA_IFR 0x1A00
#define VIA_IER 0x1C00
#define VIA_ANH 0x1E00			/* A data, no handshake: slot ints on via2 */
#define VIA2_SLOT_IFR VIA_ANH

#define via2_slot_int_mask 2


OSDefineMetaClassAndStructors(M2InterruptController, IOInterruptController);

#if 1
inline void M2InterruptController::set_via1_ier_from_cache(void)
{
        *via1_ier = 0x7F;
        OSSynchronizeIO();
        *via1_ier = 0x80 | cached_via1_ier;
        OSSynchronizeIO();
}
#endif

inline void M2InterruptController::set_via2_ier_from_cache(void)
{
        *via2_ier = 0x7F;
        OSSynchronizeIO();
        *via2_ier = 0x80 | cached_via2_ier;
        OSSynchronizeIO();
}

IOReturn M2InterruptController::initInterruptController(IOService *provider, volatile UInt8 *base)
{
	int cnt;
	
	whitneyBase = base;
	parentNub = provider;
		
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
	icr = (volatile UInt32 *)(base + WHITNEY_ICR);
	via1_ier = (volatile UInt8 *)(base + WHITNEY_VIA1 + VIA_IER);
	via1_ifr = (volatile UInt8 *)(base + WHITNEY_VIA1 + VIA_IFR);
 	via2_ier = (volatile UInt8 *)(base + WHITNEY_VIA2 + VIA_IER);
	via2_ifr = (volatile UInt8 *)(base + WHITNEY_VIA2 + VIA_IFR);
	via2_slot_ifr = (volatile UInt8 *)(base + WHITNEY_VIA2 + VIA2_SLOT_IFR);

	#if VERBOSE
	IOLog("M2InterruptController:initInterruptController() current ICR = %x\n", (unsigned int) *icr);
	OSSynchronizeIO();
	#endif

	// Set up HW now that accessors are initialized
	clearAllInterrupts();
	
	#if VERBOSE
	IOLog("M2InterruptController:initInterruptController() new ICR = %x\n", (unsigned int) *icr);
	OSSynchronizeIO();
	#endif

#if LOG_INT_COUNTS
	dumpTimer = IOTimerEventSource::timerEventSource(this, (IOTimerEventSource::Action) &M2InterruptController::dumpIntCounts);
	if (!dumpTimer) {
		Verbose_IOLog("M2IC: could not create dumpTimer");
	} else {
		getWorkLoop()->addEventSource(dumpTimer);
		dumpTimer->enable();
		if (dumpTimer->setTimeoutMS(1000) != kIOReturnSuccess) {
			Verbose_IOLog("M2IC: setTimeoutMS failed\n");
		}
	}
#endif

	return kIOReturnSuccess;
}

IOWorkLoop *M2InterruptController::getWorkLoop(void)
{
	if (!workLoop)
		workLoop = IOWorkLoop::workLoop();
	
	return workLoop;
}

void M2InterruptController::clearAllInterrupts(void)
{
	// Clear out PC registers (AppleVIAInterruptController does this, so we should)
	*((volatile UInt8 *)(whitneyBase + WHITNEY_VIA1 + VIA_PCR)) = 0x00;
	*((volatile UInt8 *)(whitneyBase + WHITNEY_VIA2 + VIA_PCR)) = 0x00;
	
	pending_ints = 0;
	*via1_ier = 0x7F;		// Clear VIA interrupt enables (only ones maskable)
	*via2_ier = 0x7F;
	*via1_ifr = 0x7F;		// Clear VIA interrupt flags
	*via2_ifr = 0x7F;
	cached_via1_ier = 0;
	cached_via2_ier = 0;
	via2_slot_ien = 0;		// Does not correspond to a register -- keeps track of enabled slot interrupts.
	OSSynchronizeIO();
	*icr = 0x80;			// Ack any pending ICR interrupt and reset interrupt mask - do this last just in case the previous lines let any ints through
	icr_ien = 0x6;			// Initially VIA1, VIA2 enabled
}

// Must be compiled with -fpermissive
IOInterruptAction M2InterruptController::getInterruptHandlerAddress(void)
{
	return (IOInterruptAction) &M2InterruptController::handleInterrupt;
}

void M2InterruptController::callVector(long vectorNumber)
{
	IOInterruptVector *vector;

#if LOG_INT_COUNTS
	intCounts[vectorNumber]++;
#endif

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
		// Hard disable the source. If nothing else, sets our internal masks
                //  so this vector is not called again.
		vector->interruptDisabledHard = 1;
		disableVectorHard(vectorNumber, vector);
	}

	vector->interruptActive = 0;
}

IOReturn M2InterruptController::handleInterrupt(void * /*refCon*/, IOService * /*nub*/, int /*source*/)
{
	unsigned long vectorNumber, maskedPending;
	unsigned char x, y, ifr, slot_ifr, orig_v1_ifr;
//	unsigned long loops = 0;
//	#define MAX_INT_LOOPS 100
	
	maskedPending = pending_ints & (icr_ien
		| ((unsigned long) cached_via1_ier << 8)
		| ((unsigned long) cached_via2_ier << 16)
		| ((unsigned long) via2_slot_ien << 24));

	while (maskedPending) {
		vectorNumber = 31 - cntlzw(maskedPending);
		maskedPending &= ~(1 << vectorNumber);
		pending_ints &= ~(1 << vectorNumber);
		callVector(vectorNumber);
	} 

	while (true) {
#if 0
		if (loops++ == MAX_INT_LOOPS)		// Failsafe mechanism
			break;
#endif
		y = *icr;
		*icr = y | 0x80;
		eieio();
		x = *icr & 7;
		eieio();
		
		if (!y)
			break;
		
		// On NMI at least, sometimes we get x = 0, but y should probably be correct.
		//  This might also happen for serial interrupts; I've never observed it for
		//  interrupt levels 1 or 2.
		if (!x)
			x = y & 7;

#if LOG_INT_COUNTS
		totalInts++;
#endif

		if (x == 2) {
			ifr = *via2_ifr & cached_via2_ier;
			eieio();
			*via2_ifr = ifr;						// clear pending VIA2 ints
			eieio();
			if (ifr & via2_slot_int_mask) {
				ifr &= ~via2_slot_int_mask;
				slot_ifr = ~(*via2_slot_ifr) & via2_slot_ien;		// slot ints are active low
				eieio();
				while (slot_ifr) {
					vectorNumber = 31 - cntlzw(slot_ifr);
					slot_ifr &= ~(1 << vectorNumber);
					callVector(vectorNumber + 24);
				}
			}
			
			while (ifr) {
				vectorNumber = 31 - cntlzw(ifr);
				ifr &= ~(1 << vectorNumber);
				callVector(vectorNumber + 16);
			}
			
			continue;
		}
		
		if (x == 1) {
#if 1
			ifr = *via1_ifr & (~0x80); 		// The PMU driver changes the VIA1 int enables directly
			eieio();
			ifr &= *via1_ier;
#else
			ifr = *via1_ifr & cached_via1_ier;
#endif
			eieio();
			*via1_ifr = ifr;						// clear pending VIA1 ints
			eieio();
			
			orig_v1_ifr = ifr;
			while (ifr) {
				vectorNumber = 31 - cntlzw(ifr);
				ifr &= ~(1 << vectorNumber);
				callVector(vectorNumber + 8);
			}

#if 1			
			if (orig_v1_ifr & 4) {			// Stop processing if we got a PMU SR interrupt
#if LOG_INT_COUNTS
				whitney_ps_icr = *icr;
				if ((*via1_ifr & 4) && (whitney_ps_icr & 0x1))
					whitney_ps_xc++;	// Count how many times we still have a PMU interrupt going
#endif
				break;
			}
#endif			
			continue;
		}
		
		callVector(x);
	}

	
	return kIOReturnSuccess;
}

bool M2InterruptController::vectorCanBeShared(long /*vectorNumber*/, IOInterruptVector */*vector*/)
{
	return true;
}

void M2InterruptController::disableVectorHard(long vectorNumber, IOInterruptVector */*vector*/)
{
	boolean_t interruptState;

	if (vectorNumber >= 32)
		return;
	
	interruptState = ml_set_interrupts_enabled(false);
	
	switch (vectorNumber >> 3) {				// Interrupt sources ordered by priority:
		case 1:		// VIA1
#if 0
			cached_via1_ier &= ~(1 << (vectorNumber & 7));
			set_via1_ier_from_cache();
#else
			*via1_ier = 1 << (vectorNumber & 7);
#endif
			break;
		case 3:		// VIA2 slot
			via2_slot_ien &= ~(1 << (vectorNumber & 7));
			if (!via2_slot_ien) {
				cached_via2_ier &= ~via2_slot_int_mask;		// no slot ints enabled -- disable at via2
				set_via2_ier_from_cache();
			}
			break;
		case 2:		// VIA2
			cached_via2_ier &= ~(1 << (vectorNumber & 7));
			set_via2_ier_from_cache();
			break;
		default:	// ICR
			icr_ien &= ~(1 << vectorNumber);
                        icr_ien |= 0x6;				// Never disable the VIA interrupts
			break;
	}
	
	(void) ml_set_interrupts_enabled(interruptState);
}

void M2InterruptController::enableVector(long vectorNumber, IOInterruptVector * /* vector */)
{
	boolean_t interruptState;

	if (vectorNumber >= 32)
		return;
	
	interruptState = ml_set_interrupts_enabled(false);
	
	switch (vectorNumber >> 3) {
		case 1:		// VIA1
#if 0
			cached_via1_ier |= (1 << (vectorNumber & 7));
			cached_via1_ier &= 0x7F;			// make sure bit 7 is not set
			set_via1_ier_from_cache();
#else
			*via1_ier = 0x80 | (1 << (vectorNumber & 7));
#endif
			break;
		case 3:		// VIA2 slot
			via2_slot_ien |= (1 << (vectorNumber & 7));

			cached_via2_ier |= via2_slot_int_mask;
			set_via2_ier_from_cache();
			break;
		case 2:		// VIA2
			cached_via2_ier |= (1 << (vectorNumber & 7));
			cached_via2_ier &= 0x7F;
			set_via2_ier_from_cache();
			break;
		default:	// ICR
			icr_ien |= (1 << vectorNumber);
			break;
	}
	
	(void) ml_set_interrupts_enabled(interruptState);
}

void M2InterruptController::causeVector(long vectorNumber, IOInterruptVector */*vector*/)
{
	boolean_t interruptState = ml_set_interrupts_enabled(false);
	
	pending_ints |= 1 << vectorNumber;
	
	(void) ml_set_interrupts_enabled(interruptState);
	  
	parentNub->causeInterrupt(0);
}

int M2InterruptController::getVectorType(long vectorNumber, IOInterruptVector */*vector*/)
{
	if (kLevelMask & (1 << vectorNumber))
		return kIOInterruptTypeLevel;
	else
		return kIOInterruptTypeEdge;
}



#if LOG_INT_COUNTS

#if 0
// Routine to write characters directly to console -- never disabled
extern "C" {
	extern void vc_putchar(char ch);		/* xnu/osfmk/ppc/POWERMAC/video_console.c */
	extern void _disable_preemption(void);
	extern void _enable_preemption(void);
}

static void vc_putchar_intf(char ch)
{
	vc_putchar(ch);
	if (ch == '\n')
		vc_putchar('\r');
}

// In 10.2, vc_putchar was made static. Use vcputc instead.
static void vc_putchar_intf_jaguar(char ch)
{
	vcputc(0, 0, ch);
	if (ch == '\n')
		vcputc(0, 0, '\r');
}


void ICLog(const char *format, ...)
{
#if 0
	const char *sp = format;
	char c;

	while (true) {
		c = *sp++;
		if (!c)
			break;
		vc_putchar(c);
	}
#endif
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

#endif 

void M2InterruptController::dumpIntCounts(IOTimerEventSource *caller)
{
	unsigned int i;
	UInt32 c, mask;
	
	#define VTGREEN    "\033[32m"
	#define VTRESET    "\033[0m"
	#define IntCountLog	IOLog
	
	mask = (icr_ien
		| ((unsigned long) cached_via1_ier << 8)
		| ((unsigned long) cached_via2_ier << 16)
		| ((unsigned long) via2_slot_ien << 24));

	IntCountLog(VTGREEN "00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D IC TO\n");
	for (i = 0; i < 0x1E; i++) {
		c = intCounts[i];
		intCounts[i] = 0;
		IntCountLog("%02X%c", c, (mask & (1 << i)) ? 'e' : ' ');
	}
	c = totalInts;
	totalInts = 0;
	IntCountLog("%02X %X", *icr, c);
//	IntCountLog("%02X %02X", whitney_ps_icr, whitney_ps_xc);
//	OSSynchronizeIO();
	IntCountLog("\n" VTRESET);
	
	if (dumpCount & 1)
		dumpRegs();
	
	dumpCount++;
	caller->setTimeoutMS(INT_COUNT_PERIOD);
}

void M2InterruptController::dumpRegs()
{
	UInt8 ricr, re1, re2, rf1, rf2, rfs;
	
	ricr = *icr; OSSynchronizeIO();
	re1 = *via1_ier; OSSynchronizeIO();
	re2 = *via2_ier; OSSynchronizeIO();
	rf1 = *via1_ifr; OSSynchronizeIO();
	rf2 = *via2_ifr; OSSynchronizeIO();
	rfs = ~(*via2_slot_ifr) & 0x3F; OSSynchronizeIO();
	
	IntCountLog(VTGREEN "=== ICR:%x EI: %x E1:%x E2:%x F1:%x F2:%x ES:%x FS:%x E1_cache:%x E2_cache:%x\n" VTRESET, ricr, icr_ien, re1, re2, rf1, rf2, via2_slot_ien, rfs, cached_via1_ier, cached_via2_ier);
}
#endif
