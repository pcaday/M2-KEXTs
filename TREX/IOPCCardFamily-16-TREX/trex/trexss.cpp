#include <IOKit/pccard/config.h>
#include <IOKit/pccard/k_compat.h>

#define IN_CARD_SERVICES
#include <IOKit/pccard/version.h>
#include <IOKit/pccard/cs_types.h>
#include <IOKit/pccard/ss.h>
#include <IOKit/pccard/cs.h>

#include "TREXRegisters.h"
#include "trexss.h"


extern "C" {

MODULE_AUTHOR("");
MODULE_DESCRIPTION("TREX PCMCIA socket driver");

#ifdef PCMCIA_DEBUG
#define pc_debug i82365_debug
extern int pc_debug;
MODULE_PARM(pc_debug, "i");
#define DEBUG(n, args...) if (pc_debug>(n)) printk(KERN_DEBUG args)
#else
#define DEBUG(n, args...) do { } while (0)
#endif

#define VERBOSE 1


#if VERBOSE
    #define VerboseIOLog(x...) IOLog(x)
#else
    #define VerboseIOLog(x...) { }
#endif

#define min_io_timing 120	// minimum cycle time in nanoseconds for I/O accesses, 0 = none
#define min_mem_timing 0	// minimum cycle time in nanoseconds for memory accesses, 0 = none

typedef signed char s_char;
typedef signed short s_short;
typedef signed int s_int;

typedef struct {
    volatile u_char *regs;
    u_char flags;
    u_char new_events;
    socket_cap_t cap;
    void		(*handler)(void *info, u_int events);
    void		*info;
    pccard_mem_map mmap[5];
    pccard_io_map imap[2];
    void *mem_win_base;
    void *io_win_base;
} TREXSocket;

// Values for TREXSocket.flags
#define IS_POWERON		0x01
#define IS_IO_INTF		0x02
#define CAN_MOVE_MEM_WINDOWS	0x04	/* after dev config, memory windows can't be moved. This is OK because only RequestConfiguration/ReleaseConfiguration are called on sleep/wakeup and they don't alter memory windows */
#define IS_IO_IRQ_EN		0x08


#define max_sockets 2
static unsigned int trex_sockets;
TREXSocket trex_socket[max_sockets];

#define trex_cycle_time 40	// in ns



static int trex_register_callback(TREXSocket *s, ss_callback_t *call);
static int trex_inquire_socket(TREXSocket *s, socket_cap_t *cap);
static int trex_get_status(TREXSocket *s, u_int *value);
static int trex_get_socket(TREXSocket *s, socket_state_t *state);
static int trex_set_socket(TREXSocket *s, socket_state_t *state);
static int trex_get_io_map(TREXSocket *s, struct pccard_io_map *io);
static int trex_set_io_map(TREXSocket *s, struct pccard_io_map *io);
static int trex_get_mem_map(TREXSocket *s, struct pccard_mem_map *mem);
static int trex_set_mem_map(TREXSocket *s, struct pccard_mem_map *mem);


typedef int (*subfn_t)(TREXSocket *, void *);

static subfn_t trex_service_table[] = {
    (subfn_t)&trex_register_callback,
    (subfn_t)&trex_inquire_socket,
    (subfn_t)&trex_get_status,
    (subfn_t)&trex_get_socket,
    (subfn_t)&trex_set_socket,
    (subfn_t)&trex_get_io_map,
    (subfn_t)&trex_set_io_map,
    (subfn_t)&trex_get_mem_map,
    (subfn_t)&trex_set_mem_map,
#if 0
    (subfn_t)&cb_get_bridge,
    (subfn_t)&cb_set_bridge,
#else
    NULL, NULL,
#endif
};

#define NFUNC (sizeof(trex_service_table)/sizeof(subfn_t))

// ----------------------


inline u_char trex_get(TREXSocket *s, int which)
{
    return (s->regs)[which];
}

inline void trex_set(TREXSocket *s, int which, u_char val)
{
    (s->regs)[which] = val;
}

void trex_dump_regs(TREXSocket *s)
{
    unsigned int i;

    VerboseIOLog("trex socket services: socket regs:");
    for (i = 0; i <= 0xA; i++)
        VerboseIOLog(" %02X", trex_get(s, i));

    VerboseIOLog("\n");
}

static u_int
trex_interrupt_top(u_int socket_index)
{
    u_char hw_events, inputs;
    u_int events = 0;
    TREXSocket *s = &trex_socket[socket_index];

    if (!s->handler) return 0;

    hw_events = s->new_events;
    s->new_events = 0;

    inputs = trex_get(s, rInputs);
    
    if (hw_events & mCD)
        events |= SS_DETECT;
    
    if (mCardPresent(inputs)) {
        if (s->flags & IS_IO_INTF) {
            if (hw_events & mSTSCHG)
                events |= SS_STSCHG;
        } else {
            if (hw_events & mBVD) {                
                if (~inputs & mBVD1)
                    events |= SS_BATDEAD;
                else if (~inputs & mBVD2)
                    events |= SS_BATWARN;
            }
            if (hw_events & mRDYBSY)
                events |= SS_READY;		// = change in RDYBSY
        }
    }
    
    if (events)
	s->handler(s->info, events);

    VerboseIOLog("trex socket services: trex_interrupt_top(%d) events = %02X, pins = %02X\n", socket_index, hw_events, inputs);

    return 0;
}

static u_int
trex_interrupt_bottom(u_int socket_index)
{
    TREXSocket *skt;
    u_char chgd;
    
    skt = &trex_socket[socket_index];
    
    // Is this socket interrupting?
    if (!(trex_get(skt, rIntFlag) & mIntFlag))
        return 0;
    
    // Read and clear enabled interrupts
    chgd = trex_get(skt, rEvents) & trex_get(skt, rIntEnable);
    OSSynchronizeIO();
    trex_set(skt, rEvents, ~chgd);
    OSSynchronizeIO();
    
    if (skt->flags & IS_IO_INTF)
        chgd &= ~mIRQ;		// mIRQ is the same as mRDYBSY -- we don't want to send false
                                //  RDYBSY events to trex_interrupt_top
    
    // MACOSXXX grab lock
    skt->new_events |= chgd;
    // MACOSXXX drop lock

    return (chgd & (mBVD | mCD | mRDYBSY)) != 0;
}

// enable/disable IRQ: OK not to lock around these
//  because all other changes to the interrupt enable register
//  happen on the workloop 
static u_int
trex_enable_functional_interrupt(u_int socket_index)
{
    TREXSocket *s;
    
    // remember not to put IOLog in this function
    s = &trex_socket[socket_index];
    
    s->flags |= IS_IO_IRQ_EN;
    if (s->flags & IS_IO_INTF)
        trex_set(s, rIntEnable, trex_get(s, rIntEnable) | mIRQ);
    
    return 0;
}

static u_int
trex_disable_functional_interrupt(u_int socket_index)
{
    TREXSocket *s;
    
    s = &trex_socket[socket_index];

    // remember not to put IOLog in this function
    s->flags &= ~IS_IO_IRQ_EN;
    if (s->flags & IS_IO_INTF)
        trex_set(s, rIntEnable, trex_get(s, rIntEnable) & ~mIRQ);
    
    return 0;
}

static int trex_service(u_int sock, u_int cmd, void *arg)
{
    TREXSocket *s = &trex_socket[sock];
    subfn_t fn;
    int ret;
    
    DEBUG(2, "trex socket services: service(%d, %d, 0x%p)\n", sock, cmd, arg);

    if (cmd >= NFUNC)
	return -EINVAL;

    fn = trex_service_table[cmd];
#ifdef __FOO /* CONFIG_CARDBUS */
    if ((s->flags & IS_CARDBUS) &&
	(cb_readl(s, CB_SOCKET_STATE) & CB_SS_32BIT)) {
	if (cmd == SS_GetStatus)
	    fn = (subfn_t)&cb_get_status;
	else if (cmd == SS_GetSocket)
	    fn = (subfn_t)&cb_get_socket;
	else if (cmd == SS_SetSocket)
	    fn = (subfn_t)&cb_set_socket;
    }
#endif

    ret = (fn == NULL) ? -EINVAL : fn(s, arg);
    return ret;
}

static int trex_register_callback(TREXSocket *s, ss_callback_t *call)
{
    if (call == NULL) {
	s->handler = NULL;
    } else {
    	s->handler = call->handler;
	s->info = call->info;
    }
    
    return 0;
}

static int trex_inquire_socket(TREXSocket *s, socket_cap_t *cap)
{
    *cap = s->cap;
    return 0;
}

static int trex_get_status(TREXSocket *s, u_int *value)
{
    u_char inputs;
    u_int status = 0;
    
    inputs = trex_get(s, rInputs);

    if (mCardPresent(inputs)) {		status |= SS_DETECT;
    
        if (s->flags & IS_IO_INTF) {
            if (~inputs & mSTSCHG)	status |= SS_STSCHG;
        } else {
            if (~inputs & mBVD1)	status |= SS_BATDEAD;
            if (~inputs & mBVD2)	status |= SS_BATWARN;
            if (inputs & mWP)		status |= SS_WRPROT;
            if (inputs & mRDYBSY)	status |= SS_READY;
        }
        
        if (s->flags & IS_POWERON) {
            status |= SS_POWERON;
            s->flags &= ~IS_POWERON;
        }
    }
    
    *value = status;
    
    return 0;
}

static int trex_get_socket(TREXSocket *s, socket_state_t *state)
{
    unsigned int temp = 0;
    u_char reg;
    
    // - state->flags
    // SS_AUTO_PWR, i.e., auto-power-up of sockets is not used by cs.c currently

    reg = trex_get(s, rCfg0);
    
    if (reg & mOutput)		temp |= SS_OUTPUT_ENA;
    if (reg & mResetSignal)	temp |= SS_RESET;
    if (s->flags & IS_IO_INTF)	temp |= SS_IOCARD;
    // other flags may be in rCfg0?
    
    state->flags = temp;
    
    // - state->csc_mask
    temp = 0;
    reg = trex_get(s, rIntEnable);
    
    if (reg & mCD)		temp |= SS_DETECT;
    
    if (s->flags & IS_IO_INTF) {
        if (reg & mSTSCHG)	temp |= SS_STSCHG;
    } else {
        if (reg & mBVD)		temp |= SS_BATDEAD | SS_BATWARN;
        if (reg & mRDYBSY)	temp |= SS_READY;
    }
    
    state->csc_mask = temp;
    
    reg = trex_get(s, rCfg1);

    // - state->Vcc
    temp = 0;
    if (reg & mVcc5V)
        temp = 50;
        
    state->Vcc = temp;
    
    // - state->Vpp
    switch (reg & mVppMask) {
        case mVpp5V_14_15:
            temp = 50;
            break;
        case mVpp12V_14_15:
            temp = 120;
            break;
        default:
            temp = 0;
            break;
    }
    
    state->Vpp = temp;
    
    state->io_irq = 0;
    
    return 0;
}

// Disables all TREX ints but mInsert and mAccessErr, returns new interrupt mask
unsigned char trex_ints_off(TREXSocket *s)
{
    boolean_t istate;
    unsigned char reg;
    
    istate = ml_set_interrupts_enabled(false);
    
    reg = trex_get(s, rIntEnable) & (mInsert | mAccessErr);
    OSSynchronizeIO();
    trex_set(s, rIntEnable, reg);
    OSSynchronizeIO();

    ml_set_interrupts_enabled(istate);
    
    return reg;
} 

static int trex_set_socket(TREXSocket *s, socket_state_t *state)
{
    u_char reg, reg2;
    u_int csc;
    bool intf_chgd;
    
    // - state->flags

    reg = trex_get(s, rCfg0) & ~(mOutput | mResetSignal);
    
    if (state->flags & SS_OUTPUT_ENA)	reg |= mOutput;
    if (state->flags & SS_RESET) {
        reg |= mResetSignal; s->flags = (s->flags | CAN_MOVE_MEM_WINDOWS) & ~IS_IO_IRQ_EN;
    }
    if (state->flags & SS_DMA_MODE)
        return -EINVAL;
    
    reg |= mMasterIntEnable;
    
    // Change interface?
    intf_chgd = 0;
    if ((s->flags & IS_IO_INTF) && !(state->flags & SS_IOCARD)) {
        intf_chgd = true;
        
        // Switch to memory interface:
        (void) trex_ints_off(s);
        
        reg &= ~mSetForIO & ~mAttr;
    } else if (!(s->flags & IS_IO_INTF) && (state->flags & SS_IOCARD)) {
        intf_chgd = true;
        
        // Switch to I/O interface
        (void) trex_ints_off(s);
        
        reg = (reg | mSetForIO) & ~mAttr;
        
        // Not sure what the following is for; ROM Socket Services does it. Let's do the same.
        reg2 = trex_get(s, 8);
        reg2 |= 0x04;
        trex_set(s, 8, reg2);        
        OSSynchronizeIO();
        
        reg2 = trex_get(s, rMemTiming);
        reg2 &= mMemTimingHighBits | mMemSpeedMask;
        trex_set(s, rMemTiming, reg2);
        OSSynchronizeIO();
    }

    // Update s->flags & SS_IO_INTF
    if (state->flags & SS_IOCARD)
        s->flags |= IS_IO_INTF;
    else
        s->flags &= ~IS_IO_INTF;

    trex_set(s, rCfg0, reg);
    OSSynchronizeIO();
    
    if (intf_chgd) {
        trex_set(s, rEvents, 0);
        OSSynchronizeIO();
    }
    
    // - state->csc_mask
    csc = state->csc_mask;

    reg = trex_ints_off(s);
    
    // It looks like Card Services never enables any of these...
    if (mCardPresent(trex_get(s, rInputs)))
        csc = (u_int) -1;	// if we have a card, enable all interrupts
    else
        csc = SS_DETECT;	// always enable detect

    if (csc & SS_DETECT)			reg |= mCD;
    
    if (s->flags & IS_IO_INTF) {
        if (csc & SS_STSCHG)			reg |= mSTSCHG;
    } else {
        if (csc & (SS_BATDEAD | SS_BATWARN))	reg |= mBVD;
        if (csc & SS_READY)			reg |= mRDYBSY;
    }

//    if ((state->flags & SS_IOCARD) && (s->flags & IS_IO_IRQ_EN))
    // TREXInterruptController (from TREXPseudoPCIBridge) sets or clears the mInsert bit,
    //  indicating when the I/O IRQ has been enabled.
    if ((state->flags & SS_IOCARD) && (reg & mInsert))
        reg |= mIRQ;
    
    trex_set(s, rIntEnable, reg);
    
    // - state->Vcc and state->Vpp
    reg2 = trex_get(s, rCfg1);
    reg = reg2 & ~(mVcc5V | mVppMask);
    
    switch (state->Vcc) {
        case 0:
            break;
        case 50:
            reg |= mVcc5V;
            break;
        default:
            return -EINVAL;
            break;
    }
    
    switch (state->Vpp) {
        case 0:
            break;
        case 50:
            reg |= mVpp5V_14_15;
            break;
        case 120:
            reg |= mVpp12V_14_15;
            break;
        default:
            return -EINVAL;
            break;
    }
    
    trex_set(s, rCfg1, reg);
    if (reg & ~reg2 & mVcc5V)		// Are we turning on Vcc? If so, flag it for
        s->flags |= IS_POWERON;		//  trex_get_status
        
    VerboseIOLog("after trex_set_socket, ");
    trex_dump_regs(s);
    
    OSSynchronizeIO();
    
    return 0;
}

static int trex_get_io_map(TREXSocket *s, struct pccard_io_map *io)
{
    if (io->map >= 2)
        return -EINVAL;
    
    // Copy from the cache
    *io = s->imap[io->map];
    io->flags |= MAP_USE_WAIT;					// We always do this
    io->flags &= ~(MAP_0WS | MAP_PREFETCH | MAP_WRPROT);	//  and never do this
    
    return 0;
}

static int trex_set_io_map(TREXSocket *s, struct pccard_io_map *io)
{
    u_int map, slowest, cycles;
    
    if (io->map >= 2)
        return -EINVAL;
    if ((io->start > 0xffff) || (io->stop > 0xffff) ||
	(io->stop < io->start)) return -EINVAL;
    
    s->imap[io->map] = *io;

    // Recompute socket-global I/O timing
    
    slowest = min_io_timing; 
    for (map = 0; map < 2; map++) {
        if (s->imap[map].flags & MAP_ACTIVE) {
            slowest = max(slowest, s->imap[map].speed);
        }
    }
    
    // TREX I/O timing periods: 1 = 1 cycle; 2 = 3 cycles; 3 = 5 cycles, etc.; 0 = 31 cycles
    cycles = 1 + (((slowest + trex_cycle_time - 1) / trex_cycle_time) >> 1);

    if (cycles > (mIOSpeedMask + 1)) {
        s->imap[map].flags = 0;
        DEBUG(1, "trex_set_io_map: speed too slow\n");
        return -EINVAL;
    }

    cycles &= mIOSpeedMask;
    trex_set(s, rIOTiming, mIOTimingHighBits | cycles);

    OSSynchronizeIO();

    return 0;
}

static int trex_get_mem_map(TREXSocket *s, struct pccard_mem_map *mem)
{
    if (mem->map >= 5)
        return -EINVAL;
    
    // Copy from the cache
    *mem = s->mmap[mem->map];
    mem->flags &= ~(MAP_USE_WAIT | MAP_0WS | MAP_WRPROT);
    
    return 0;
}


static int trex_set_mem_map(TREXSocket *s, struct pccard_mem_map *mem)
{
    u_int len;
    u_char this_map = mem->map;
    u_char cfg0;
    
    cfg0 = trex_get(s, rCfg0) & ~mAttr;
    
    // We watch out for a special signal (a mem->map value of -1) from the modified
    //  CIS reading code that tells us it's done with attribute memory.
    if ((s_char) this_map != -1) {
        // Typical call
        if (this_map >= 5)
            return -EINVAL;
    
        if (mem->flags & MAP_ACTIVE) {    
            if (!this_map) {
                if (mem->flags & MAP_ATTRIB)
                    cfg0 |= mAttr;    		// This is the only place in the code attribute memory is enabled     
            } else {
                if (mem->flags & MAP_ATTRIB)	// Attribute memory windows not allowed
                    return -EINVAL;		//  except for CIS reading/writing
                
                if (!(s->flags & CAN_MOVE_MEM_WINDOWS))
                    if (mem->card_start != s->mmap[this_map].card_start)
                        return -EINVAL;
            }
        }

        len = mem->sys_stop - mem->sys_start;
        mem->sys_start = (u_long) ((u_char *) s->mem_win_base + mem->card_start);
        mem->sys_stop = mem->sys_start + len;
//        IOLog("trex_set_mem_map: card_start = %x, sys_start = %x\n", mem->card_start, mem->sys_start);
    }
    
    trex_set(s, rCfg0, cfg0);
    OSSynchronizeIO();
    
    // Recompute socket-global memory timing
    u_int map, slowest, cycles;

    s->mmap[this_map] = *mem;
    
    // Include map 0 in timing only if reading attribute mem
    map = !(cfg0 & mAttr);
    slowest = min_mem_timing;
    for (; map < 5; map++) {
        if (s->mmap[map].flags & MAP_ACTIVE) {
            slowest = max(slowest, s->mmap[map].speed);
        }
    }
    
    cycles = (slowest + trex_cycle_time - 1) / trex_cycle_time;
    if (cycles == 0)
        cycles = 1;
    if (cycles > (mMemSpeedMask + 1)) {
        s->mmap[map].flags = 0;
        DEBUG(1, "trex_set_mem_map: speed too slow\n");
        return -EINVAL;
    }

    cycles &= mMemSpeedMask;
    trex_set(s, rMemTiming, mMemTimingHighBits | cycles);
            
    return 0;
}

int trex_get_ranges(unsigned int socket_number, unsigned long *range_list, unsigned int *num_ranges)
{
    unsigned int map, nmaps = 0;
    TREXSocket *s;
    unsigned long io_base;
    
    if (socket_number >= max_sockets) {
        VerboseIOLog("trex_get_ranges: bad socket_number\n");
        return -EINVAL;
    }
    
    s = &trex_socket[socket_number];

    VerboseIOLog("trex_get_ranges: ");
    
    // Record memory windows, but not CIS window
    for (map = 1; map < 5; map++) {
        if (s->mmap[map].flags & MAP_ACTIVE) {
            VerboseIOLog("mem%u:%x-%x  ", map, s->mmap[map].sys_start, s->mmap[map].sys_stop);
            *range_list++ = (unsigned long) s->mmap[map].sys_start;
            *range_list++ = (unsigned long) s->mmap[map].sys_stop;
            nmaps++;
        }
    }
    
    // Record I/O windows
    io_base = (unsigned long) s->io_win_base;
    
    for (map = 0; map < 2; map++) {
        if (s->imap[map].flags & MAP_ACTIVE) {
            VerboseIOLog("io%u:%x-%x  ", map, s->imap[map].start + io_base, s->imap[map].stop + io_base);
            *range_list++ = (unsigned long) s->imap[map].start + io_base;
            *range_list++ = (unsigned long) s->imap[map].stop + io_base;
            nmaps++;
        }
    }

    VerboseIOLog("\n");
    *num_ranges = nmaps;
    
    // Now that we have given out physical addresses, we disallow
    //  further movement of the memory windows
    s->flags &= ~CAN_MOVE_MEM_WINDOWS;
    
    trex_dump_regs(s);
    
    return 0;
}

static void trex_add_socket(IOPCCardBridge *pccard_nub, IOPCIDevice *bridge_nub, volatile u_char *regs, void *mem_base, void *io_base)
{
    TREXSocket *s;
    
    s = &trex_socket[trex_sockets++];
    
    s->cap.features = SS_CAP_STATIC_MAP | SS_CAP_PCCARD | SS_CAP_CARDBUS;	// TREX can't really do CardBus...
    s->cap.cardbus = 0;
    s->cap.irq_mask = 0xFFFF;
    s->cap.pci_irq = 0;
    s->cap.pccard_nub = pccard_nub;
    s->cap.bridge_nub = bridge_nub;
    s->cap.map_size = 0x400;		// adjustable; this should be a good value
    
    s->regs = regs;
    s->flags = CAN_MOVE_MEM_WINDOWS;
    s->mem_win_base = mem_base;
    s->io_win_base = io_base;
    
    trex_set(s, rReset, 0);
    IOSleep(1);
    trex_set(s, rEvents, 0);		// after reset, TREX signals a card-insertion event
                                        //  if the socket has a card in it. We don't want this
                                        //  event.
    
    // Default timings for memory and I/O accesses
    trex_set(s, rMemTiming, mMemTimingHighBits | 0x08);
    trex_set(s, rIOTiming, mIOTimingHighBits | 0x05);
}

int
init_trex(IOPCCardBridge *pccard_nub, IOPCIDevice *bridge_nub, void *device_regs, void *socket_windows)
{
    TREXSocket *s;
    unsigned int starting_socket = trex_sockets;
    unsigned int i;
    
    // for macosx this is all evil, the original code assumes it is
    // called once during init, and all the "sockets" are found at
    // once, unfortunately it doesn't work that way in macosx. in
    // macos9 there is the concept of logical socket indexes, CS deals
    // in logical sockets and SS deals in physical sockets.  This
    // makes the code equally ugly due the constant need to convert
    // back and forth between physical and logical.  For now at least,
    // this code needs make sure that the indexes in DS, CS, SS all
    // refer to same sockets.  The bridge object insures that this
    // code and the ds registration code get called synchronously.
    // this code registers with CS via register_ss_entry

    if ((starting_socket + 1) > max_sockets)
        return eTREXNoMoreSockets;
    
    trex_add_socket(pccard_nub, bridge_nub, (volatile u_char *) device_regs, socket_windows, (u_char *) socket_windows + woIOWin);
//    trex_add_socket(pccard_nub, bridge_nub, ((volatile u_char *) device_regs) + sockOffStep, ((u_char *) socket_windows) + woSocket1, ((u_char *) socket_windows) + woSocket1 + woIOWin);
    
    /* Set up interrupt handlers */
    for (i = starting_socket; i < trex_sockets; i++) {
        s = &trex_socket[i];
        IOPCCardAddCSCInterruptHandlers(s->cap.pccard_nub, i, 0,
                                        trex_interrupt_top, trex_interrupt_bottom, 
                                        trex_enable_functional_interrupt,
                                        trex_disable_functional_interrupt, 
                                        "trex");
    }
    
    if (register_ss_entry(starting_socket, trex_sockets, &trex_service) != 0) {
	printk(KERN_NOTICE "trex socket services: register_ss_entry() failed\n");
        return eTREXRegisterFailed;
    }

    return 0;
}

}	/* extern "C" */
