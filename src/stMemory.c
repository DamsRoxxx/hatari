/*
  Hatari - stMemory.c

  This file is distributed under the GNU Public License, version 2 or at
  your option any later version. Read the file gpl.txt for details.

  ST Memory access functions.
*/
const char STMemory_fileid[] = "Hatari stMemory.c : " __DATE__ " " __TIME__;

#include "stMemory.h"
#include "configuration.h"
#include "floppy.h"
#include "gemdos.h"
#include "ioMem.h"
#include "log.h"
#include "memory.h"
#include "memorySnapShot.h"
#include "tos.h"
#include "vdi.h"


/* STRam points to our ST Ram. Unless the user enabled SMALL_MEM where we have
 * to save memory, this includes all TOS ROM and IO hardware areas for ease
 * and emulation speed - so we create a 16 MiB array directly here.
 * But when the user turned on ENABLE_SMALL_MEM, this only points to a malloc'ed
 * buffer with the ST RAM; the ROM and IO memory will be handled separately. */
#if ENABLE_SMALL_MEM
Uint8 *STRam;
#else
Uint8 STRam[16*1024*1024];
#endif

Uint32 STRamEnd;            /* End of ST Ram, above this address is no-mans-land and ROM/IO memory */


/**
 * Clear section of ST's memory space.
 */
void STMemory_Clear(Uint32 StartAddress, Uint32 EndAddress)
{
	memset(&STRam[StartAddress], 0, EndAddress-StartAddress);
}


/**
 * Save/Restore snapshot of RAM / ROM variables
 * ('MemorySnapShot_Store' handles type)
 */
void STMemory_MemorySnapShot_Capture(bool bSave)
{
	MemorySnapShot_Store(&STRamEnd, sizeof(STRamEnd));

	/* Only save/restore area of memory machine is set to, eg 1Mb */
	MemorySnapShot_Store(STRam, STRamEnd);

	/* And Cart/TOS/Hardware area */
	MemorySnapShot_Store(&RomMem[0xE00000], 0x200000);
}


/**
 * Set default memory configuration, connected floppies, memory size and
 * clear the ST-RAM area.
 * As TOS checks hardware for memory size + connected devices on boot-up
 * we set these values ourselves and fill in the magic numbers so TOS
 * skips these tests.
 */
void STMemory_SetDefaultConfig(void)
{
	int i;
	int screensize;
	int memtop;
	Uint8 nMemControllerByte;
	Uint8 nFalcSysCntrl, nFalcSysCntrl2 = 0;

	static const int MemControllerTable[] =
	{
		0x01,   /* 512 KiB */
		0x05,   /* 1 MiB */
		0x02,   /* 2 MiB */
		0x06,   /* 2.5 MiB */
		0x0A    /* 4 MiB */
	};

	if (bRamTosImage)
	{
		/* Clear ST-RAM, excluding the RAM TOS image */
		STMemory_Clear(0x00000000, TosAddress);
		STMemory_Clear(TosAddress+TosSize, STRamEnd);
	}
	else
	{
		/* Clear whole ST-RAM */
		STMemory_Clear(0x00000000, STRamEnd);
	}

	/* Mirror ROM boot vectors */
	STMemory_WriteLong(0x00, STMemory_ReadLong(TosAddress));
	STMemory_WriteLong(0x04, STMemory_ReadLong(TosAddress+4));

	/* Fill in magic numbers, so TOS does not try to reference MMU */
	STMemory_WriteLong(0x420, 0x752019f3);            /* memvalid - configuration is valid */
	STMemory_WriteLong(0x43a, 0x237698aa);            /* another magic # */
	STMemory_WriteLong(0x51a, 0x5555aaaa);            /* and another */

	/* Set memory size, adjust for extra VDI screens if needed.
	 * Note: TOS seems to set phys_top-0x8000 as the screen base
	 * address - so we have to move phys_top down in VDI resolution
	 * mode, although there is more "physical" ST RAM available. */
	screensize = VDIWidth * VDIHeight / 8 * VDIPlanes;
        /* Use 32 kiB in normal screen mode or when the screen size is smaller than 32 kiB */
	if (!bUseVDIRes || screensize < 0x8000)
		screensize = 0x8000;
	/* mem top - upper end of user memory (right before the screen memory).
	 * Note: memtop / phystop must be dividable by 512, or TOS crashes */
	memtop = (STRamEnd - screensize) & 0xfffffe00;
	STMemory_WriteLong(0x436, memtop);
	/* phys top - This must be memtop + 0x8000 to make TOS happy */
	STMemory_WriteLong(0x42e, memtop+0x8000);

	/* Set memory controller byte according to different memory sizes */
	/* Setting per bank: %00=128k %01=512k %10=2Mb %11=reserved. - e.g. %1010 means 4Mb */
	if (ConfigureParams.Memory.nMemorySize <= 4)
		nMemControllerByte = MemControllerTable[ConfigureParams.Memory.nMemorySize];
	else
		nMemControllerByte = 0x0f;
	STMemory_WriteByte(0x424, nMemControllerByte);
	IoMem_WriteByte(0xff8001, nMemControllerByte);

	if (ConfigureParams.System.nMachineType == MACHINE_FALCON)
	{
		/* Set the Falcon memory and monitor configuration register:
			$FFFF8006: Monitor Type BIT 7 6  (These 2 bits are duplicated in $FFFF82C0 (BIT 1 0))
				00 - Monochrome (SM124)
				01 - Color (SC1224)
				10 - VGA Color
				11 - Television
			
			$FFFF8006: Memory configuration BIT 5 4
				00 -  1Mb
				01 -  4Mb
				10 - 14Mb
				11 - no boot !
		*/
		if (ConfigureParams.Memory.nMemorySize == 14)
			nFalcSysCntrl = 0x20;
		else if (ConfigureParams.Memory.nMemorySize >= 4)
			nFalcSysCntrl = 0x10;
		else
			nFalcSysCntrl = 0x00;

		switch(ConfigureParams.Screen.nMonitorType) {
			case MONITOR_TYPE_TV:
				nFalcSysCntrl |= FALCON_MONITOR_TV;
				nFalcSysCntrl2 = 0x3;
				break;
			case MONITOR_TYPE_VGA:
				nFalcSysCntrl |= FALCON_MONITOR_VGA;
				nFalcSysCntrl2 = 0x2;
				break;
			case MONITOR_TYPE_RGB:
				nFalcSysCntrl |= FALCON_MONITOR_RGB;
				nFalcSysCntrl2 = 0x1;
				break;
			case MONITOR_TYPE_MONO:
				nFalcSysCntrl |= FALCON_MONITOR_MONO;
				nFalcSysCntrl2 = 0x0;
				break;
		}
		STMemory_WriteByte(0xff8006, nFalcSysCntrl);
		nFalcSysCntrl = STMemory_ReadByte(0xff82c0) & 0xfc;
		nFalcSysCntrl |= nFalcSysCntrl2;
		STMemory_WriteByte(0xff82c0, nFalcSysCntrl);

		/* Set the Falcon Bus Control:
			$FFFF8007 Falcon Bus Control
				BIT 6 : F30 Start (0=Cold, 1=Warm) 
				BIT 5 : STe Bus Emulation (0=on)
				BIT 3 : Blitter Flag (0=on, 1=off)
				BIT 2 : Blitter (0=8mhz, 1=16mhz)
				BIT 0 : 68030 (0=8mhz, 1=16mhz)
		
			Todo: these values should be verified (docs are too poor on this register)
			Todo: set $FFFF8007 with correct values
		*/
	}

	/* Set TOS floppies */
	STMemory_WriteWord(0x446, nBootDrive);          /* Boot up on A(0) or C(2) */

	/* Create connected drives mask: */
	ConnectedDriveMask = STMemory_ReadLong(0x4c2);  // Get initial drive mask (see what TOS thinks)
	ConnectedDriveMask |= 0x03;                     // Always use A: and B:
	if (GEMDOS_EMU_ON)
	{
		for (i = 0; i < MAX_HARDDRIVES; i++)
		{
			if (emudrives[i] != NULL)     // Is this GEMDOS drive enabled?
				ConnectedDriveMask |= (1 << emudrives[i]->drive_number);
		}
	}
	/* Set connected drives system variable.
	 * NOTE: some TOS images overwrite this value, see 'OpCode_SysInit', too */
	STMemory_WriteLong(0x4c2, ConnectedDriveMask);
}
