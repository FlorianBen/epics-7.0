/******************************************************************************
 * CEA - Direction des Sciences de la Matiere - IRFU/SIS
 * CE-SACLAY, 91191 Gif-sur-Yvette, France
 *
 * $Id: devIcv150.c 23 2013-03-13 15:38:34Z lussi $
 *
 * ADAS ICV150 Device Support
 *
 * who       when       what
 * --------  --------   ----------------------------------------------
 * jgournay  08/05/93   created
 * fgougnaud 01/06/97   added gain control
 * jhosselet 07/12/06   updated for 3.14
 * ylussign  09/10/07   removed all warnings
 * ylussign  19/10/07   added config & service functions
 *                      added icv150.h
 *                      added 16bit ADC support
 *                      ADC resolution configured by function call
 *                      scan mode selectable by function call
 * ylussign  09/11/07   field DPVT used to mark bad records
 * ylussign  03/07/08   - changed icv150 functions type to void
 *                      - added icv150 functions registration for iocsh
 *                      - use Device Support Library devLib
 *                      - added RTEMS support
 * ylussign  24/11/09   - added include errlog.h
 *                      - added doxygen documentation
 * ylussign  29/01/10   added function icv150OverSampling
 * ylussign  04/07/11   - changed max number of signals to 128
 *                      - lastChan & trigCnt changed to arrays
 * ylussign  13/03/13   added waveform record support
 */

/** 
 * @file
 * @brief ADAS ICV150 device support for EPICS R3.14
 *
 * The ADAS ICV150 is a 12/14/16-bit multiplexed ADC board with 32
 * input signals. Only boards configured with differential inputs
 * are supported. The number of signals be extended up to 128
 * using 48 signals extension boards ICV110. The number of signals
 * can be set by calling the function icv150CfgScan() @b before @b iocInit.
 * The default number of signals is 32.
 * 
 * ICV150 Device Support accepts up to 4 boards in a VME crate, starting 
 * from address @b 0x500000 with an increment of 0x1000. Each ICV150 uses
 * an interrupt vector, starting from 0xC0 for the board 0.
 * 
 * The device supports 12, 14 or 16 bit ADC resolution. The resolution must
 * be configured by calling the function icv150CfgAdc() @b before @b iocInit. 
 * The default resolution is 16 bit.
 * 
 * Since hardware revision J, 16 bit boards have an oversampling mode.
 * This mode can be configured by calling the function icv150OverSampling().
 * 
 * Signals may be scanned automatically (default) or on an external trigger
 * on J3. On external trigger, signals are scanned once and an interrupt
 * is generated at the end of conversion. A database event is generated by 
 * the interrupt service routine to allow records processing.
 * The automatic scanning can be set by calling the function icv150CfgAutoScan(). 
 * The scanning on external trigger can be set by calling the function
 * icv150CfgExtTrig(). A soft trigger can be generated by calling the function
 * icv150SoftTrig().
 * 
 * There are two different ways to control the gain of an input signal:
 * - using an ICV150 AO record
 * - by calling the configuration function icv150CfgGain().
 * 
 * The gains can be saved in NOVRAM by calling the function icv150StoreGains().
 *
 * @section Record-Support Record Support
 * 
 * The device supports @b AI and @b WAVEFORM record types for signal input
 * and @b AO record type for signal gain control. The device type DTYP is
 * @b ICV150 for all record types.
 * 
 * A WAVEFORM record may contain the following type of data (FTVL): USHORT,
 * LONG, ULONG, FLOAT, DOUBLE. For types USHORT, LONG and ULONG the waveform
 * contains raw ADC data. For types FLOAT and DOUBLE the waveform contains
 * raw ADC data if LOPR is equal to HOPR; else the ADC data are converted
 * according to the following formula:
 *
 * VAL = RAW * (HOPR - LOPR) / RAWF + LOPR
 *
 * where RAW is the ADC value and RAWF is the highest ADC value.
 */

#ifdef vxWorks
#include <vxWorks.h>
#include <vxLib.h>
#include <sysLib.h>
#include <taskLib.h>
#include <intLib.h>
#include <iv.h>
#include <vme.h>
#include <types.h>
#include <stdioLib.h>
#endif

#ifdef __rtems__
#include <rtems.h>
#include <bsp/VME.h>
#include <bsp/bspExt.h>
#define OK     0
#define ERROR  (-1)
#define taskDelay(a) (rtems_task_wake_after(a));
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alarm.h>
#include <cvtTable.h>
#include <dbDefs.h>
#include <dbAccess.h>
#include <recGbl.h>
#include <recSup.h>
#include <devSup.h>
#include <link.h>
#include <aiRecord.h>
#include <aoRecord.h>
#include <waveformRecord.h>
#include <dbScan.h>
#include <epicsExport.h>
#include <iocsh.h>
#include <devLib.h>
#include <errlog.h>

#include "icv150.h"

/* VME ICV150 defines */

#define ICV150_BASE (char *)0x500000	/* VME base address */
#define ICV150_SIZE           0x1000	/* VME memory length */
#define MAX_ICV150_CARDS           4	/* max. number of boards in a VME crate */
#define ICV150_MAXCHAN           128	/* number of differential signals (32+48+48)*/
#define IT_LEVEL                   2	/* Interrupt level */
#define IT_VECTOR               0xC0	/* Interrupt vector for board 0 */
#define IT_ENABLE               0x01	/* board interrupt enable bit */
#define RD_GAIN                0xE00	/* (RD) signal 0 gain code */
#define CS_SCAN                0xC00	/* (WR) last signal number to scan */
#define CS_OVERSAMPLING        0xA00	/* (WR) oversampling mode */
#define CS_WIT                 0x800	/* (WR) interrupt register */
#define CS_RIT                 0x800	/* (RD) end of conversion state */
#define CS_STORE               0x700	/* (WR) store gain codes in NOVRAM */
#define CS_EXT                 0x500	/* (WR) enable external trigger */
#define CS_TRIG                0x400	/* (WR) soft trigger */
#define CS_STOP                0x300	/* (WR) stop scanning */
#define CS_SCANNING            0x300	/* (RD) scanning state */
#define CS_START               0x200	/* (WR) start scanning */

static char *icv150[MAX_ICV150_CARDS] = {0,0,0,0};      /* VME address */
static unsigned short masks[MAX_ICV150_CARDS] =
	{0xffff, 0xffff, 0xffff, 0xffff};               /* ADC resolution mask */
static int events[MAX_ICV150_CARDS] = {0,0,0,0};        /* interrupt event */
static int autoScan[MAX_ICV150_CARDS] = {1,1,1,1};      /* ADC automatic scanning (1=on, 0=off) */
static int lastChan[MAX_ICV150_CARDS] = {31,31,31,31};  /* last scanned signal */
static int trigCnt[MAX_ICV150_CARDS] = {0,0,0,0};       /* external trigs counter */

#define devMapAddr(a,b,c,d,e) ((pdevLibVirtualOS->pDevMapAddr)(a,b,c,d,e))


/*__________________________________________________________________
 *
 *	Service and configuration functions
 * 
 * These functions may be called from an application, from the shell
 * or from a startup script, after the iocInit, excepted icv150CfgAdc()
 * that must be called before iocInit.
 */

/**
 * @n
 * This IOC shell variable allows to print debug messages.
 * Valid range is:
 * - 0 no message is printed
 * - 1 messages at initialization are printed
 * - 2 initialization and I/O messages are printed
 */
int devIcv150Verbose = 0;
epicsExportAddress(int, devIcv150Verbose);

static char
*mapAddress (
	int card
	)
{
    char *icv150Addr;
    short dum;
    size_t vmeAddress = (size_t)(ICV150_BASE + card * ICV150_SIZE);

    if (devMapAddr (atVMEA24,
		    0,
		    vmeAddress,
		    0,
		   (volatile void **)&icv150Addr) != OK)
	return NULL;
    
    /*printf ("mapAddress: VME-adrs=0x%x CPU-adrs=0x%x\n", vmeAddress, (int)icv150Addr);*/
    
    if (devReadProbe (sizeof (short),
		     (volatile const void *)icv150Addr,
		     (void *)&dum) != OK)
	return NULL;
		      
    return icv150Addr;
}



/**
 * @n
 * This IOC shell function selects the ADC resolution.
 *
 * @note
 * This function must be called @b before @b iocInit.
 */

void
icv150CfgAdc (
	/** [in] ICV150 card number (valid range 0 to 3) */
	int card,
	/** [in] ADC resolution (valid range 12, 14 or 16 bits) */
	int resolution
	)
{
    char *icv150Addr;

    if ((icv150Addr = mapAddress (card)) == NULL)
    {
	printf ("icv150CfgAdc: missing card %d\n", card);
	return;
    }

    switch (resolution) {
    case 12:
	masks[card] = 0x0fff;
	break;
    case 14:
	masks[card] = 0x3fff;
	break;
    case 16:
	masks[card] = 0xffff;
	break;
    default:
	printf ("icv150CfgAdc: invalid resolution %d\n", resolution);
	return;
    }
    printf ("icv150CfgAdc: card %d done\n", card);
    
    return;
}

/* Information needed by iocsh */
static const iocshArg     icv150CfgAdcArg0 = {"card", iocshArgInt};
static const iocshArg     icv150CfgAdcArg1 = {"resolution", iocshArgInt};
static const iocshArg    *icv150CfgAdcArgs[] = {&icv150CfgAdcArg0, &icv150CfgAdcArg1};
static const iocshFuncDef icv150CfgAdcFuncDef = {"icv150CfgAdc", 2, icv150CfgAdcArgs};

/* Wrapper called by iocsh, selects the argument types that icv150CfgAdc needs */
static void icv150CfgAdcCallFunc(const iocshArgBuf *args) {
    icv150CfgAdc(args[0].ival, args[1].ival);
}

/* Registration routine, runs at startup */
static void icv150CfgAdcRegister(void) {
    iocshRegister(&icv150CfgAdcFuncDef, icv150CfgAdcCallFunc);
}
epicsExportRegistrar(icv150CfgAdcRegister);



/**
 * @n
 * This IOC shell function changes the number of signals scanned
 * on the board, starting from the first signal. It supersedes 
 * the default number of signals given by the straps on ST3.
 * The current acquisition mode is restarted.
 *
 * @note
 * This function must be called @b before @b iocInit.
 */

void
icv150CfgScan (
	/** [in] ICV150 card number (valid range 0 to 3) */
	int card,
	/** [in] number of signals to scan (valid range 1 to 128) */
	int signal
	)
{
    unsigned short *pReg;
    char *icv150Addr;

    if ((icv150Addr = mapAddress (card)) == NULL)
    {
	printf ("icv150CfgScan: missing card %d\n", card);
	return;
    }

    if ((signal < 1) || (signal > ICV150_MAXCHAN))
    {
	printf ("icv150CfgScan: invalid number of signals %d [1-%d]\n",
		signal, ICV150_MAXCHAN);
	return;
    }

    /* stop scanning */
    pReg = (unsigned short *)(icv150Addr + CS_STOP);
    *pReg = 0;
    taskDelay(1);

    /* set last signal# to scan */
    pReg = (unsigned short *)(icv150Addr + CS_SCAN);
    *pReg = lastChan[card] = signal - 1;

    /* restart scanning mode */
    if (autoScan[card])
	pReg = (unsigned short *)(icv150Addr + CS_START);
    else
	pReg = (unsigned short *)(icv150Addr + CS_EXT);
    *pReg = 0;
    
    printf ("icv150CfgScan: card %d done\n", card);
    
    return;
}

/* Information needed by iocsh */
static const iocshArg     icv150CfgScanArg0 = {"card", iocshArgInt};
static const iocshArg     icv150CfgScanArg1 = {"signal", iocshArgInt};
static const iocshArg    *icv150CfgScanArgs[] = {&icv150CfgScanArg0, &icv150CfgScanArg1};
static const iocshFuncDef icv150CfgScanFuncDef = {"icv150CfgScan", 2, icv150CfgScanArgs};

/* Wrapper called by iocsh, selects the argument types that icv150CfgScan needs */
static void icv150CfgScanCallFunc(const iocshArgBuf *args) {
    icv150CfgScan(args[0].ival, args[1].ival);
}

/* Registration routine, runs at startup */
static void icv150CfgScanRegister(void) {
    iocshRegister(&icv150CfgScanFuncDef, icv150CfgScanCallFunc);
}
epicsExportRegistrar(icv150CfgScanRegister);



/**
 * @n
 * This IOC shell function allows to enable/disable the oversampling mode.
 * 
 * @note
 * Oversampling is available since hardware revision J on 16 bit ADC
 * boards only.
 */

void icv150OverSampling (
	/** [in] ICV150 card number (valid range 0 to 3) */
	int card,
	/** [in] oversampling mode (valid range 0 = OFF, not 0 = ON) */
	int on
	)
{
    unsigned short *pReg;
    char *icv150Addr;

    if ((icv150Addr = mapAddress (card)) == NULL)
    {
	printf ("icv150OverSampling: missing card %d\n", card);
	return;
    }
   
    /* Oversampling ON/OFF */
    pReg = (unsigned short *)(icv150Addr + CS_OVERSAMPLING);
    if (on)
    {
	*pReg = 1;
	printf ("icv150OverSampling: card %d oversampling ON\n", card);
    }
    else
    {
	*pReg = 0;
	printf ("icv150OverSampling: card %d oversampling OFF\n", card);
    }
    
    return;
}

/* Information needed by iocsh */
static const iocshArg     icv150OverSamplingArg0 = {"card", iocshArgInt};
static const iocshArg     icv150OverSamplingArg1 = {"on", iocshArgInt};
static const iocshArg    *icv150OverSamplingArgs[] = {&icv150OverSamplingArg0, &icv150OverSamplingArg1};
static const iocshFuncDef icv150OverSamplingFuncDef = {"icv150OverSampling", 2, icv150OverSamplingArgs};

/* Wrapper called by iocsh, selects the argument types that icv150OverSampling needs */
static void icv150OverSamplingCallFunc(const iocshArgBuf *args) {
    icv150OverSampling(args[0].ival, args[1].ival);
}

/* Registration routine, runs at startup */
static void icv150OverSamplingRegister(void) {
    iocshRegister(&icv150OverSamplingFuncDef, icv150OverSamplingCallFunc);
}
epicsExportRegistrar(icv150OverSamplingRegister);



/**
 * @n
 * This IOC shell function changes the gain code value of an input signal.
 * The gain code is stored in the on board RAM. To make this change permanent,
 * it is necessary to store the gain codes in the on board NOVRAM by calling
 * the function icv150StoreGains(). The current acquisition mode is restarted.
 * @n
 * @n
 */
 
void
icv150CfgGain (
	/** [in] ICV150 card number (valid range 0 to 3) */
	int card,
	/** [in] signal number (valid range 0 to 31) */
	int signal,
	/** [in] gain code (valid range 0 to 15) */
	int gain
	)
{
    unsigned short *pReg;
    char *icv150Addr;

    if ((icv150Addr = mapAddress (card)) == NULL)
    {
	printf ("icv150CfgGain: missing card %d\n", card);
	return;
    }
    
    /* stop scanning */
    pReg = (unsigned short *)(icv150Addr + CS_STOP);
    *pReg = 0;
    taskDelay(1);
    
    /* write gain code in RAM */
    pReg = (unsigned short *)(icv150Addr + 2 * signal);
    *pReg = gain & 0x0f;
    
    /* restart scanning mode */
    if (autoScan[card])
	pReg = (unsigned short *)(icv150Addr + CS_START);
    else
	pReg = (unsigned short *)(icv150Addr + CS_EXT);
    *pReg = 0;

    printf ("icv150CfgGain: card %d signal %d gain=%d done\n",
	    card, signal, gain);

    return;
}

/* Information needed by iocsh */
static const iocshArg     icv150CfgGainArg0 = {"card", iocshArgInt};
static const iocshArg     icv150CfgGainArg1 = {"signal", iocshArgInt};
static const iocshArg     icv150CfgGainArg2 = {"gain", iocshArgInt};
static const iocshArg    *icv150CfgGainArgs[] = {&icv150CfgGainArg0, &icv150CfgGainArg1, &icv150CfgGainArg2};
static const iocshFuncDef icv150CfgGainFuncDef = {"icv150CfgGain", 3, icv150CfgGainArgs};

/* Wrapper called by iocsh, selects the argument types that icv150CfgGain needs */
static void icv150CfgGainCallFunc(const iocshArgBuf *args) {
    icv150CfgGain(args[0].ival, args[1].ival, args[2].ival);
}

/* Registration routine, runs at startup */
static void icv150CfgGainRegister(void) {
    iocshRegister(&icv150CfgGainFuncDef, icv150CfgGainCallFunc);
}
epicsExportRegistrar(icv150CfgGainRegister);



/**
 * @n
 * This IOC shell function stores the gain codes in NOVRAM.
 * The current acquisition mode is restarted.
 *
 * @note
 * The number of changes in NOVRAM is limited to 10000.
 * So you should avoid calling this function after each reboot.
 */
 
void
icv150StoreGains (
	/** [in] ICV150 card number (valid range 0 to 3) */
	int card
	)
{
    unsigned short *pReg;
    char *icv150Addr;

    if ((icv150Addr = mapAddress (card)) == NULL)
    {
	printf ("icv150StoreGains: missing card %d\n", card);
	return;
    }

    /* stop scanning */
    pReg = (unsigned short *)(icv150Addr + CS_STOP);
    *pReg = 0;
    taskDelay(1);
    
    /* store gains in NOVRAM */
    pReg = (unsigned short *)(icv150Addr + CS_STORE);
    *pReg = 0;
    
    /* wait */
    taskDelay(1);
    
    /* restart scanning mode */
    if (autoScan[card])
	pReg = (unsigned short *)(icv150Addr + CS_START);
    else
	pReg = (unsigned short *)(icv150Addr + CS_EXT);
    *pReg = 0;

    printf ("icv150StoreGains: card %d done\n", card);

    return;
}

/* Information needed by iocsh */
static const iocshArg     icv150StoreGainsArg0 = {"card", iocshArgInt};
static const iocshArg    *icv150StoreGainsArgs[] = {&icv150StoreGainsArg0};
static const iocshFuncDef icv150StoreGainsFuncDef = {"icv150StoreGains", 1, icv150StoreGainsArgs};

/* Wrapper called by iocsh, selects the argument types that icv150StoreGains needs */
static void icv150StoreGainsCallFunc(const iocshArgBuf *args) {
    icv150StoreGains(args[0].ival);
}

/* Registration routine, runs at startup */
static void icv150StoreGainsRegister(void) {
    iocshRegister(&icv150StoreGainsFuncDef, icv150StoreGainsCallFunc);
}
epicsExportRegistrar(icv150StoreGainsRegister);



/**
 * @n
 * This IOC shell function stops the current acquisition mode and enables
 * an external trigger on J3. In this mode, data are scanned once
 * on trigger and an interrupt is generated at the end of
 * conversion. A database event is generated by the interrupt
 * service routine to allow records processing.
 * @n
 * @n
 */

void
icv150CfgExtTrig (
	/** [in] ICV150 card number (valid range 0 to 3) */
	int card,
	/** [in] database event number (valid range 0 to 255) */
	int event
	)
{
    unsigned short *pReg;
    char *icv150Addr;

    if ((icv150Addr = mapAddress (card)) == NULL)
    {
	printf ("icv150CfgExtTrig: missing card %d\n", card);
	return;
    }
    
    if ((event < 0) || (event > 255))
    {
	printf ("icv150CfgExtTrig: invalid event value %d [0-255]\n", 
		event);
	return;
    }
    events[card] = event;
    
    /* stop scanning */
    pReg = (unsigned short *)(icv150Addr + CS_STOP);
    *pReg = 0;
    taskDelay(1);
    
    /* enable external trigger */
    pReg = (unsigned short *)(icv150Addr + CS_EXT);
    *pReg = 0;
    
    autoScan[card] = 0;
    trigCnt[card] = 0;
    
    printf ("icv150CfgExtTrig: card %d done\n", card);

    return;
}

/* Information needed by iocsh */
static const iocshArg     icv150CfgExtTrigArg0 = {"card", iocshArgInt};
static const iocshArg     icv150CfgExtTrigArg1 = {"event", iocshArgInt};
static const iocshArg    *icv150CfgExtTrigArgs[] = {&icv150CfgExtTrigArg0, &icv150CfgExtTrigArg1};
static const iocshFuncDef icv150CfgExtTrigFuncDef = {"icv150CfgExtTrig", 2, icv150CfgExtTrigArgs};

/* Wrapper called by iocsh, selects the argument types that icv150CfgExtTrig needs */
static void icv150CfgExtTrigCallFunc(const iocshArgBuf *args) {
    icv150CfgExtTrig(args[0].ival, args[1].ival);
}

/* Registration routine, runs at startup */
static void icv150CfgExtTrigRegister(void) {
    iocshRegister(&icv150CfgExtTrigFuncDef, icv150CfgExtTrigCallFunc);
}
epicsExportRegistrar(icv150CfgExtTrigRegister);



/**
 * @n
 * This IOC shell function stops the current acquisition mode and starts 
 * automatic scanning. In this mode, signals are permanently 
 * scanned and data are always available.
 * @n
 * @n
 */

void
icv150CfgAutoScan (
	/** [in] ICV150 card number (valid range 0 to 3) */
	int card
	)
{
    unsigned short *pReg;
    char *icv150Addr;

    if ((icv150Addr = mapAddress (card)) == NULL)
    {
	printf ("icv150CfgAutoScan: missing card %d\n", card);
	return;
    }
	
    /* stop scanning */
    pReg = (unsigned short *)(icv150Addr + CS_STOP);
    *pReg = 0;
    taskDelay(1);
    
    /* start automatic scanning */
    pReg = (unsigned short *)(icv150Addr + CS_START);
    *pReg = 0;
    
    autoScan[card] = 1;
    trigCnt[card] = 0;
    
    printf ("icv150CfgAutoScan: card %d done\n", card);
    
    return;
}

/* Information needed by iocsh */
static const iocshArg     icv150CfgAutoScanArg0 = {"card", iocshArgInt};
static const iocshArg    *icv150CfgAutoScanArgs[] = {&icv150CfgAutoScanArg0};
static const iocshFuncDef icv150CfgAutoScanFuncDef = {"icv150CfgAutoScan", 1, icv150CfgAutoScanArgs};

/* Wrapper called by iocsh, selects the argument types that icv150CfgAutoScan needs */
static void icv150CfgAutoScanCallFunc(const iocshArgBuf *args) {
    icv150CfgAutoScan(args[0].ival);
}

/* Registration routine, runs at startup */
static void icv150CfgAutoScanRegister(void) {
    iocshRegister(&icv150CfgAutoScanFuncDef, icv150CfgAutoScanCallFunc);
}
epicsExportRegistrar(icv150CfgAutoScanRegister);



/**
 * @n
 * This IOC shell function generates a software trigger only if external
 * trigger mode was selected by icv150CfgExtTrig().
 * @n
 * @n
 */

void
icv150SoftTrig (
	/** [in] ICV150 card number (valid range 0 to 3) */
	int card
	)
{
    unsigned short *pReg;
    char *icv150Addr;

    /* check scanning mode */
    if (autoScan[card])
    {
	printf ("icv150SoftTrig: call icv150CfgExtTrig before\n");
	return;
    }
    
    if ((icv150Addr = mapAddress (card)) == NULL)
    {
	printf ("icv150SoftTrig: missing card %d\n", card);
	return;
    }
    
    /* soft trigger */
    pReg = (unsigned short *)(icv150Addr + CS_TRIG);
    *pReg = 0;
    
    if (devIcv150Verbose)
	printf ("icv150SoftTrig: card %d done\n", card);

    return;
}

/* Information needed by iocsh */
static const iocshArg     icv150SoftTrigArg0 = {"card", iocshArgInt};
static const iocshArg    *icv150SoftTrigArgs[] = {&icv150SoftTrigArg0};
static const iocshFuncDef icv150SoftTrigFuncDef = {"icv150SoftTrig", 1, icv150SoftTrigArgs};

/* Wrapper called by iocsh, selects the argument types that icv150SoftTrig needs */
static void icv150SoftTrigCallFunc(const iocshArgBuf *args) {
    icv150SoftTrig(args[0].ival);
}

/* Registration routine, runs at startup */
static void icv150SoftTrigRegister(void) {
    iocshRegister(&icv150SoftTrigFuncDef, icv150SoftTrigCallFunc);
}
epicsExportRegistrar(icv150SoftTrigRegister);



/*__________________________________________________________________
 *
 *	AO Device Support (Gain)
 */

/*
 * Initialize AO record
 */

static long
init_ao_record (
	aoRecord *pao
	)
{
    struct vmeio *pvmeio;
    short gain;
    unsigned short *pReg;
    char *icv150Addr;

    pao->dpvt = (void *)0;
    
    switch (pao->out.type)
    {
    case (VME_IO):
	
	pvmeio = (struct vmeio *)&(pao->out.value);
	
	/*
	 * check card number
	 */
	if (pvmeio->card >= MAX_ICV150_CARDS) 
	{
	    errlogPrintf ("devIcv150: init_ao_record: %s: invalid card number %d\n",
			  pao->name, pvmeio->card);
	    pao->dpvt = (void *)1;
	    return ERROR;
	}

	if (icv150[pvmeio->card] == 0)
	{
	    errlogPrintf ("devIcv150: init_ao_record: %s: invalid card number %d\n",
			  pao->name, pvmeio->card);
	    pao->dpvt = (void *)1;
	    return ERROR;
	}

	/*
	 * check signal number
	 */
	if (pvmeio->signal > lastChan[pvmeio->card])
	{
	    errlogPrintf ("devIcv150: init_ao_record: %s: invalid signal number %d\n",
			  pao->name, pvmeio->signal);
	    pao->dpvt = (void *)1;
	    return ERROR;
	}
	
	icv150Addr = icv150[pvmeio->card];
    
	/* 
	 * read gain code
	 */
    
	/* stop scanning */
	pReg = (unsigned short *)(icv150Addr + CS_STOP);
	*pReg = 0;
	taskDelay(1);

	/* read gain */
	pReg = (unsigned short *)(icv150Addr + RD_GAIN + 2 * pvmeio->signal);
	gain = *pReg & 0x0f;
	pao->rval = gain;

	/* restart scanning mode */
	if (autoScan[pvmeio->card])
	    pReg = (unsigned short *)(icv150Addr + CS_START);
	else
	    pReg = (unsigned short *)(icv150Addr + CS_EXT);
	*pReg = 0;

	if (devIcv150Verbose)
	    printf ("\ndevIcv150: init_ao_record: %s: card %d signal %d gain=%d\n", 
		    pao->name, pvmeio->card, pvmeio->signal, pao->rval);

	return OK;
    
    default:
	
	errlogPrintf ("devIcv150: init_ao_record: %s: illegal OUT field\n",
		      pao->name);
	pao->dpvt = (void *)1;
	return ERROR;
    }
}



/*
 * Write signal gain code to on board RAM
 */

static long
write_ao (
	aoRecord *pao
	)
{
    struct vmeio *pvmeio;
    short gain;
    unsigned short *pReg;
    char *icv150Addr;

    if (pao->dpvt)
	return ERROR;
	
    pvmeio = (struct vmeio *)&(pao->out.value);
    icv150Addr = icv150[pvmeio->card];
    
    /* stop scanning */
    pReg = (unsigned short *)(icv150Addr + CS_STOP);
    *pReg = 0;
    taskDelay(1);
    
    /* write gain code in RAM */
    gain = pao->val;
    gain = gain & 0x0f;
    pReg = (unsigned short *)(icv150Addr + 2 * pvmeio->signal);
    *pReg = gain;
    
    /* restart scanning mode */
    if (autoScan[pvmeio->card])
	pReg = (unsigned short *)(icv150Addr + CS_START);
    else
	pReg = (unsigned short *)(icv150Addr + CS_EXT);
    *pReg = 0;
    
    if (devIcv150Verbose == 2)
	printf ("devIcv150: write_ao: card %d signal %d gain=%d\r\n",
		pvmeio->card, pvmeio->signal, gain);

    return 0;
}



/*
 * Create the dset for devAoIcv150
 */

struct {
    long number;
    DEVSUPFUN report;
    DEVSUPFUN init;
    DEVSUPFUN init_record;
    DEVSUPFUN get_ioint_info;
    DEVSUPFUN write_ao;
    DEVSUPFUN special_linconv;
} devAoIcv150 = {
    6, 
    NULL, 
    NULL, 
    init_ao_record, 
    NULL,
    write_ao, 
    NULL
};
epicsExportAddress(dset, devAoIcv150);



/*__________________________________________________________________
 *
 *	AI Device Support 
 */

/*
 * Generate device report
 */

static long
report (
	int interest
	)
{
    int card;

    for (card = 0; card < MAX_ICV150_CARDS; card++) 
    {
	if (icv150[card])
	{
	    printf ("Report ICV150 card %d:\n", card);
	    printf ("- VME address = 0x%x\n", (int)icv150[card]);
	   
	    switch (masks[card]) {
	    case 0x0fff:
		printf ("- 12 bit ADC\n");
		break;
	    case 0x3fff:
		printf ("- 14 bit ADC\n");
		break;
	    case 0xffff:
		printf ("- 16 bit ADC\n");
	    }
	   
	    if (autoScan[card])
		printf ("- automatic scanning\n");
	    else
	    {
		printf ("- scanning on external trigger\n");
		printf ("- number of trigs = %d\n", trigCnt[card]);
	    }
	    printf ("- number of scanned signals = %d\n", lastChan[card] + 1);
	}
    }
     
    return OK;
}



/*
 *  Interrupt service routine for trigger acquisition mode
 */

static void
icv150_int_service (
	int card
	)
{
    unsigned short *pReg;
    char *icv150Addr;
    int itReg;
    
    /* release interrupt */
    icv150Addr = icv150[card];
    pReg = (unsigned short *)(icv150Addr + CS_RIT);
    itReg = *pReg;
    
    /* post database event */
    if (events[card] > 0)
	post_event (events[card]);

    trigCnt[card]++;
    return;
}



/*
 * Initialize device processing
 */

static long
init (
	int after
	)
{
    short dum;
    char *icv150Addr;
    int card;
    unsigned short *pReg;
    unsigned int itVector = IT_VECTOR;
    unsigned short itReg, itLvl;

    /*
     * process init only once before
     */
    if (after)
	return OK;

    /* 
     * convert VME address A24/D16 to CPU local address 
     */
    if (devMapAddr (atVMEA24,
		    0,
		   (size_t) ICV150_BASE,
		    0,
		   (volatile void **)&icv150Addr) != OK)
    {
	errlogPrintf ("devIcv150: init: unable to map ICV150 base address\n");
	return ERROR;
    }

    /* 
     * test for ICV150 boards present in the VME crate
     */
    for (card = 0; card < MAX_ICV150_CARDS; card++)
    {
	if (devReadProbe (sizeof (short),
			 (volatile const void *)icv150Addr,
			 (void *)&dum) == OK)
	{
	    icv150[card] = icv150Addr;
	    
	    if (devIcv150Verbose)
		printf ("devIcv150: init: card %d present (0x%x)\n",
			card, (int)icv150Addr);

	    /* 
	     * connect service routine to hardware interrupt 
	     */
	    itVector = IT_VECTOR + card;
	    if (devConnectInterruptVME (itVector,
					(void (*)(void *))icv150_int_service,
					(void *)card) != OK)
	    {
		errlogPrintf ("devIcv150: init: card %d ISR install error\n", card);
		return ERROR;
	    }
	    if (devIcv150Verbose)
		printf ("devIcv150: init: card %d ISR install ok, vector=0x%x\n", card, itVector);
	    
	    /* 
	     * enable a bus interrupt level 
	     */
	    if (devEnableInterruptLevelVME (IT_LEVEL) != OK)
	    {
		errlogPrintf ("devIcv150: init: card %d enable interrupt level error\n", card);
		return ERROR;
	    }
	    if (devIcv150Verbose)
		printf ("devIcv150: init: card %d enable interrupt level ok\n", card);
	    
	    /*
	     * enable board interrupt 
	     */
	    pReg = (unsigned short *)(icv150Addr + CS_WIT);
	    itLvl = IT_LEVEL;
	    itLvl = (~itLvl << 1) & 0x00FE;
	    itReg = (itVector << 8) | itLvl | IT_ENABLE;
	    *pReg = itReg;
	    if (devIcv150Verbose)
		printf ("devIcv150: init: card %d it=0x%4x\n",
			card, itReg);
	}

	/* 
	 * next card
	 */
	icv150Addr += ICV150_SIZE;
    }
    
    return OK;
}



/*
 * Initialize AI record
 */

static long
init_ai_record (
	aiRecord *pai
	)
{
    struct vmeio *pvmeio;

    pai->dpvt = (void *)0;
    
    switch (pai->inp.type)
    {
    case (VME_IO):
	
	pvmeio = (struct vmeio *)&(pai->inp.value);
	
	/*
	 * check card number
	 */
	if (pvmeio->card >= MAX_ICV150_CARDS) 
	{
	    errlogPrintf ("devIcv150: init_ai_record: %s: invalid card number %d\n",
			  pai->name, pvmeio->card);
	    pai->dpvt = (void *)1;
	    return ERROR;
	}

	if (icv150[pvmeio->card] == 0)
	{
	    errlogPrintf ("devIcv150: init_ai_record: %s: invalid card number %d\n",
			  pai->name, pvmeio->card);
	    pai->dpvt = (void *)1;
	    return ERROR;
	}

	/*
	 * check signal number
	 */
	if (pvmeio->signal > lastChan[pvmeio->card])
	{
	    errlogPrintf ("devIcv150: init_ai_record: %s: invalid signal number %d\n",
			  pai->name, pvmeio->signal);
	    pai->dpvt = (void *)1;
	    return ERROR;
	}

	/* 
	 * set linear conversion slope 
	 */
	pai->eslo = (pai->eguf - pai->egul) / masks[pvmeio->card];

	if (devIcv150Verbose)
	    printf ("\ndevIcv150: init_ai_record: %s: card %d signal %d mask=0x%4x eslo=%f\n",
		    pai->name, pvmeio->card, pvmeio->signal, masks[pvmeio->card], pai->eslo);

	return OK;

    default:
	  
	errlogPrintf ("devIcv150: init_ai_record: %s: illegal INP field\n",
		      pai->name);
	pai->dpvt = (void *)1;
	return ERROR;
    }
}



/*
 *  Read signal value
 */

static long
read_ai (
	aiRecord *pai
	)
{
    unsigned short *pReg;
    struct vmeio *pvmeio;

    if (pai->dpvt)
	return ERROR;
	
    pvmeio = (struct vmeio *)&(pai->inp.value);

    pReg = (unsigned short *)icv150[pvmeio->card] + pvmeio->signal;
    pai->rval = *pReg & masks[pvmeio->card];

    if (devIcv150Verbose == 2)
	printf ("devIcv150: read_ai: %s: mask=0x%4x rval=%d\r\n",
		pai->name, masks[pvmeio->card], pai->rval);

    return 0;
}



/*
 * Set linear conversion slope
 */

static long
special_linconv (
	aiRecord *pai,
	int after
	)
{
    struct vmeio *pvmeio;

    if (!after)
	return 0;
    
    pvmeio = (struct vmeio *)&(pai->inp.value);
    pai->eslo = (pai->eguf - pai->egul) / masks[pvmeio->card];
    return 0;
}



/*
 * Create the dset for devAiIcv150
 */
struct {
    long number;
    DEVSUPFUN report;
    DEVSUPFUN init;
    DEVSUPFUN init_record;
    DEVSUPFUN get_ioint_info;
    DEVSUPFUN read_ai;
    DEVSUPFUN special_linconv;
} devAiIcv150 = {
    6, 
    report, 
    init, 
    init_ai_record, 
    NULL, 
    read_ai, 
    special_linconv
};
epicsExportAddress(dset, devAiIcv150);



/*__________________________________________________________________
 *
 *	WAVEFORM Device Support 
 */

/*
 * Initialize WAVEFORM record
 */

static long
init_wf_record (
	waveformRecord *pwf
	)
{
    struct vmeio *pvmeio;

    pwf->dpvt = (void *)0;
    
    switch (pwf->inp.type)
    {
    case (VME_IO):
	
	pvmeio = (struct vmeio *)&(pwf->inp.value);
	
	/*
	 * check card number
	 */
	if (pvmeio->card >= MAX_ICV150_CARDS) 
	{
	    errlogPrintf ("devIcv150: init_wf_record: %s: invalid card number %d\n",
			  pwf->name, pvmeio->card);
	    pwf->dpvt = (void *)1;
	    return ERROR;
	}

	if (icv150[pvmeio->card] == 0)
	{
	    errlogPrintf ("devIcv150: init_wf_record: %s: invalid card number %d\n",
			  pwf->name, pvmeio->card);
	    pwf->dpvt = (void *)1;
	    return ERROR;
	}

	/*
	 * check signal number
	 */
	if (pvmeio->signal > lastChan[pvmeio->card])
	{
	    errlogPrintf ("devIcv150: init_wf_record: %s: invalid signal number %d\n",
			  pwf->name, pvmeio->signal);
	    pwf->dpvt = (void *)1;
	    return ERROR;
	}
	
	if ((pvmeio->signal + pwf->nelm) > (lastChan[pvmeio->card] + 1))
	{
	    errlogPrintf ("devIcv150: init_wf_record: %s: invalid signals count %d\n",
			  pwf->name, pwf->nelm);
	    pwf->dpvt = (void *)1;
	    return ERROR;
	}

	/*
	 * check waveform data type
	 */
	switch (pwf->ftvl)
	{
	case (DBF_USHORT):
	case (DBF_LONG):
	case (DBF_ULONG):
	case (DBF_FLOAT):
	case (DBF_DOUBLE):
	    return OK;
	default:
	    errlogPrintf ("devIcv150: init_wf_record: %s: invalid data type\n",
			  pwf->name);
	    pwf->dpvt = (void *)1;
	    return ERROR;
	}

    default:
	  
	errlogPrintf ("devIcv150: init_wf_record: %s: illegal INP field\n",
		      pwf->name);
	pwf->dpvt = (void *)1;
	return ERROR;
    }
}



/*
 *  Read WAVEFORM
 */

static long
read_wf (
	waveformRecord *pwf
	)
{
    unsigned short *pReg;
    struct vmeio *pvmeio;
    int i;

    if (pwf->dpvt)
	return ERROR;
	
    pvmeio = (struct vmeio *)&(pwf->inp.value);

    pReg = (unsigned short *)icv150[pvmeio->card] + pvmeio->signal;
    
    switch (pwf->ftvl)
    {
	case (DBF_USHORT):
	{
	    unsigned short *pBuf = (unsigned short *)pwf->bptr;
	    
	    for (i = 1; i <= pwf->nelm; i++)
		*pBuf++ = *pReg++ & masks[pvmeio->card];
	}
	break;
	case (DBF_LONG):
	{
	    long *pBuf = (long *)pwf->bptr;
	    
	    for (i = 1; i <= pwf->nelm; i++)
		*pBuf++ = *pReg++ & masks[pvmeio->card];
	}
	break;
	case (DBF_ULONG):
	{
	    unsigned long *pBuf = (unsigned long *)pwf->bptr;
	    
	    for (i = 1; i <= pwf->nelm; i++)
		*pBuf++ = *pReg++ & masks[pvmeio->card];
	}
	break;
	case (DBF_FLOAT):
	{
	    float *pBuf = (float *)pwf->bptr;
	    int conv = pwf->lopr != pwf->hopr;
	    double slope = (pwf->hopr - pwf->lopr) / masks[pvmeio->card];
	    
	    for (i = 1; i <= pwf->nelm; i++)
		if (conv)
		    *pBuf++ = (*pReg++ & masks[pvmeio->card]) * slope + pwf->lopr;
		else
		    *pBuf++ = *pReg++ & masks[pvmeio->card];
	}
	break;
	case (DBF_DOUBLE):
	{
	    double *pBuf = (double *)pwf->bptr;
	    int conv = pwf->lopr != pwf->hopr;
	    double slope = (pwf->hopr - pwf->lopr) / masks[pvmeio->card];
	    
	    for (i = 1; i <= pwf->nelm; i++)
		if (conv)
		    *pBuf++ = (*pReg++ & masks[pvmeio->card]) * slope + pwf->lopr;
		else
		    *pBuf++ = *pReg++ & masks[pvmeio->card];
	}
    }
    
    pwf->nord = pwf->nelm;
    
    if (devIcv150Verbose == 2)
	printf ("devIcv150: read_wf: %s\n", pwf->name);
    
    return 0;
}



struct {
    long		number;
    DEVSUPFUN report;
    DEVSUPFUN init;
    DEVSUPFUN init_record;
    DEVSUPFUN get_ioint_info;
    DEVSUPFUN read_wf;
} devWfIcv150 = {
    5,
    NULL,
    NULL,
    init_wf_record,
    NULL,
    read_wf
};
epicsExportAddress(dset, devWfIcv150);


