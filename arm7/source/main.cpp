// SPDX-License-Identifier: CC0-1.0
// ARM7 firmware for RSVPReaderDS.
// Handles touch, keypad, power management and RTC via calico.
// No audio is needed — identical structure to ChordSynthDS ARM7 minus sound.

#include <calico.h>
#include <nds.h>

int main(void) {
    envReadNvramSettings();
    keypadStartExtServer();
    lcdSetIrqMask(DISPSTAT_IE_ALL, DISPSTAT_IE_VBLANK);
    irqEnable(IRQ_VBLANK);
    rtcInit();
    rtcSyncTime();
    pmInit();
    blkInit();
    touchInit();
    touchStartServer(80, MAIN_THREAD_PRIO);

    while (pmMainLoop()) {
        threadWaitForVBlank();
    }
    return 0;
}
