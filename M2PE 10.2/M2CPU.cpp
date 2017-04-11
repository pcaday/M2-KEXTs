// WARNING: This uses IOLog. In some places IOLog perhaps should not be used,
//  may hang, etc...

extern "C" {
	#include <ppc/proc_reg.h>
}

#include "M2CPU.h"
#include "M2.h"

#define VERBOSE 1

#if VERBOSE
    #define VerboseIOLog(x...) IOLog(x)
#else
    #define VerboseIOLog(x...) {}
#endif

#define super IOCPU

OSDefineMetaClassAndStructors(M2CPU, IOCPU);

bool M2CPU::start(IOService *provider)
{
    kern_return_t result;
    ml_processor_info_t processor_info;
    M2PE *platformExpert;
    unsigned int processorKind;
    
    if (!super::start(provider))
        return false;

    // Check if we should be running
    platformExpert = OSDynamicCast(M2PE, getPlatform());
    if (!platformExpert) {
        VerboseIOLog("M2CPU::start not on M2, exiting\n");
        return false;
    }

    cpuIC = new IOCPUInterruptController;
    if (cpuIC == 0)
        return false;

    if (cpuIC->initCPUInterruptController(1) != kIOReturnSuccess) return false;
    cpuIC->attach(this);

    cpuIC->registerCPUInterruptController();

    processor_info.cpu_id = (cpu_id_t) this;
    processor_info.boot_cpu = true;
    processor_info.start_paddr = 0x100;			// Wakes to reset vector
    processorKind = mfpvr() >> 16;
    
    // Retrieve L2CR for processors which are known to have L2: namely 750, 750*X
    if ((processorKind == 8) || ((processorKind & ~0x0002) == 0x7000))
	processor_info.l2cr_value = mfl2cr() & 0x7FFFFFFF;	// cache-disabled value (see GossamerCPU)
    else
	processor_info.l2cr_value = 0;
        
    processor_info.supports_nap = false;
    processor_info.time_base_enable = 0;

    // Register this CPU with mach.
    result = ml_processor_register(&processor_info, &machProcessor, &ipi_handler);
    if (result == KERN_FAILURE)
        return false;

    setCPUState(kIOCPUStateUninitalized);

    processor_start(machProcessor);

    registerService();
	
    return true;
}

void M2CPU::ipiHandler(void *refCon, void *nub, int source)
{
    // Call mach IPI handler for this CPU.
    if (ipi_handler)
        ipi_handler();
}

// boot = true:  we are booting
// boot = false: we are waking from sleep
void M2CPU::initCPU(bool boot)
{
    if (boot)
        cpuIC->enableCPUInterrupt(this);
    else {
        // Tweak memory controller (from OS 8.6) -- note the entire 0x5xxxxxxx segment is BAT-mapped 1-1
        *((volatile UInt8 *) 0x50F80007) |= 0x02;
    
        // Reset 68k interrupt level
        *((volatile UInt32 *) 0x50F2A000) = 0;
    
        // Restore time base after wake (since CPU's TBR was set to zero during sleep)
        restoreTB();
        
        // Restore VIA1/2 state
        saveRestoreVIA(false);

#if 0        
        // Make some noise via Singer
        singerNote();
#endif
    }

    setCPUState(kIOCPUStateRunning);
}

extern UInt32 ResetHandler;

// flushes the cache for a word at the given address.
#define cFlush(addr) __asm__ volatile("dcbf	0, %0" : : "r" (addr))

extern "C" {
    extern void cacheInit(void);
    extern void cacheDisable(void);
}

void M2CPU::quiesceCPU(void)
{
    ml_set_interrupts_enabled(false);
    
    // Tell PMU to sleep
    if (pmu)
        pmu->callPlatformFunction("sleepNow", false, 0, 0, 0, 0);
    
    // Save VIA1/2 state
    saveRestoreVIA(true);

    // Save time base before sleep since CPU's TBR will be set to zero at wake.
    saveTB();

    // Raise the 68k interrupt level to 7 (at the ICR)
    *((volatile UInt32 *) 0x50F2A000) = 0x38;
    
    // Tweak memory controller (from OS 8.6)
    *((volatile UInt8 *) 0x50F80007) &= ~0x02;

    ml_ppc_sleep();
}

const OSSymbol *M2CPU::getCPUName(void)
{
    return OSSymbol::withCStringNoCopy("Primary0");
}

kern_return_t M2CPU::startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr)
{
	return KERN_FAILURE;
}

void M2CPU::haltCPU(void)
{
	mach_timespec_t timeout;

//        VerboseIOLog("M2CPU::haltCPU entered\n");

	timeout.tv_sec = 1;
	timeout.tv_nsec = 0;
	
	// Find PMU now -- should not do this in quiesceCPU, which runs in interrupt context.
	pmu = waitForService(serviceMatching("ApplePMU"), &timeout);

	processor_exit(machProcessor); 
}

void M2CPU::saveTB(void)
{
	unsigned long tbHigh2;
	
	do {
		savedTBHigh = mftbu();
		savedTBLow = mftb();
		tbHigh2 = mftbu();
	} while (savedTBHigh != tbHigh2);
}

void M2CPU::restoreTB(void)
{
	mttb(0);
	mttbu(savedTBHigh);
	mttb(savedTBLow);
}

void M2CPU::saveRestoreVIA(bool save)
{
    volatile UInt8 *viaAddr = (volatile UInt8 *) 0x50F00000;
    
    if (save) {
        // Save VIA1
        viaSave[0] = viaAddr[0x0000];
        viaSave[1] = viaAddr[0x0400];
        viaSave[2] = viaAddr[0x1E00];
        viaSave[3] = viaAddr[0x0600];
        viaSave[4] = viaAddr[0x1C00] | 0x80;		// set 'enable' bit
        viaSave[5] = viaAddr[0x1600];
        viaSave[6] = viaAddr[0x1800];

        // Save VIA2
        viaAddr += 0x2000;
        viaSave[7] = viaAddr[0x0000];
        viaSave[8] = viaAddr[0x0400];
        viaSave[9] = viaAddr[0x1E00];
        viaSave[10] = viaAddr[0x0600];
        viaSave[11] = viaAddr[0x1C00] | 0x80;		// set 'enable' bit
        viaSave[12] = viaAddr[0x1600];
        viaSave[13] = viaAddr[0x1800];        
    } else {
        // Restore VIA1
        viaAddr[0x0000] = viaSave[0]; OSSynchronizeIO();
        viaAddr[0x0400] = viaSave[1]; OSSynchronizeIO();
        viaAddr[0x1E00] = viaSave[2]; OSSynchronizeIO();
        viaAddr[0x0600] = viaSave[3]; OSSynchronizeIO();
        viaAddr[0x1C00] = viaSave[4]; OSSynchronizeIO();
        viaAddr[0x1600] = viaSave[5]; OSSynchronizeIO();
        viaAddr[0x1800] = viaSave[6]; OSSynchronizeIO();
        
        // from ROM; starts one of the timers running, perhaps?
        viaAddr[0x1200] = viaAddr[0x1200]; OSSynchronizeIO();
        
        // clear VIA1 interrupts
//        viaAddr[0x1A00] = viaAddr[0x1A00];
        
        // Restore VIA2
        viaAddr += 0x2000;
        viaAddr[0x0000] = viaSave[7]; OSSynchronizeIO();
        viaAddr[0x0400] = viaSave[8]; OSSynchronizeIO();
        viaAddr[0x1E00] = viaSave[9]; OSSynchronizeIO();
        viaAddr[0x0600] = viaSave[10]; OSSynchronizeIO();
        viaAddr[0x1C00] = viaSave[11]; OSSynchronizeIO();
        viaAddr[0x1600] = viaSave[12]; OSSynchronizeIO();
        viaAddr[0x1800] = viaSave[13]; OSSynchronizeIO();
    }
}

#if 0
void M2CPU::singerNote(void)
{
    unsigned int i, j;
    volatile UInt16 *lfifo, *rfifo;
    volatile UInt8 *fifoEmpty;
    
    // Only proceed if we have a sound buffer
    if (!soundBufPhys)
        return;
    
    IODelay(1000);
    
    lfifo = (volatile UInt16 *) 0x50F15000;
    rfifo = (volatile UInt16 *) 0x50F15800;
    fifoEmpty = (volatile UInt8 *) 0x50F14804;
    
    // Initialize Singer
    *((volatile UInt8 *) 0x50F96000) = 0x96; OSSynchronizeIO();
    *((volatile UInt32 *) 0x50F14F44) = soundBufPhys; OSSynchronizeIO();
    *((volatile UInt16 *) 0x50F14F4A) = 0xFFC; OSSynchronizeIO();
    *((volatile UInt8 *) 0x50F14F40) = 0x21; OSSynchronizeIO();
    *((volatile UInt8 *) 0x50F14803) = 0x80; OSSynchronizeIO();
    *((volatile UInt8 *) 0x50F14803) = 0; OSSynchronizeIO();
    *lfifo = 0; OSSynchronizeIO();
    *rfifo = 0; OSSynchronizeIO();
    *((volatile UInt16 *) 0x50F14F48) = 0x400; OSSynchronizeIO();
    *((volatile UInt8 *) 0x50F14F09) = 1; OSSynchronizeIO();
    *((volatile UInt8 *) 0x50F14F29) = 1; OSSynchronizeIO();
    *((volatile UInt8 *) 0x50F1480A) = 0; OSSynchronizeIO();
    *((volatile UInt8 *) 0x50F14801) = 1; OSSynchronizeIO();
    *((volatile UInt16 *) 0x50F14F48) = 0; OSSynchronizeIO();
    *((volatile UInt16 *) 0x50F14F4A) = 8; OSSynchronizeIO();
    *((volatile UInt8 *) 0x50F14806) = 0x80; OSSynchronizeIO();

    IODelay(250);
    
    for (j = 0; j < 0x10; j++) {
        for (i = 0; i < 0x100; i++) {
            *lfifo = i * 0x0101; OSSynchronizeIO();
            *rfifo = i * 0x0101; OSSynchronizeIO();
        }
        OSSynchronizeIO();
        while (!(*fifoEmpty & 8))
            OSSynchronizeIO();
    }
}

#endif