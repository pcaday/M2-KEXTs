#ifndef _M2_CPU_H
#define _M2_CPU_H

#include <IOKit/IOCPU.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>

class M2CPU : public IOCPU
{
	OSDeclareDefaultStructors(M2CPU)
public:
	bool start(IOService *provider);
	void initCPU(bool boot);
	void quiesceCPU(void);
	const OSSymbol *getCPUName(void);
	kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr);
	void haltCPU(void);
        
protected:
	IOService *pmu;
	IOCPUInterruptController *cpuIC;
	unsigned long savedTBLow, savedTBHigh;		// saved timebase for sleep
	UInt8 viaSave[2 * 7];				// saved VIA registers for sleep
        
	void ipiHandler(void *refCon, void *nub, int source);
	void saveTB(void);
        void restoreTB(void);
        void saveRestoreVIA(bool save);
};

#endif