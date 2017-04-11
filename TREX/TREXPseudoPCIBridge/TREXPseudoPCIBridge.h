#ifndef _TREX_PSEUDO_PCI_BRIDGE_H
#define _TREX_PSEUDO_PCI_BRIDGE_H 1

#include <IOKit/pci/IOPCIBridge.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOLib.h>
#include "TREXRegisters.h"


class TREXInterruptController;

class TREXPseudoPCIBridge : public IOPCIBridge
{
    OSDeclareDefaultStructors(TREXPseudoPCIBridge)

protected:
    TREXInterruptController *trexIC;
    OSSymbol *icName;
    
    virtual UInt8 firstBusNum( void );
    virtual UInt8 lastBusNum( void );

public:
    virtual bool start(	IOService * provider );
    virtual bool configure( IOService * provider );
    virtual void free(void);
    
    virtual IODeviceMemory *ioDeviceMemory( void );
    IOReturn TREXPseudoPCIBridge::getNubResources(IOService *service);

    virtual UInt32 configRead32( IOPCIAddressSpace space, UInt8 offset );
    virtual void configWrite32( IOPCIAddressSpace space,
					UInt8 offset, UInt32 data );
    virtual UInt16 configRead16( IOPCIAddressSpace space, UInt8 offset );
    virtual void configWrite16( IOPCIAddressSpace space,
					UInt8 offset, UInt16 data );
    virtual UInt8 configRead8( IOPCIAddressSpace space, UInt8 offset );
    virtual void configWrite8( IOPCIAddressSpace space,
					UInt8 offset, UInt8 data );

    IOPCIAddressSpace getBridgeSpace();
    
    
    IORegistryEntry *nub[kNumSockets];
};

class TREXInterruptController : public IOInterruptController
{
    OSDeclareDefaultStructors(TREXInterruptController);
  
protected:
    IOMemoryMap *trexRegMap;
    volatile UInt8 *trexRegs;
    IOService *intParentDevice;
    
    unsigned long		registeredEvents;
    unsigned long		pendingEvents;

    struct ExpansionData 	{ };
    ExpansionData *		reserved;

    inline volatile UInt8 *	getSocketRegs(unsigned long skt);

public:

    IOReturn		initInterruptController(IOService *provider, IOPhysicalAddress base);
    void free(void);
  
    IOInterruptAction	getInterruptHandlerAddress(void);
    IOReturn		handleInterrupt(void * refCon, IOService * nub, int source);
  
    void		initVector(long vectorNumber, IOInterruptVector *vector);
    
    bool		vectorCanBeShared(long vectorNumber, IOInterruptVector * vector);
    int			getVectorType(long vectorNumber, IOInterruptVector * vector);
    void		disableVectorHard(long vectorNumber, IOInterruptVector * vector);
    void		enableVector(long vectorNumber, IOInterruptVector * vector);
    void		causeVector(long vectorNumber, IOInterruptVector * vector);
};


#endif
