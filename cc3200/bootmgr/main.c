/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Daniel Campora
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdint.h>
#include <stdbool.h>

#include <std.h>
#include "hw_ints.h"
#include "hw_types.h"
#include "hw_gpio.h"
#include "hw_memmap.h"
#include "hw_gprcm.h"
#include "hw_common_reg.h"
#include "pin.h"
#include "gpio.h"
#include "rom.h"
#include "rom_map.h"
#include "prcm.h"
#include "simplelink.h"
#include "interrupt.h"
#include "gpio.h"
#include "flc.h"
#include "bootmgr.h"
#include "shamd5.h"
#include "hash.h"
#include "utils.h"
#include "cc3200_hal.h"


//*****************************************************************************
// Local Constants
//*****************************************************************************
#define SL_STOP_TIMEOUT                     250
#define BOOTMGR_HASH_ALGO                   SHAMD5_ALGO_MD5
#define BOOTMGR_HASH_SIZE                   32
#define BOOTMGR_BUFF_SIZE                   512

#define BOOTMGR_WAIT_SAFE_MODE_MS           2000
#define BOOTMGR_WAIT_SAFE_MODE_TOOGLE_MS    250

#define BOOTMGR_SAFE_MODE_ENTER_MS          1000
#define BOOTMGR_SAFE_MODE_ENTER_TOOGLE_MS   100

#define BOOTMGR_PINS_PRCM                   PRCM_GPIOA3
#define BOOTMGR_PINS_PORT                   GPIOA3_BASE
#define BOOTMGR_LED_PIN_NUM                 PIN_21
#define BOOTMGR_SFE_PIN_NUM                 PIN_18
#define BOOTMGR_LED_PORT_PIN                GPIO_PIN_1      // GPIO25
#define BOOTMGR_SFE_PORT_PIN                GPIO_PIN_4      // GPIO28


//*****************************************************************************
// Exported functions declarations
//*****************************************************************************
extern void bootmgr_run_app (_u32 base);

//*****************************************************************************
// Local functions declarations
//*****************************************************************************
static void bootmgr_board_init (void);
static bool bootmgr_verify (void);
static void bootmgr_load_and_execute (_u8 *image);
static bool safe_mode_boot (void);
static void bootmgr_image_loader (sBootInfo_t *psBootInfo);

//*****************************************************************************
// Private data
//*****************************************************************************
static _u8 bootmgr_file_buf[BOOTMGR_BUFF_SIZE];
static _u8 bootmgr_hash_buf[BOOTMGR_HASH_SIZE + 1];

//*****************************************************************************
// Vector Table
//*****************************************************************************
extern void (* const g_pfnVectors[])(void);

//*****************************************************************************
// WLAN Event handler callback hookup function
//*****************************************************************************
void SimpleLinkWlanEventHandler(SlWlanEvent_t *pWlanEvent)
{

}

//*****************************************************************************
// HTTP Server callback hookup function
//*****************************************************************************
void SimpleLinkHttpServerCallback(SlHttpServerEvent_t *pHttpEvent,
                                  SlHttpServerResponse_t *pHttpResponse)
{

}

//*****************************************************************************
// Net APP Event callback hookup function
//*****************************************************************************
void SimpleLinkNetAppEventHandler(SlNetAppEvent_t *pNetAppEvent)
{

}

//*****************************************************************************
// General Event callback hookup function
//*****************************************************************************
void SimpleLinkGeneralEventHandler(SlDeviceEvent_t *pDevEvent)
{

}

//*****************************************************************************
// Socket Event callback hookup function
//*****************************************************************************
void SimpleLinkSockEventHandler(SlSockEvent_t *pSock)
{

}

//*****************************************************************************
//! Board Initialization & Configuration
//*****************************************************************************
static void bootmgr_board_init(void) {
    // Set vector table base
    MAP_IntVTableBaseSet((unsigned long)&g_pfnVectors[0]);

    // Enable Processor Interrupts
    MAP_IntMasterEnable();
    MAP_IntEnable(FAULT_SYSTICK);

    // Mandatory MCU Initialization
    PRCMCC3200MCUInit();

    // Enable the Data Hashing Engine
    HASH_Init();

    // Enable GPIOA3 Peripheral Clock
    MAP_PRCMPeripheralClkEnable(BOOTMGR_PINS_PRCM, PRCM_RUN_MODE_CLK);

    // Configure the bld
    MAP_PinTypeGPIO(BOOTMGR_LED_PIN_NUM, PIN_MODE_0, false);
    MAP_PinConfigSet(BOOTMGR_LED_PIN_NUM, PIN_STRENGTH_6MA, PIN_TYPE_STD);
    MAP_GPIODirModeSet(BOOTMGR_PINS_PORT, BOOTMGR_LED_PORT_PIN, GPIO_DIR_MODE_OUT);

    // Configure the safe mode pin
    MAP_PinTypeGPIO(BOOTMGR_SFE_PIN_NUM, PIN_MODE_0, false);
    MAP_PinConfigSet(BOOTMGR_SFE_PIN_NUM, PIN_STRENGTH_4MA, PIN_TYPE_STD_PU);
    MAP_GPIODirModeSet(BOOTMGR_PINS_PORT, BOOTMGR_SFE_PORT_PIN, GPIO_DIR_MODE_IN);
}

//*****************************************************************************
//! Verifies the integrity of the new application binary
//*****************************************************************************
static bool bootmgr_verify (void) {
    SlFsFileInfo_t FsFileInfo;
    _u32 reqlen, offset = 0;
    _i32 fHandle;

    // open the file for reading
    if (0 == sl_FsOpen((_u8 *)IMG_UPDATE, FS_MODE_OPEN_READ, NULL, &fHandle)) {
        // get the file size
        sl_FsGetInfo((_u8 *)IMG_UPDATE, 0, &FsFileInfo);

        if (FsFileInfo.FileLen > BOOTMGR_HASH_SIZE) {
            FsFileInfo.FileLen -= BOOTMGR_HASH_SIZE;
            HASH_SHAMD5Start(BOOTMGR_HASH_ALGO, FsFileInfo.FileLen);
            do {
                if ((FsFileInfo.FileLen - offset) > BOOTMGR_BUFF_SIZE) {
                    reqlen = BOOTMGR_BUFF_SIZE;
                }
                else {
                    reqlen = FsFileInfo.FileLen - offset;
                }

                offset += sl_FsRead(fHandle, offset, bootmgr_file_buf, reqlen);
                HASH_SHAMD5Update(bootmgr_file_buf, reqlen);
            } while (offset < FsFileInfo.FileLen);

            HASH_SHAMD5Read (bootmgr_file_buf);

            // convert the resulting hash to hex
            for (_u32 i = 0; i < (BOOTMGR_HASH_SIZE / 2); i++) {
                snprintf ((char *)&bootmgr_hash_buf[(i * 2)], 3, "%02x", bootmgr_file_buf[i]);
            }

            // read the hash from the file and close it
            ASSERT (BOOTMGR_HASH_SIZE == sl_FsRead(fHandle, offset, bootmgr_file_buf, BOOTMGR_HASH_SIZE));
            sl_FsClose (fHandle, NULL, NULL, 0);
            bootmgr_file_buf[BOOTMGR_HASH_SIZE] = '\0';
            // compare both hashes
            if (!strcmp((const char *)bootmgr_hash_buf, (const char *)bootmgr_file_buf)) {
                // it's a match
                return true;
            }
        }
        // close the file
        sl_FsClose(fHandle, NULL, NULL, 0);
    }
    return false;
}

//*****************************************************************************
//! Loads the application from sFlash and executes
//*****************************************************************************
static void bootmgr_load_and_execute (_u8 *image) {
    SlFsFileInfo_t pFsFileInfo;
    _i32 fhandle;
    // open the application binary
    if (!sl_FsOpen(image, FS_MODE_OPEN_READ, NULL, &fhandle)) {
        // get the file size
        if (!sl_FsGetInfo(image, 0, &pFsFileInfo)) {
            // read the application into SRAM
            if (pFsFileInfo.FileLen == sl_FsRead(fhandle, 0, (unsigned char *)APP_IMG_SRAM_OFFSET, pFsFileInfo.FileLen)) {
                // close the file
                sl_FsClose(fhandle, 0, 0, 0);
                // stop the network services
                sl_Stop(SL_STOP_TIMEOUT);
                // execute the application
                bootmgr_run_app(APP_IMG_SRAM_OFFSET);
            }
        }
    }
}

//*****************************************************************************
//! Check for the safe mode pin
//*****************************************************************************
static bool safe_mode_boot (void) {
    _u32 count = 0;
    while (!MAP_GPIOPinRead(BOOTMGR_PINS_PORT, BOOTMGR_SFE_PORT_PIN) &&
            ((BOOTMGR_WAIT_SAFE_MODE_TOOGLE_MS * count++) < BOOTMGR_WAIT_SAFE_MODE_MS)) {
        // toogle the led
        MAP_GPIOPinWrite(BOOTMGR_PINS_PORT, BOOTMGR_LED_PORT_PIN, ~MAP_GPIOPinRead(GPIOA3_BASE, BOOTMGR_LED_PORT_PIN));
        UtilsDelay(UTILS_DELAY_US_TO_COUNT(BOOTMGR_WAIT_SAFE_MODE_TOOGLE_MS * 1000));
    }
    return MAP_GPIOPinRead(BOOTMGR_PINS_PORT, BOOTMGR_SFE_PORT_PIN) ? false : true;
}

//*****************************************************************************
//! Load the proper image based on information from boot info and executes it.
//*****************************************************************************
static void bootmgr_image_loader(sBootInfo_t *psBootInfo) {
    _i32 fhandle;
    if (safe_mode_boot()) {
         _u32 count = 0;
         while ((BOOTMGR_SAFE_MODE_ENTER_TOOGLE_MS * count++) > BOOTMGR_SAFE_MODE_ENTER_MS) {
             // toogle the led
             MAP_GPIOPinWrite(BOOTMGR_PINS_PORT, BOOTMGR_LED_PORT_PIN, ~MAP_GPIOPinRead(GPIOA3_BASE, BOOTMGR_LED_PORT_PIN));
             UtilsDelay(UTILS_DELAY_US_TO_COUNT(BOOTMGR_SAFE_MODE_ENTER_TOOGLE_MS * 1000));
         }
         psBootInfo->ActiveImg = IMG_ACT_FACTORY;
         // turn the led off
         MAP_GPIOPinWrite(BOOTMGR_PINS_PORT, BOOTMGR_LED_PORT_PIN, 0);
    }
    // do we have a new update image that needs to be verified?
    else if ((psBootInfo->ActiveImg == IMG_ACT_UPDATE) && (psBootInfo->Status == IMG_STATUS_CHECK)) {
        if (!bootmgr_verify()) {
            // delete the corrupted file
            sl_FsDel((_u8 *)IMG_UPDATE, 0);
            // switch to the factory image
            psBootInfo->ActiveImg = IMG_ACT_FACTORY;
        }
        // in any case, set the status as "READY"
        psBootInfo->Status = IMG_STATUS_READY;
        // write the new boot info
        if (!sl_FsOpen((unsigned char *)IMG_BOOT_INFO, FS_MODE_OPEN_WRITE, NULL, &fhandle)) {
            sl_FsWrite(fhandle, 0, (unsigned char *)psBootInfo, sizeof(sBootInfo_t));
            // close the file
            sl_FsClose(fhandle, 0, 0, 0);
        }
    }

    // now boot the active image
    if (IMG_ACT_UPDATE == psBootInfo->ActiveImg) {
        bootmgr_load_and_execute((unsigned char *)IMG_UPDATE);
    }
    else {
        bootmgr_load_and_execute((unsigned char *)IMG_FACTORY);
    }
}

//*****************************************************************************
//! Main function
//*****************************************************************************
int main (void) {
    sBootInfo_t sBootInfo = { .ActiveImg = IMG_ACT_FACTORY, .Status = IMG_STATUS_READY };
    bool bootapp = false;
    _i32 fhandle;

    // Board Initialization
    bootmgr_board_init();

    // start simplelink since we need it to access the sflash
    sl_Start(NULL, NULL, NULL);

    // if a boot info file is found, load it, else, create a new one with the default boot info
    if (!sl_FsOpen((unsigned char *)IMG_BOOT_INFO, FS_MODE_OPEN_READ, NULL, &fhandle)) {
        if (sizeof(sBootInfo_t) == sl_FsRead(fhandle, 0, (unsigned char *)&sBootInfo, sizeof(sBootInfo_t))) {
            bootapp = true;
        }
        sl_FsClose(fhandle, 0, 0, 0);
    }
    if (!bootapp) {
        // create a new boot info file
        _u32 BootInfoCreateFlag  = _FS_FILE_OPEN_FLAG_COMMIT | _FS_FILE_PUBLIC_WRITE | _FS_FILE_PUBLIC_READ;
        if (!sl_FsOpen ((unsigned char *)IMG_BOOT_INFO, FS_MODE_OPEN_CREATE((2 * sizeof(sBootInfo_t)),
                        BootInfoCreateFlag), NULL, &fhandle)) {
            // Write the default boot info.
            if (sizeof(sBootInfo_t) == sl_FsWrite(fhandle, 0, (unsigned char *)&sBootInfo, sizeof(sBootInfo_t))) {
                bootapp = true;
            }
            sl_FsClose(fhandle, 0, 0, 0);
        }
    }

    if (bootapp) {
        // load and execute the image based on the boot info
        bootmgr_image_loader(&sBootInfo);
    }

    // stop simplelink
    sl_Stop(SL_STOP_TIMEOUT);

    // if we've reached this point, then it means a fatal error occurred and the application
    // could not be loaded, so, loop forever and signal the crash to the user
    while (true) {
        // keep the bld on
        MAP_GPIOPinWrite(BOOTMGR_PINS_PORT, BOOTMGR_LED_PORT_PIN, BOOTMGR_LED_PORT_PIN);
        __asm volatile("    dsb      \n"
                       "    isb      \n"
                       "    wfi      \n");
    }
}

