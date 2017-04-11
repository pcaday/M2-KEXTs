#define VERBOSE 1

#if VERBOSE
    #define Verbose_IOLog(x...)		IOLog(x)
#else
    #define Verbose_IOLog(x...)
#endif