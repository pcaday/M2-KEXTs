#ifndef _WHITNEY_H
#define _WHITNEY_H

#include <IOKit/IOService.h>
#include <IOKit/IOInterruptController.h>
#include <IOKit/platform/AppleMacIODevice.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>


#define LOG_INT_COUNTS 1
#define INT_COUNT_PERIOD 2000

#define kNumVectors 32

class M2InterruptController;


class Whitney : public IOService
{
    OSDeclareDefaultStructors(Whitney);

public:
	bool start(IOService *provider);
	void stop(IOService *provider);
	IOService *createNub(IORegistryEntry * from);
	const char *deleteList(void);
	const char *excludeList(void);
	void publishBelow(IORegistryEntry *root);
	bool compareNubName(const IOService *nub, OSString *name, OSString **matched) const;
	IOReturn getNubResources(IOService *nub);
	
protected:
        void testIntController(volatile UInt8 *ioBase, M2InterruptController *ic);
	IOMemoryMap *ioMemoryMap;
};

class WhitneyDevice : public AppleMacIODevice
{
  OSDeclareDefaultStructors(WhitneyDevice);

private:  
  struct ExpansionData { };
  ExpansionData *reserved;

public:
  virtual bool compareName(OSString *name, OSString **matched = 0 ) const;
  virtual IOService *matchLocation(IOService *client);
  virtual IOReturn getResources(void);
    
  OSMetaClassDeclareReservedUnused(WhitneyDevice, 0);
  OSMetaClassDeclareReservedUnused(WhitneyDevice, 1);
  OSMetaClassDeclareReservedUnused(WhitneyDevice, 2);
  OSMetaClassDeclareReservedUnused(WhitneyDevice, 3);
};

class M2InterruptController : public IOInterruptController
{
	OSDeclareDefaultStructors(M2InterruptController);
public:

	IOReturn initInterruptController(IOService *provider, volatile UInt8 *base);
	void clearAllInterrupts(void);

	IOInterruptAction getInterruptHandlerAddress(void);
        IOReturn handleInterrupt(void * /*refCon*/, IOService * /*nub*/, int /*source*/);
	
        bool vectorCanBeShared(long /*vectorNumber*/, IOInterruptVector */*vector*/);

	void disableVectorHard(long vectorNumber, IOInterruptVector */*vector*/);
	void enableVector(long vectorNumber, IOInterruptVector */* vector */);
	void causeVector(long vectorNumber, IOInterruptVector */*vector*/);
        
        int getVectorType(long vectorNumber, IOInterruptVector */*vector*/);

        IOWorkLoop *getWorkLoop(void);
        

//protected:
	IOService *parentNub;
	volatile UInt8 *whitneyBase;

	volatile UInt8 *via1_ier;
	volatile UInt8 *via2_ier;
	volatile UInt8 *via1_ifr;
	volatile UInt8 *via2_ifr;
	volatile UInt8 *via2_slot_ifr;
	volatile UInt32 *icr;
	
	UInt8 cached_via1_ier;
	UInt8 cached_via2_ier;
	UInt8 icr_ien;
	UInt8 via2_slot_ien;
	UInt32 pending_ints;
        
        IOWorkLoop *workLoop;
        
	inline void set_via1_ier_from_cache(void);
	inline void set_via2_ier_from_cache(void);
	void callVector(long vectorNumber);
    
#if LOG_INT_COUNTS
        IOTimerEventSource *dumpTimer;
        UInt32 intCounts[kNumVectors];
        UInt32 totalInts;
        UInt16 dumpCount;
        
        void dumpIntCounts(IOTimerEventSource *caller);
        void dumpRegs();
#endif
};

#endif