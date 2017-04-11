#include "TREXPseudoPCIBridge.h"
#include "TREXRegisters.h"
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/OSByteOrder.h>
#include <ppc/proc_reg.h>
#include <ppc/machine_routines.h>

#if VERBOSE
    #define VerboseIOLog(x...) IOLog(x)
#else
    #define VerboseIOLog(x...) { }
#endif

// Safe release macro
#define RELEASE(obj) { (obj)->release(); (obj) = NULL; }


#define super IOPCIBridge

OSDefineMetaClassAndStructors(TREXPseudoPCIBridge, IOPCIBridge)

#define trexICNamePrefix "TREXInterruptController"

enum {
    kBridgeNum = 0,
    kTREXBridgeNum = 0x0F
};

enum {
    kBridgeSelfDevice = 0,
    kBridgeTREXDevice = 0x0F
};

typedef struct {
    unsigned long addr;
    unsigned long len;
} wmPropFormat;

#define highBits ((kBridgeNum << 0x10) | (kBridgeTREXDevice << 0x0B))

#define TREX_regIOSpace (0x81000010 | highBits)
#define TREX_winMemSpace (0x82000014 | highBits)
#define trexPseudoCSDefined 0x1C
static const unsigned char trexConfigSpace[] =
{
    0x99, 0x99,	// Vendor ID
    0x99, 0x99,	// Device ID
    0x00, 0x00, 0x00, 0x00, 0x00, // Command, Status, Revision ID
    0x00, 0x05, 0x06, // Class Code
    0x00, 0x00, 0x00, 0x00, // Cache line size, latency timer, header type, BIST
    0x00, 0x00, 0x00, 0x00, // Base address 0
    0x00, 0x00, 0x00, 0x00, // Base address 1 (?)
    0x00, kTREXBridgeNum, kTREXBridgeNum, 0x00, // 0, First bus#, Last bus#, 0
};

bool TREXPseudoPCIBridge::start( IOService * provider )
{
    IORegistryEntry *newNub;
    IODeviceMemory *regMem;
    OSDictionary *propTable;
    OSData *prop;
    unsigned long propNum;
    wmPropFormat *range;
    IOPhysicalAddress trexRegs;
    char uniqueName[50];
    IOInterruptAction intHandler;
    unsigned int socket;
    unsigned int i;
    unsigned long nubReg[5 * 3] =
        {0x00000800 | highBits, 0, 0, 0, 0,
        TREX_regIOSpace, 0, 0x50F1C000, 0, sockOffStep,
        TREX_winMemSpace, 0, 0x80000000, 0, woSocketStep};

    VerboseIOLog("TREXPseudoPCIBridge::start() entered\n");

#if !TEST
    // Get memory ranges
    regMem = provider->getDeviceMemoryWithIndex(0);
    if (!regMem) {
        VerboseIOLog("TREXPseudoPCIBridge: no registers\n");
        goto error;
    }
    trexRegs = regMem->getPhysicalAddress();
    nubReg[(5 * 1) + 2] = trexRegs;

    prop = OSDynamicCast(OSData, provider->getProperty("window-mem"));
    if (!prop || (prop->getCapacity() != sizeof(wmPropFormat))) {
	VerboseIOLog("TREX: could not get window-mem or bad format\n");
	goto error;
    }
    
    range = (wmPropFormat *) prop->getBytesNoCopy();
    prop = NULL;
    if (!range) {
        VerboseIOLog("TREX: could not get window-mem property data\n");
        goto error;
    }
    
    nubReg[(5 * 2) + 2] = range->addr;
    
    // Create and register TREXInterruptController
    sprintf(uniqueName, "%s%08x", trexICNamePrefix, trexRegs);
    icName = (OSSymbol *) OSSymbol::withCString(uniqueName);
    if (!icName) {
        VerboseIOLog("TPPB: could not create icName\n");
        goto error;
    }
    
    trexIC = new TREXInterruptController;
    if (!trexIC) {
        VerboseIOLog("TPPB: could not create interruptController\n");
        goto error;
    }
    
    if (trexIC->initInterruptController(this, trexRegs) != kIOReturnSuccess) {
        VerboseIOLog("TPPB: initInterruptController failed\n");
        goto error;
    }

    intHandler = trexIC->getInterruptHandlerAddress();
    if (provider->registerInterrupt(0, trexIC, intHandler, 0) != kIOReturnSuccess) {
        VerboseIOLog("TPPB: registerInterrupt failed\n");
        goto error;
    }
    
    getPlatform()->registerInterruptController(icName, trexIC);
    
    provider->enableInterrupt(0);    // Always good to do this
#endif

    for (socket = 0; socket < kNumSockets; socket++) {
        newNub = new IORegistryEntry;
        if (!newNub) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create nub\n");
            goto error;
        }
            
        propTable = OSDictionary::withCapacity(1);
        if (!propTable) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create property table\n");
            goto error;
        }
        
    #if !TEST
        // Create PMU socket# property for ApplePMUPCCardEject
        propNum = socket + 1;
        prop = OSData::withBytes(&propNum, sizeof(unsigned long));
        if (!prop) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create a property\n");
            goto error;
        }
        propTable->setObject("AAPL,pmu-socket-number", prop);
    #endif
    
        VerboseIOLog("TREXPseudoPCIBridge: here\n");
    
        // Create standard PCI device properties
    #if !TEST
        prop = OSData::withBytes(nubReg, 5 * 3 * sizeof(unsigned long));
    #else
        prop = OSData::withBytes(nubReg, 5 * 1 * sizeof(unsigned long));
    #endif
        if (!prop) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create a property\n");
            goto error;
        }
        
        propTable->setObject("reg", prop);
        prop->release(); prop = NULL;
        
        // IOPCIBridge uses "assigned-addresses" to set the IODeviceMemory property,
        //  which will be used when IOPCCardBridge calls mapDeviceMemoryWithRegister(0x10)
    #if !TEST
        prop = OSData::withBytes(&nubReg[5 * 1], 5 * 2 * sizeof(unsigned long));
    #else
        prop = OSData::withBytes(&nubReg[5 * 1], 5 * 0 * sizeof(unsigned long));
    #endif
        if (!prop) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create a property\n");
            goto error;
        }
        
        propTable->setObject("assigned-addresses", prop);
        prop->release(); prop = NULL;
                
        propNum = 0x9999;
        prop = OSData::withBytes(&propNum, sizeof(unsigned long));
        if (!prop) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create a property\n");
            goto error;
        }
        
        propTable->setObject("vendor-id", prop);
        prop->release(); prop = NULL;
        
        propNum = 0x9999;
        prop = OSData::withBytes(&propNum, sizeof(unsigned long));
        if (!prop) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create a property\n");
            goto error;
        }
        
        propTable->setObject("device-id", prop);
        prop->release(); prop = NULL;
        
        propNum = trexConfigSpace[kIOPCIConfigRevisionID];
        prop = OSData::withBytes(&propNum, sizeof(unsigned long));
        if (!prop) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create a property\n");
            goto error;
        }
        
        propTable->setObject("revision-id", prop);
        prop->release(); prop = NULL;
    
        propNum = 0x06050000;
        prop = OSData::withBytes(&propNum, sizeof(unsigned long));
        if (!prop) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create a property\n");
            goto error;
        }
        
        propTable->setObject("class-code", prop);
        prop->release(); prop = NULL;
        
        propNum = 0x08;
        prop = OSData::withBytes(&propNum, sizeof(unsigned long));
        if (!prop) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create a property\n");
            goto error;
        }
        
        propTable->setObject("cache-line-size", prop);
        prop->release(); prop = NULL;
    
        propNum = 0x00;
        prop = OSData::withBytes(&propNum, sizeof(unsigned long));
        if (!prop) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create a property\n");
            goto error;
        }
        
        propTable->setObject("min-grant", prop);
        prop->release(); prop = NULL;
    
        propNum = 0x00;
        prop = OSData::withBytes(&propNum, sizeof(unsigned long));
        if (!prop) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create a property\n");
            goto error;
        }
        
        propTable->setObject("max-latency", prop);
        prop->release(); prop = NULL;
    
        propNum = 0x01;
        prop = OSData::withBytes(&propNum, sizeof(unsigned long));
        if (!prop) {
            VerboseIOLog("TREXPseudoPCIBridge: could not create a property\n");
            goto error;
        }
        
        propTable->setObject("devsel-speed", prop);
        prop->release(); prop = NULL;
    
        newNub->init(propTable);
        nub[socket] = newNub;
        newNub->setName("pci9999,9999");
        newNub->attachToParent(provider, gIODTPlane);
    
        propTable->release(); propTable = NULL;
        
        nubReg[(5 * 1) + 2] += sockOffStep; nubReg[(5 * 2) + 2] += woSocketStep;
        for (i = 0; i < 3; i++)
            nubReg[(5 * i)] |= 0x100;		// second nub is function 1
    }
        
    VerboseIOLog("TREXPseudoPCIBridge::start() done\n");

    // Delay super::start until here since we have modified the device tree
    return super::start(provider);
    
error:
    if (prop)
        prop->release();
    if (propTable)
        propTable->release();
    for (socket = 0; socket < kNumSockets; socket++)
        if (nub[socket])
            nub[socket]->release();
    
    return false;
}

void TREXPseudoPCIBridge::free(void)
{
    unsigned int socket;
    
    for (socket = 0; socket < kNumSockets; socket++) {
        if (nub[socket]) {
            nub[socket]->detachFromParent(getProvider(), gIODTPlane);
            nub[socket]->release();
        }
    }
    if (trexIC)
        trexIC->release();
    if (icName)
        icName->release();

    super::free();
}


bool TREXPseudoPCIBridge::configure( IOService * provider )
{
    bool ok;

    // Fake something up...
    ok = addBridgeMemoryRange(0x70000000, 0x10000000, false);
    ok = addBridgeIORange(0, 0x00100000);

    return( super::configure( provider ));
}

IOReturn TREXPseudoPCIBridge::getNubResources(IOService *service)
{
    IOService *provider;
    IODeviceMemory *mem, *newMem;
    OSArray *array;
    wmPropFormat *range;
    OSData *prop;
    long iSpecLong, i, socket;
    OSData *iSpec;
    IOPCIDevice *pciDev;
    
#if TEST
    (void) super::getNubResources(service);
    return kIOReturnSuccess;

#else

    if(service->getDeviceMemory())
	return( kIOReturnSuccess );

    VerboseIOLog("TPPB: *** getNubResources\n");

    pciDev = OSDynamicCast(IOPCIDevice, service);
    if (!pciDev) {
        VerboseIOLog("TPPB: could not cast service to IOPCIDevice");
        goto error;
    }
    
    socket = pciDev->space.s.functionNum;

    array = OSArray::withCapacity(2);
    if (!array) {
        VerboseIOLog("TREXPseudoPCIBridge: could not create array\n");
        goto error;
    }
    
    provider = getProvider();
    
    mem = provider->getDeviceMemoryWithIndex(0);
    if (!mem) {
        VerboseIOLog("TREXPseudoPCIBridge: no registers\n");
        goto error;
    }

    newMem = IODeviceMemory::withRange(mem->getPhysicalAddress() + (socket * sockOffStep), regsSize);
    if (!mem) {
        VerboseIOLog("TREXPseudoPCIBridge: could not create IODeviceMemory\n");
        goto error;
    }

    newMem->setTag(TREX_regIOSpace | (socket ? 0x100 : 0));	// #if 0 Also modifies our IODeviceMemory, but we don't mind #endif
    array->setObject(0, newMem);
    newMem->release(); newMem = NULL;
    
    prop = OSDynamicCast(OSData, provider->getProperty("window-mem"));
    if (!prop || (prop->getCapacity() != sizeof(wmPropFormat))) {
	VerboseIOLog("TREXPseudoPCIBridge: could not get window-mem or bad format\n");
	goto error;
    }
    
    range = (wmPropFormat *) prop->getBytesNoCopy();
    if (!range) {
        VerboseIOLog("TREXPseudoPCIBridge: could not get window-mem property data\n");
        goto error;
    }
    
    mem = IODeviceMemory::withRange(((IOPhysicalAddress) range->addr) + (socket * woSocketStep), windowsSize);
    if (!mem) {
        VerboseIOLog("TREXPseudoPCIBridge: could not create window device memory\n");
        goto error;
    }
    
    mem->setTag(TREX_winMemSpace | (socket ? 0x100 : 0));
    array->setObject(1, mem);
    mem->release(); mem = NULL;
    
    service->setDeviceMemory(array);
    array->release(); array = NULL;
    
    // Create interrupt controllers array
    array = OSArray::withCapacity(2);
    if (!array) {
        VerboseIOLog("TPPB: could not create array\n");
        goto error;
    }
    
    array->setObject(icName);
    array->setObject(icName);
    
    service->setProperty(gIOInterruptControllersKey, array);
    array->release(); array = NULL;
    
    // Create interrupt specifiers array
    array = OSArray::withCapacity(2);
    if (!array) {
        VerboseIOLog("TPPB: could not create array\n");
        goto error;
    }
    
    for (i = 0, iSpecLong = (socket << 1); i < 2; i++, iSpecLong++) {
        iSpec = OSData::withBytes(&iSpecLong, sizeof(iSpecLong));
        if (!iSpec) {
            VerboseIOLog("TPPB: could not create a property\n");
            goto error;
        }
        array->setObject(iSpec);
        iSpec->release(); iSpec = NULL;
    }
    
    service->setProperty(gIOInterruptSpecifiersKey, array);
    array->release(); array = NULL;

    return kIOReturnSuccess;

error:
    if (iSpec)
        iSpec->release();
    if (array)
        array->release();
    if (mem)
        mem->release();
    
    return kIOReturnNoResources;
#endif
}

IODeviceMemory *TREXPseudoPCIBridge::ioDeviceMemory( void )
{
    return NULL;
}

UInt8 TREXPseudoPCIBridge::firstBusNum( void )
{
    return kBridgeNum;
}

UInt8 TREXPseudoPCIBridge::lastBusNum( void )
{
    return kBridgeNum;
}

UInt32 TREXPseudoPCIBridge::configRead32( IOPCIAddressSpace space, UInt8 offset )
{
//    VerboseIOLog("TPPB: read32 @ %u\n", offset);
    if ((offset + 3) < trexPseudoCSDefined)
        return OSSwapLittleToHostInt32(*((unsigned long *) &trexConfigSpace[offset]));
    else
        return 0;
}

void TREXPseudoPCIBridge::configWrite32( IOPCIAddressSpace space, UInt8 offset, UInt32 data )
{
//    VerboseIOLog("TPPB: write32 @ %u: %u\n", offset, data);
}

UInt16 TREXPseudoPCIBridge::configRead16( IOPCIAddressSpace space, UInt8 offset )
{
//    VerboseIOLog("TPPB: read16 @ %u\n", offset);
    
    if ((offset + 1) < trexPseudoCSDefined)
        return OSSwapLittleToHostInt16(*((unsigned short *) &trexConfigSpace[offset]));
    else
        return 0;
}

void TREXPseudoPCIBridge::configWrite16( IOPCIAddressSpace space,
					UInt8 offset, UInt16 data )
{
//    VerboseIOLog("TPPB: write16 @ %u: %u\n", offset, data);
}

UInt8 TREXPseudoPCIBridge::configRead8( IOPCIAddressSpace space, UInt8 offset )
{
//    VerboseIOLog("TPPB: read8 @ %u\n", offset);
    
    if (offset < trexPseudoCSDefined)
        return trexConfigSpace[offset];
    else
        return 0;
}

void TREXPseudoPCIBridge::configWrite8( IOPCIAddressSpace space,
					UInt8 offset, UInt8 data )
{
//    VerboseIOLog("TPPB: write8 @ %u: %u\n", offset, data);
}

IOPCIAddressSpace TREXPseudoPCIBridge::getBridgeSpace( void )
{
    IOPCIAddressSpace space;

    space.bits = 0;
    space.s.busNum = kBridgeNum;
    space.s.deviceNum = kBridgeSelfDevice;

    return( space );
}

#undef super
#define super IOInterruptController


#define kNumVectors (2 * kNumSockets)

OSDefineMetaClassAndStructors(TREXInterruptController, IOInterruptController)

IOReturn
TREXInterruptController::initInterruptController(IOService *provider, IOPhysicalAddress base)
{
    int cnt;
    IODeviceMemory *trexMem;
    IOReturn result;
    
    intParentDevice = provider->getProvider();
    
    trexMem = IODeviceMemory::withRange(base, (kNumSockets * sockOffStep));
    if (!trexMem) {
        VerboseIOLog("TREX IC: could not create trexMem\n");
        return kIOReturnNoResources;
    }
    
    trexRegMap = trexMem->map();
    if (!trexRegMap) {
        VerboseIOLog("TREX IC: could not map registers\n");
        return kIOReturnNoResources;
    }
    
    RELEASE(trexMem);
    
    trexRegs = (volatile UInt8 *) trexRegMap->getVirtualAddress();
    
    registeredEvents = 0;
  
    // Allocate the memory for the vectors
    vectors = (IOInterruptVector *)IOMalloc(kNumVectors * sizeof(IOInterruptVector));
    if (vectors == NULL) return kIOReturnNoMemory;
    bzero(vectors, kNumVectors * sizeof(IOInterruptVector));
  
    // Allocate locks for the vectors
    for (cnt = 0; cnt < kNumVectors; cnt++) {
	vectors[cnt].interruptLock = IOLockAlloc();
	if (vectors[cnt].interruptLock == NULL) {
            for (cnt = 0; cnt < kNumVectors; cnt++) {
                if (vectors[cnt].interruptLock != NULL)
                    IOLockFree(vectors[cnt].interruptLock);
            }
            return kIOReturnNoResources;
	}
    }
  
    return kIOReturnSuccess;
}

void
TREXInterruptController::free(void)
{
    if (trexRegMap)
        trexRegMap->release();
    
    trexRegs = NULL;		// Make sure we don't use this virtual address (which may now be invalid)
    
    super::free();
}

inline volatile UInt8 *
TREXInterruptController::getSocketRegs(unsigned long skt)
{
    return trexRegs + (skt * sockOffStep);
}

IOInterruptAction
TREXInterruptController::getInterruptHandlerAddress(void)
{
    return (IOInterruptAction) &TREXInterruptController::handleInterrupt;
}

#ifdef __ppc__
#define sync() __asm__ volatile("sync")
#define isync() __asm__ volatile("isync")
#endif


IOReturn
TREXInterruptController::handleInterrupt(void */*refCon*/,
					     IOService */*nub*/,
					     int /*source*/)
{
    unsigned long i, j, vnum;
    IOInterruptVector *vector;
    volatile UInt8 *regs;
    UInt8 maskedEvents, events, outEvents, cfg0;
    bool wasAttr[kNumSockets];
    
    wasAttr[0] = false;
    wasAttr[1] = false;
        
    for (i = 0; i < kNumSockets; i++) {        
        regs = getSocketRegs(i);

        // Quit if we don't have any interrupt sources
        if (!((regs[rIntFlag] & mIntFlag) || outEvents))
            continue;

        // Disable attribute memory if it is enabled. Probably unnecessary
        //  since we only care about this for I/O IRQs, and they won't occur
        //  under the memory-only interface.

        cfg0 = regs[rCfg0];
        OSSynchronizeIO();
        
        if (cfg0 & mAttr) {
            wasAttr[i] = true;
            
            regs[rCfg0] = cfg0 & ~mAttr;
            OSSynchronizeIO();
        }
        
        while (true) {
            events = regs[rEvents];
            OSSynchronizeIO();
            maskedEvents = events & regs[rIntEnable];
            OSSynchronizeIO();
            
            // debug
//            regs[rEvents] = ~maskedEvents;

            // Check for IRQ and clear its interrupt flag
            if ((cfg0 & mSetForIO) && (maskedEvents & mIRQ)) {
                outEvents |= 2;
                
                regs[rEvents] = ~mIRQ;
                OSSynchronizeIO();

                maskedEvents &= ~mIRQ;
            }
                        
            // All other interrupts are status change (CSC) and are routed to TREXPCCard16Bridge
            // Don't clear status change interrupts -- leave them for trex_interrupt_bottom 
            //  (in Socket Services, trexss.c)
            if (maskedEvents)
                outEvents |= 1;
        }

        // Call the appropriate vectors
        for (j = 0; j < 2; j++) {
            if (outEvents & (1 << j)) {
                vnum = (2 * i) + j;
                vector = &vectors[vnum];
                vector->interruptActive = 1;
        #ifdef __ppc__
                sync();
                isync();
        #endif
                if (!vector->interruptDisabledSoft && (registeredEvents & (1 << vnum))) {
        #ifdef __ppc__
                    isync();
        #endif
                    // Call the handler if it exists.
                    if (vector->interruptRegistered) {
                        vector->handler(vector->target, vector->refCon,
                                        vector->nub, vector->source);
                    } else
                        registeredEvents ^= (1 << vnum);
                } else {
                    // Hard disable the source.
                    vector->interruptDisabledHard = 1;
                    disableVectorHard(i, vector);
                }
            
                vector->interruptActive = 0;
            }
        }
    }
    
#if TREX_IC_LOOP_UNTIL_NO_INTS
    } while (gotInts);
#endif

    // Restore attribute memory on any sockets where it was disabled.
    for (i = 0; i < kNumSockets; i++)
        if (wasAttr[i]) {
            regs = getSocketRegs(i);
            
            cfg0 = regs[rCfg0];
            OSSynchronizeIO();
            regs[rCfg0] = cfg0 | mAttr;
            OSSynchronizeIO();
        }
    
    return kIOReturnSuccess;
}


void
TREXInterruptController::initVector(long vectorNumber, IOInterruptVector *vector)
{
    registeredEvents |= (1 << vectorNumber);
    VerboseIOLog("TREX IC: %x\n", registeredEvents);
}

bool
TREXInterruptController::vectorCanBeShared(long /*vectorNumber*/, IOInterruptVector */*vector*/)
{
    return true;
}

// We have a problem because there is no way to indicate to socket services
//  when IRQ has been enabled if the memory interface is active. Hence,
//  we also mirror IRQ enabling in the mInsert bit of rIntEnable. This interrupt
//  never occurs, it appears, on my PowerBook 1400.
void
TREXInterruptController::disableVectorHard(long vectorNumber, IOInterruptVector */*vector*/)
{
    volatile UInt8 *regs = getSocketRegs(vectorNumber >> 1);
    UInt8 x;
    boolean_t state;
    
    x = mInsert;
    if (regs[rCfg0] & mSetForIO)
        x |= mIRQ;
    
    if (vectorNumber & 1)
        x = ~x;
        // if (!vectorNumber & 1) x = 0x00;	// Disable all interrupts if CSC vector disabled (since this only happens when no bridge object is attached to one of the PCI nodes) XXXX not true, also disabled during IOPCCardAddCSCHandlers and more importantly, sleep
    
    // Our usual problem: no way to disable the TREX interrupt itself, so we resort
    //  to disabling all interrupts.
    state = ml_set_interrupts_enabled(false);
    regs[rIntEnable] &= x;
    ml_set_interrupts_enabled(state);
}

void
TREXInterruptController::enableVector(long vectorNumber, IOInterruptVector */*vector*/)
{
    volatile UInt8 *regs = getSocketRegs(vectorNumber >> 1);
    UInt8 x;
    boolean_t state;

    // We don't do any enabling for the CSC vectors: TREX socket services must do that.
    // Again, we mirror whether the IRQ is enabled in the mInsert bit.
    if (vectorNumber & 1) {
        x = mInsert;
        if (regs[rCfg0] & mSetForIO)
            x |= mIRQ;
        
        state = ml_set_interrupts_enabled(false);
        regs[rIntEnable] |= x;
        ml_set_interrupts_enabled(state);
    }
}

void
TREXInterruptController::causeVector(long vectorNumber, IOInterruptVector */*vector*/)
{
    pendingEvents |= (1 << vectorNumber);
    intParentDevice->causeInterrupt(0);
}

int
TREXInterruptController::getVectorType(long vectorNumber, IOInterruptVector */*vector*/)
{
    return (vectorNumber & 1) ? kIOInterruptTypeLevel : kIOInterruptTypeEdge;
}
