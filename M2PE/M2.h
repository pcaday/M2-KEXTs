#ifndef _M2_H
#define _M2_H

#include <IOKit/platform/ApplePlatformExpert.h>

enum {kChipSetTypeM2 = 47};		// arbitrary, does not conflict with established chipsets.

class M2PE : public ApplePlatformExpert
{
	OSDeclareDefaultStructors(M2PE);
  
private:
	void getDefaultBusSpeeds(long *numSpeeds, unsigned long **speedList);
	void PMInstantiatePowerDomains(void);
	void PMRegisterDevice(IOService *theNub, IOService *theDevice);
    
public:
	bool start(IOService *provider);
	
	void registerNVRAMController(IONVRAMController *nvram);
	
	IOReturn callPlatformFunction(const OSSymbol *functionName,
					bool waitForFunction,
					void *param1, void *param2,
					void *param3, void *param4);
};

#endif