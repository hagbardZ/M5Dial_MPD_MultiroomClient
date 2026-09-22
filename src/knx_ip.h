#pragma once

#include <Arduino.h>

// Minimal KNXnet/IP tunneling client (raw UDP, no external library).
//
// The client keeps a KNX IP tunneling connection to the interface defined
// in config.h (KNX_HOST / KNX_PORT), triggers group telegrams for the
// Amplifier1 toggle (group address write) and tracks the state reported
// on the status group address.
//
// The KNX runner lives in the MPD task: call knxSetup() once at boot and
// knxLoop() from the task loop while Wi-Fi is up.  knxReset() drops the
// tunnel (e.g. Wi-Fi loss / standby).

void knxSetup();                 // once at boot (before the MPD task starts)
void knxLoop();                  // non-blocking periodic driver
void knxReset();                 // tear the tunnel down and forget the state

// Number of KNX devices configured (from config.h).
int knxDeviceCount();

// Request an On/Off toggle of device `dev` (0-based index).
void knxToggle(int dev);

// Current device state as seen on its status group (UI may read this from
// any task; internally mutex-guarded).  knxAmpIsValid() is false until the
// status group reports once (or the device has been toggled locally).
bool knxAmpIsOn(int dev);
bool knxAmpIsValid(int dev);