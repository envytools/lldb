//===-- SBInstruction.cpp ---------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/SBInstruction.h"

#include "lldb/API/SBAddress.h"
#include "lldb/API/SBFrame.h"
#include "lldb/API/SBInstruction.h"
#include "lldb/API/SBStream.h"
#include "lldb/API/SBTarget.h"

#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/DataBufferHeap.h"
#include "lldb/Core/DataExtractor.h"
#include "lldb/Core/Disassembler.h"
#include "lldb/Core/EmulateInstruction.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"

using namespace lldb;
using namespace lldb_private;

SBInstruction::SBInstruction ()
{
}

SBInstruction::SBInstruction (const lldb::InstructionSP& inst_sp) :
    m_opaque_sp (inst_sp)
{
}

SBInstruction::SBInstruction(const SBInstruction &rhs) :
    m_opaque_sp (rhs.m_opaque_sp)
{
}

const SBInstruction &
SBInstruction::operator = (const SBInstruction &rhs)
{
    if (this != &rhs)
        m_opaque_sp = rhs.m_opaque_sp;
    return *this;
}

SBInstruction::~SBInstruction ()
{
}

bool
SBInstruction::IsValid()
{
    return (m_opaque_sp.get() != NULL);
}

SBAddress
SBInstruction::GetAddress()
{
    SBAddress sb_addr;
    if (m_opaque_sp && m_opaque_sp->GetAddress().IsValid())
        sb_addr.SetAddress(&m_opaque_sp->GetAddress());
    return sb_addr;
}

const char *
SBInstruction::GetMnemonic(SBTarget target)
{
    if (m_opaque_sp)
    {        
        ExecutionContext exe_ctx;
        TargetSP target_sp (target.GetSP());
        std::unique_lock<std::recursive_mutex> lock;
        if (target_sp)
        {
            lock = std::unique_lock<std::recursive_mutex>(target_sp->GetAPIMutex());

            target_sp->CalculateExecutionContext (exe_ctx);
            exe_ctx.SetProcessSP(target_sp->GetProcessSP());
        }
        return m_opaque_sp->GetMnemonic(&exe_ctx);
    }
    return NULL;
}

const char *
SBInstruction::GetOperands(SBTarget target)
{
    if (m_opaque_sp)
    {
        ExecutionContext exe_ctx;
        TargetSP target_sp (target.GetSP());
        std::unique_lock<std::recursive_mutex> lock;
        if (target_sp)
        {
            lock = std::unique_lock<std::recursive_mutex>(target_sp->GetAPIMutex());

            target_sp->CalculateExecutionContext (exe_ctx);
            exe_ctx.SetProcessSP(target_sp->GetProcessSP());
        }
        return m_opaque_sp->GetOperands(&exe_ctx);
    }
    return NULL;
}

const char *
SBInstruction::GetComment(SBTarget target)
{
    if (m_opaque_sp)
    {
        ExecutionContext exe_ctx;
        TargetSP target_sp (target.GetSP());
        std::unique_lock<std::recursive_mutex> lock;
        if (target_sp)
        {
            lock = std::unique_lock<std::recursive_mutex>(target_sp->GetAPIMutex());

            target_sp->CalculateExecutionContext (exe_ctx);
            exe_ctx.SetProcessSP(target_sp->GetProcessSP());
        }
        return m_opaque_sp->GetComment(&exe_ctx);
    }
    return NULL;
}

size_t
SBInstruction::GetByteSize ()
{
    if (m_opaque_sp)
        return m_opaque_sp->GetOpcode().GetByteSize();
    return 0;
}

SBData
SBInstruction::GetData (SBTarget target)
{
    lldb::SBData sb_data;
    if (m_opaque_sp)
    {
        DataExtractorSP data_extractor_sp (new DataExtractor());
        if (m_opaque_sp->GetData (*data_extractor_sp))
        {
            sb_data.SetOpaque (data_extractor_sp);
        }
    }
    return sb_data;
}



bool
SBInstruction::DoesBranch ()
{
    if (m_opaque_sp)
        return m_opaque_sp->DoesBranch ();
    return false;
}

bool
SBInstruction::HasDelaySlot ()
{
    if (m_opaque_sp)
        return m_opaque_sp->HasDelaySlot ();
    return false;
}

void
SBInstruction::SetOpaque (const lldb::InstructionSP &inst_sp)
{
    m_opaque_sp = inst_sp;
}

bool
SBInstruction::GetDescription (lldb::SBStream &s)
{
    if (m_opaque_sp)
    {
        SymbolContext sc;
        const Address &addr = m_opaque_sp->GetAddress();
        ModuleSP module_sp (addr.GetModule());
        if (module_sp)
            module_sp->ResolveSymbolContextForAddress(addr, eSymbolContextEverything, sc);
        // Use the "ref()" instead of the "get()" accessor in case the SBStream 
        // didn't have a stream already created, one will get created...
        FormatEntity::Entry format;
        FormatEntity::Parse("${addr}: ", format);
        m_opaque_sp->Dump (&s.ref(), 0, true, false, NULL, &sc, NULL, &format, 0);
        return true;
    }
    return false;
}

void
SBInstruction::Print (FILE *out)
{
    if (out == NULL)
        return;

    if (m_opaque_sp)
    {
        SymbolContext sc;
        const Address &addr = m_opaque_sp->GetAddress();
        ModuleSP module_sp (addr.GetModule());
        if (module_sp)
            module_sp->ResolveSymbolContextForAddress(addr, eSymbolContextEverything, sc);
        StreamFile out_stream (out, false);
        FormatEntity::Entry format;
        FormatEntity::Parse("${addr}: ", format);
        m_opaque_sp->Dump (&out_stream, 0, true, false, NULL, &sc, NULL, &format, 0);
    }
}

bool
SBInstruction::EmulateWithFrame (lldb::SBFrame &frame, uint32_t evaluate_options)
{
    if (m_opaque_sp)
    {
        lldb::StackFrameSP frame_sp (frame.GetFrameSP());

        if (frame_sp)
        {
            lldb_private::ExecutionContext exe_ctx;
            frame_sp->CalculateExecutionContext (exe_ctx);
            lldb_private::Target *target = exe_ctx.GetTargetPtr();
            lldb_private::ArchSpec arch = target->GetArchitecture();
            
            return m_opaque_sp->Emulate (arch, 
                                         evaluate_options,
                                         (void *) frame_sp.get(), 
                                         &lldb_private::EmulateInstruction::ReadMemoryFrame,
                                         &lldb_private::EmulateInstruction::WriteMemoryFrame,
                                         &lldb_private::EmulateInstruction::ReadRegisterFrame,
                                         &lldb_private::EmulateInstruction::WriteRegisterFrame);
        }
    }
    return false;
}

bool
SBInstruction::DumpEmulation (const char *triple)
{
    if (m_opaque_sp && triple)
    {
        lldb_private::ArchSpec arch (triple, NULL);
        
        return m_opaque_sp->DumpEmulation (arch);
                                     
    }
    return false;
}

bool
SBInstruction::TestEmulation (lldb::SBStream &output_stream,  const char *test_file)
{
    if (!m_opaque_sp.get())
        m_opaque_sp.reset (new PseudoInstruction());
        
    return m_opaque_sp->TestEmulation (output_stream.get(), test_file);
}

lldb::AddressClass
SBInstruction::GetAddressClass ()
{
    if (m_opaque_sp.get())
        return m_opaque_sp->GetAddressClass();
    return eAddressClassInvalid;
}
