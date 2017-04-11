#include <IOKit/pccard/IOPCCard.h>

extern "C" {
#include "trexss.h"
}

#include "TREXRegisters.h"

#define VerboseIOLog(x...) IOLog(x)
//#define VerboseIOLog(x...) { }

#define trexRegsBaseReg 0x10
#define trexWMBaseReg 0x14

// Interrupt sources
#define cscIntSrcNum 0
#define ioIRQIntSrcNum 1

class TREXPCCard16Bridge : public IOPCCardBridge
{
    OSDeclareDefaultStructors(TREXPCCard16Bridge)

public:
    virtual bool start(IOService *provider);
    virtual void free(void);
    virtual bool init(OSDictionary *propTable);
    virtual IODeviceMemory *ioDeviceMemory(void);

protected:
    IOMemoryMap *socketRegsMap;
    IOInterruptEventSource *intSrc;
    #if 1
    OSData *irqIntSpecifier;
    OSSymbol *irqIntController;
    #endif
    IODeviceMemory *ioMemory;
    
    virtual bool initializeSocketServices(void);
    virtual void addNubInterruptProperties(OSDictionary * propTable);
    
    #ifdef OLD_CSC
    interrupt_handler_t *trexInterruptHandlers;

    virtual void cscInterrupt(void */*refCon*/, IOService */*nub*/, int /*source*/);
    virtual bool addCSCInterruptHandler(unsigned int socket, unsigned int irq, 
				       int (*top_handler)(int), int (*bottom_handler)(int), 
				       int (*enable_functional)(int), int (*disable_functional)(int),
				       const char* name);
    virtual bool removeCSCInterruptHandler(unsigned int socket);
    #endif
};


#define super IOPCCardBridge

OSDefineMetaClassAndStructors(TREXPCCard16Bridge, IOPCCardBridge)

bool TREXPCCard16Bridge::init(OSDictionary *propTable)
{
    bool res;
    int i;
    
    for (i = 0; i < 3; i++) {
        VerboseIOLog("TREX: before init\n");
        IOSleep(10000);
    }
    res = super::init(propTable);
    VerboseIOLog("TREX: after init\n");
    
    return res;
}

bool TREXPCCard16Bridge::start(IOService *provider)
{
    OSArray *array;
    IOPCIDevice *nub = OSDynamicCast(IOPCIDevice, getProvider());
    IODeviceMemory *winMem;
    
    if (!super::start(provider))
        return false;
    
    VerboseIOLog("TREXPCCard16Bridge::start()\n");
    
    winMem = nub->getDeviceMemoryWithRegister(trexWMBaseReg);
    if (!winMem) {
        VerboseIOLog("TREX: could not get window memory\n");
        return false;
    }

    ioMemory = IODeviceMemory::withSubRange(winMem, woIOWin, 0x10000);
    if (!ioMemory) {
        VerboseIOLog("TREX: could not create ioMemory\n");
        return false;
    }
    
    #if 1
    array = OSDynamicCast(OSArray, provider->getProperty(gIOInterruptSpecifiersKey));
    if (!array) {
        VerboseIOLog("TREX: could not get my interrupt specifier\n");
        return false;
    }
    
    irqIntSpecifier = OSDynamicCast(OSData, array->getObject(ioIRQIntSrcNum));
    if (!irqIntSpecifier) {
        VerboseIOLog("TREX: could not get my interrupt specifier\n");
        return false;
    }
    
    irqIntSpecifier->retain();
    
    array = OSDynamicCast(OSArray, provider->getProperty(gIOInterruptControllersKey));
    if (!array) {
        VerboseIOLog("TREX: could not get my interrupt controller\n");
        return false;
    }
    
    irqIntController = OSDynamicCast(OSSymbol, array->getObject(ioIRQIntSrcNum));
    if (!irqIntController) {
        VerboseIOLog("TREX: could not get my interrupt controller\n");
        return false;
    }
    
    irqIntController->retain();
    #endif
    
    #ifdef OLD_CSC
    intSrc = IOInterruptEventSource::interruptEventSource((OSObject *) this,
                (IOInterruptEventAction)&IOPCCardBridge::interruptDispatcher, NULL, 0);
    
    if (!intSrc) {
        VerboseIOLog("TREX: could not create intSrc\n");
        return false;
    }
    
    if (getWorkLoop()->addEventSource(intSrc)) {
        VerboseIOLog("TREX: could not add intSrc to workloop\n");
        return false;
    }
    
    if (provider->registerInterrupt(cscIntIndex, this, (IOInterruptAction) &TREXPCCard16Bridge::cscInterrupt, 0)) {
        VerboseIOLog("TREX: cn register CSC interrupt\n");
        return false;
    }
    #endif
    
    return true;
}

// This replaces super's implementation completely
bool TREXPCCard16Bridge::initializeSocketServices(void)
{
    IOPCIDevice *nub = OSDynamicCast(IOPCIDevice, getProvider());
    IODeviceMemory *winMem;
    
    if (!nub)
        return false;
    
    socketRegsMap = nub->mapDeviceMemoryWithRegister(trexRegsBaseReg);
    if (!socketRegsMap)
        return false;
    
    winMem = nub->getDeviceMemoryWithRegister(trexWMBaseReg);
    if (!winMem)
        return false;
    
    return 0 == init_trex(this, nub, (void *) socketRegsMap->getVirtualAddress(), (void *) winMem->getPhysicalAddress());
}

void
TREXPCCard16Bridge::free(void)
{
    if (socketRegsMap)
        socketRegsMap->release();
    #if 1
    if (irqIntSpecifier)
        irqIntSpecifier->release();
    if (irqIntController)
        irqIntController->release();
    #endif
    
    super::free();
}

IODeviceMemory *
TREXPCCard16Bridge::ioDeviceMemory(void)
{
    return ioMemory;
}

// This replaces super's implementation completely
#if 1
void
TREXPCCard16Bridge::addNubInterruptProperties(OSDictionary * propTable)
{
    OSArray *      controller;
    OSArray *      specifier;

    // Create the interrupt specifer array.
    specifier = OSArray::withCapacity(1);
    if (!specifier) {
        VerboseIOLog("TREX: could not create irqSpec\n");
        return;
    }
    specifier->setObject(irqIntSpecifier);

    // Create the interrupt controller array.
    controller = OSArray::withCapacity(1);
    if (!controller) {
        VerboseIOLog("TREX: could not create irqCtrlr\n");
        return;
    }
    
    controller->setObject(irqIntController);

    // Put the two arrays into the property table.
    propTable->setObject(gIOInterruptControllersKey, controller);
    propTable->setObject(gIOInterruptSpecifiersKey, specifier);

    // Release the arrays after being added to the property table.
    specifier->release();
    controller->release();
}
#endif

#ifdef OLD_CSC

bool
TREXPCCard16Bridge::addCSCInterruptHandler(unsigned int socket, unsigned int irq, 
				       int (*top_handler)(int), int (*bottom_handler)(int), 
				       int (*enable_functional)(int), int (*disable_functional)(int),
				       const char* name)
{
    IOService *provider;
    struct interrupt_handler * h = (struct interrupt_handler *)IOMalloc(sizeof(interrupt_handler_t));
    if (!h) return false;

    h->socket = socket;
    h->irq = irq;
    h->top_handler = top_handler;
    h->bottom_handler = bottom_handler;
    h->enable_functional = enable_functional;
    h->disable_functional = disable_functional;
    h->name = name;
    h->interruptSource = intSrc;

    provider = getProvider();
    provider->disableInterrupt(1);

    h->next = trexInterruptHandlers;
    trexInterruptHandlers = h;

    provider->enableInterrupt(1);

    return true;
}

bool
TREXPCCard16Bridge::removeCSCInterruptHandler(unsigned int socket)
{
    interrupt_handler_t * prev = NULL;
    interrupt_handler_t * h = trexInterruptHandlers;
    IOService *provider;
    
    provider = getProvider();
    provider->disableInterrupt(1);

    while (h) {
	if (h->socket == socket) {
	    if (h == trexInterruptHandlers) {
		trexInterruptHandlers = h->next;
	    } else {
		prev->next = h->next;
	    }
	    IOFree(h, sizeof(interrupt_handler_t));
	}

	prev = h;
	h = h->next;
    }

    provider->enableInterrupt(1);

    return true;
}

void
TREXPCCard16Bridge::cscInterrupt(void */*refCon*/, IOService */*nub*/, int /*source*/)
{
    interrupt_handler_t * h = trexInterruptHandlers;
    while (h) {
	unsigned int event = h->bottom_handler(h->socket);
	if (event) h->interruptSource->interruptOccurred(0, 0, 0);
	h = h->next;
    }
}

#endif

bool
trex_fixup(IOPCCard16Device *dev, unsigned int socket)
{
    unsigned long range_list[(4 + 2) * 2];
    unsigned int i, num_ranges, num_mem;
//    IODeviceMemory::InitElement new_ranges[CISTPL_MEM_MAX_WIN + CISTPL_IO_MAX_WIN];
    OSArray *devMem;
    IODeviceMemory *dm;
        
    if (!dev)
        return false;

    // Don't do anything if the device is not a TREX socket
    if (!OSDynamicCast(TREXPCCard16Bridge, dev->getProvider()))
        return true;

    if (trex_get_ranges(socket, range_list, &num_ranges, &num_mem))
        return false;

    if (num_ranges != dev->getDeviceMemoryCount()) {
        IOLog("TREX: range count mismatch\n");
        return false;
    }

#if 0
    for (i = 0; i < num_ranges; i++) {
        new_ranges[i].start = range_list[2*i];
        new_ranges[i].length = range_list[2*i + 1] - range_list[2*i] + 1;
    }

    devMem = IODeviceMemory::arrayFromList(new_ranges, num_ranges);
    if (!devMem) {
        VerboseIOLog("TREX: could not create IODeviceMemory\n");
        return false;
    }
#else
    devMem = OSArray::withCapacity(num_ranges);
    if (!devMem) {
        VerboseIOLog("TREX: could not create IODeviceMemory\n");
        return false;
    }
    
    for (i = 0; i < num_ranges; i++) {        
        dm = IODeviceMemory::withRange(range_list[2*i], range_list[2*i + 1] - range_list[2*i] + 1);
        if (!dm) {
            VerboseIOLog("TREX: cn create IODevMem elt\n");
            goto failed;
        }

        dm->setTag((i >= num_mem) ? IOPCCARD16_IO_WINDOW : IOPCCARD16_MEMORY_WINDOW);
        if (!devMem->setObject(dm)) {
            VerboseIOLog("TREX: cn add IODevMem elt\n");
            goto failed;
        }
        dm->release(); dm = NULL;
    }

#endif
    dev->setDeviceMemory(devMem);
    devMem->release(); devMem = NULL;
    
    return true;

failed:
    if (devMem)
        devMem->release();
    if (dm)
        dm->release();
    return false;
}

