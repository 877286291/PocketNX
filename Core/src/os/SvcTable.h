#pragma once

#include "os/OS.h"

namespace pnx {

// Register the raw kernel SVC handlers a minimal CPU homebrew needs. This is the
// M1 subset of the Switch SVC table (numbers from switchbrew.org/wiki/SVC). Each
// handler reads its arguments from the guest registers (x0..x7) and writes the
// guest-visible Result to x0, matching the real kernel ABI.
//
// Implemented (M1 core set):
//   0x07 ExitProcess            0x10 GetCurrentProcessorNumber
//   0x0B SleepThread            0x11 SignalEvent (lenient)
//   0x12 ClearEvent (lenient)   0x16 CloseHandle
//   0x17 ResetSignal (lenient)  0x18 WaitSynchronization (immediate)
//   0x1E GetSystemTick          0x1F ConnectToNamedPort
//   0x20 SendSyncRequestLight   0x21 SendSyncRequest
//   0x22 SendSyncRequestWithUserBuffer
//   0x25 GetThreadId            0x26 Break
//   0x27 OutputDebugString      0x29 GetInfo
//   0x2A FlushEntireDataCache   0x45 CreateEvent
//
// Everything else returns `Unimplemented` (logged) so bring-up surfaces exactly
// which SVC a homebrew still needs.
void RegisterDefaultSVCs(OS& os);

// Convenience: register the default IPC services AND the raw SVC handlers.
void RegisterDefaults(OS& os);

} // namespace pnx
