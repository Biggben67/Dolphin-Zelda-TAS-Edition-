// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/HW/Memmap.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/PowerPC/JitInterface.h"

namespace API::Memory
{

// memchecks

void AddMemcheck(u32 addr);
void RemoveMemcheck(u32 addr);

// memory reading: just directly forward to the MMU

inline u8 Read_U8(const Core::CPUThreadGuard &guard, u32 addr)
{
  return guard.GetSystem().GetMMU().HostRead<u8>(guard, addr);
}

inline u16 Read_U16(const Core::CPUThreadGuard& guard, u32 addr)
{
  return guard.GetSystem().GetMMU().HostRead<u16>(guard, addr);
}

inline u32 Read_U32(const Core::CPUThreadGuard& guard, u32 addr)
{
  return guard.GetSystem().GetMMU().HostRead<u32>(guard, addr);
}

inline u64 Read_U64(const Core::CPUThreadGuard& guard, u32 addr)
{
  return guard.GetSystem().GetMMU().HostRead<u64>(guard, addr);
}

inline s8 Read_S8(const Core::CPUThreadGuard& guard, u32 addr)
{
  return guard.GetSystem().GetMMU().HostRead<s8>(guard, addr);
}

inline s16 Read_S16(const Core::CPUThreadGuard& guard, u32 addr)
{
  return guard.GetSystem().GetMMU().HostRead<s16>(guard, addr);
}

inline s32 Read_S32(const Core::CPUThreadGuard& guard, u32 addr)
{
  return guard.GetSystem().GetMMU().HostRead<s32>(guard, addr);
}

inline s64 Read_S64(const Core::CPUThreadGuard& guard, u32 addr)
{
  return guard.GetSystem().GetMMU().HostRead<s64>(guard, addr);
}

inline float Read_F32(const Core::CPUThreadGuard& guard, u32 addr)
{
  return guard.GetSystem().GetMMU().HostRead<float>(guard, addr);
}

inline double Read_F64(const Core::CPUThreadGuard& guard, u32 addr)
{
  return guard.GetSystem().GetMMU().HostRead<double>(guard, addr);
}

inline char* Read_Bytes(const Core::CPUThreadGuard& guard, u32 addr, u32 size)
{
  char* data = new char[size];
  for (u32 i = 0; i < size; ++i)
    data[i] = static_cast<char>(guard.GetSystem().GetMMU().HostRead<u8>(guard, addr + i));
  return data;
}

// memory writing: arguments of write functions are swapped (address first) to be consistent with other scripting APIs

inline void InvalidateICache(const Core::CPUThreadGuard &guard, u32 addr, u32 size)
{
  (void)guard;
  Core::System::GetInstance().GetJitInterface().InvalidateICache(addr, size, true);
}

inline void Write_U8(const Core::CPUThreadGuard &guard, u32 addr, u8 val)
{
  guard.GetSystem().GetMMU().HostWrite<u8>(guard, val, addr);
}

inline void Write_U16(const Core::CPUThreadGuard& guard, u32 addr, u16 val)
{
  guard.GetSystem().GetMMU().HostWrite<u16>(guard, val, addr);
}

inline void Write_U32(const Core::CPUThreadGuard& guard, u32 addr, u32 val)
{
  guard.GetSystem().GetMMU().HostWrite<u32>(guard, val, addr);
}

inline void Write_U64(const Core::CPUThreadGuard& guard, u32 addr, u64 val)
{
  guard.GetSystem().GetMMU().HostWrite<u64>(guard, val, addr);
}

inline void Write_S8(const Core::CPUThreadGuard& guard, u32 addr, s8 val)
{
  guard.GetSystem().GetMMU().HostWrite<s8>(guard, val, addr);
}

inline void Write_S16(const Core::CPUThreadGuard& guard, u32 addr, s16 val)
{
  guard.GetSystem().GetMMU().HostWrite<s16>(guard, val, addr);
}

inline void Write_S32(const Core::CPUThreadGuard& guard, u32 addr, s32 val)
{
  guard.GetSystem().GetMMU().HostWrite<s32>(guard, val, addr);
}

inline void Write_S64(const Core::CPUThreadGuard& guard, u32 addr, s64 val)
{
  guard.GetSystem().GetMMU().HostWrite<s64>(guard, val, addr);
}

inline void Write_F32(const Core::CPUThreadGuard& guard, u32 addr, float val)
{
  guard.GetSystem().GetMMU().HostWrite<float>(guard, val, addr);
}

inline void Write_F64(const Core::CPUThreadGuard& guard, u32 addr, double val)
{
  guard.GetSystem().GetMMU().HostWrite<double>(guard, val, addr);
}

inline void Write_Bytes(const Core::CPUThreadGuard& guard, u32 addr, char* bytes, size_t size)
{
  for (size_t i = 0; i < size; ++i)
    guard.GetSystem().GetMMU().HostWrite<u8>(guard, static_cast<u8>(bytes[i]), addr + i);
}

}  // namespace API::Memory
