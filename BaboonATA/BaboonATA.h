#include <IOKit/IOTypes.h>
#include <IOKit/IOService.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOInterruptController.h>

#include <IOKit/ata/IOATATypes.h>
#include <IOKit/ata/IOATAController.h>
#include <IOKit/ata/IOATACommand.h>
#include <IOKit/ata/IOATADevice.h>
#include <IOKit/ata/IOATABusInfo.h>
#include <IOKit/ata/IOATABusCommand.h>
#include <IOKit/ata/IOATADevConfig.h>
#include <IOKit/ata/ATADeviceNub.h>

class BaboonATA;
class BaboonInterruptController;

class Baboon : public IOService
{
	OSDeclareDefaultStructors(Baboon);

public:
	bool start(IOService *provider);
	void free(void);
	void publishBelow(IORegistryEntry *root);
	IOWorkLoop *getWorkLoop();
	bool registerMBDriver(IOService *driver, const char *mbDevType);
	IOReturn setPowerState(UInt32 newState, IOService *device);

protected:
	void mediaBayInterrupt(IOInterruptEventSource *evtSrc, int count);
	IOReturn mbRemoval(void *arg0 = NULL, void *arg1 = NULL, void *arg2 = NULL, void *arg3 = NULL);
	IOReturn mbInsertion(void *arg0 = NULL, void *arg1 = NULL, void *arg2 = NULL, void *arg3 = NULL);
	IOReturn mbGotPower(void *arg0 = NULL, void *arg1 = NULL, void *arg2 = NULL, void *arg3 = NULL);
	void powerBaboonOn(void *arg0 = NULL, void *arg1 = NULL, void *arg2 = NULL, void *arg3 = NULL);

	bool waitForMBPower();
	void setMBPower(bool powerOn);
	UInt8 readMBID();

	void dumpRegs();
	
	
	IOService *mbDriver;		// Not retain()ed
	IOMemoryMap *regsMap;
	volatile UInt16 *baboonRegs;
	bool baboonPower;
	UInt8 savedBaboonCtrls;
	
	const OSSymbol *symReadMBHWID;
	const OSSymbol *symSetMediaBayPower;
	
	IOService *ata1Nub, *pmu;

	bool mbATAPresent, mbHasPower;
	IOInterruptEventSource *mbIntSrc;

	IOCommandGate *commandGate;
	IOWorkLoop *workLoop;
	
	BaboonInterruptController *interruptController;
};

class BaboonInterruptController : public IOInterruptController {
	OSDeclareDefaultStructors(BaboonInterruptController)
	
public:
	IOReturn initInterruptController(IOService *provider, volatile UInt16 *base, IOInterruptEventSource *mb);
	void clearAllInterrupts(void);

	IOInterruptAction getInterruptHandlerAddress(void);
	IOReturn handleInterrupt(void * /*refCon*/, IOService * /*nub*/, int /*source*/);

	void disableVectorHard(long vectorNumber, IOInterruptVector */*vector*/);
	void enableVector(long vectorNumber, IOInterruptVector * /* vector */);

	bool vectorCanBeShared(long /*vectorNumber*/, IOInterruptVector */*vector*/);
	int getVectorType(long /*vectorNumber*/, IOInterruptVector */*vector*/);
//	IOReturn getInterruptType(int source, int *interruptType);

	UInt8 mbLastIntKinds;

protected:
#if 0
	inline void updateIntEnables();
#endif
	
	IOInterruptEventSource *mbIntSrc;	
	volatile UInt16 *bControls, *bCause, *bPending;
};

class BaboonATA : public IOATAController
{
	friend class Baboon;
	OSDeclareDefaultStructors(BaboonATA)
	
public:
	bool start(IOService *provider);
	void free();
	bool configureTFPointers(void);
	IOWorkLoop *getWorkLoop();
	IOReturn provideBusInfo(IOATABusInfo* infoOut);
	IOReturn getConfig(IOATADevConfig *configRequest, UInt32 unit);
	IOReturn selectConfig(IOATADevConfig* configRequest, UInt32 unit);
	void selectIOTiming(ataUnitID unit);
	void deviceInterruptOccurred(IOInterruptEventSource *evtSrc, int count);
	void handleTimeout(void);
	IOReturn message(UInt32 type, IOService* provider, void *argument);
	bool checkTimeout(void);
	IOReturn executeCommand(IOATADevice *nub, IOATABusCommand *command);
	IOReturn cleanUpAction(void *, void *, void *, void *);
	IOReturn synchronousIO(void);
	IOReturn selectDevice(ataUnitID unit);
	UInt32 scanForDrives(void);
	
protected:
	IOReturn selectIOTimerValue(IOATADevConfig *configRequest, UInt32 unit);
	void deviceInterruptEnable(bool on);
	
	IOReturn txDataIn(IOLogicalAddress buf, IOByteCount length);
	IOReturn txDataOut(IOLogicalAddress buf, IOByteCount length);

	
	Baboon *baboon;
	IODeviceMemory *regsMem;		// Not retain()ed
	IOMemoryMap *regsMap;
	volatile UInt16 *timingReg;
	volatile UInt8 *baseAddr;

	UInt8 channel;
	UInt32 intDisableCnt;
	
	bool busOnline;
	
	struct {
		UInt32 pioCycleTime;
		UInt16 pioMode;
		UInt16 pioTimerVal;
	} busTimings[2];
	
	IOInterruptEventSource *intSrc;
};

