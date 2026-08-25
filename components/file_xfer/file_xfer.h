#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Serial file transfer service (test utility).
 *
 * Listens on UART0 (the CH343 USB-serial bridge) for simple ASCII commands
 * and streams binary payloads onto the mounted SD card:
 *
 *   ARPFILE put <name> <size> <crc32hex>\n   -> RDY\n, <size> bytes in
 *      16 KB blocks each ACKed with 'K', then "OK <crc>\n" or "BAD\n".
 *      The card file is written as <name>.part and renamed on success.
 *   ARPFILE ls\n                             -> "<size> <name>" lines + END\n
 *   ARPFILE del <name>\n                     -> OK\n or ERR\n
 *
 * Baud is 115200 for the command line; the sender may request a faster
 * payload baud with "ARPFILE baud <rate>\n" right after RDY.
 */

/** Spawns the listener task. Safe to call once after boot; the task itself
 *  waits until the SD card is mounted before accepting transfers. */
void arpile_file_xfer_start(void);

#ifdef __cplusplus
}
#endif
