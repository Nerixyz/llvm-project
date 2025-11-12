//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MicrosoftABILanguageRuntime.h"

#include "Plugins/TypeSystem/Clang/TypeSystemClang.h"

#include "lldb/Core/PluginManager.h"
#include "lldb/Target/Process.h"

#include "lldb/Utility/LLDBLog.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/VTableBuilder.h"

#include <memory>

using namespace lldb;
using namespace lldb_private;

static const llvm::StringRef vtable_demangled_prefix =
    "const "; // for both vftables and vbtables
static const llvm::StringRef vftable_demangled_indicator = "::`vftable'";
static const llvm::StringRef vbtable_demangled_indicator = "::`vbtable'";
static const llvm::StringRef rtti_complete_object_locator_indicator =
    "::`RTTI Complete Object Locator'";

namespace {

llvm::SmallVector<llvm::StringRef, 1>
GetVFTableComponents(llvm::StringRef demangled) {
  llvm::StringRef context = demangled.split(vftable_demangled_indicator).second;
  context.consume_front("{for `");
  context.consume_back("'}");

  llvm::SmallVector<llvm::StringRef, 1> vec;
  if (!context.empty())
    context.split(vec, "'s `");

  return vec;
}

} // namespace

LLDB_PLUGIN_DEFINE_ADV(MicrosoftABILanguageRuntime, CXXMicrosoftABI)

char MicrosoftABILanguageRuntime::ID = 0;

LanguageRuntime *
MicrosoftABILanguageRuntime::CreateInstance(Process *process,
                                            lldb::LanguageType language) {
  if (!ShouldUseMicrosoftABI(process))
    return nullptr;

  if (!(language == eLanguageTypeC_plus_plus ||
        language == eLanguageTypeC_plus_plus_03 ||
        language == eLanguageTypeC_plus_plus_11 ||
        language == eLanguageTypeC_plus_plus_14))
    return nullptr;

  return new MicrosoftABILanguageRuntime(process);
}

void MicrosoftABILanguageRuntime::Initialize() {
  PluginManager::RegisterPlugin(GetPluginNameStatic(),
                                "Microsoft ABI for the C++ language",
                                CreateInstance);
}

void MicrosoftABILanguageRuntime::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

bool MicrosoftABILanguageRuntime::IsVTableSymbolName(
    llvm::StringRef demangledName) const {
  return demangledName.contains(vftable_demangled_indicator) ||
         demangledName.contains(vbtable_demangled_indicator);
}

llvm::StringRef MicrosoftABILanguageRuntime::StripVTableSymbolName(
    llvm::StringRef demangledName) const {
  auto indicator_idx = demangledName.find(vftable_demangled_indicator);
  if (indicator_idx == llvm::StringRef::npos)
    indicator_idx = demangledName.find(vbtable_demangled_indicator);
  if (indicator_idx == llvm::StringRef::npos)
    return llvm::StringRef();

  demangledName = demangledName.slice(0, indicator_idx);
  demangledName.consume_front(vtable_demangled_prefix);
  return demangledName;
}

std::optional<int64_t>
MicrosoftABILanguageRuntime::GetOffsetToTop(const VTableInfo &vtable_info,
                                            ValueObject &in_value,
                                            const CompilerType &dynamic_type) {
  // if we found a vbtable pointer, read that and jump to the respective vftable (if it exists)
  auto adjust = MaybeAdjustForVBTable(vtable_info, in_value);
  if (!adjust)
    return std::nullopt;
  const auto &[adjusted_info, adjustment_offset] = *adjust;

  // prefer getting the offset through RTTI
  auto from_rtti = GetOffsetUsingRTTI(adjusted_info, dynamic_type);
  if (from_rtti)
    return *from_rtti + adjustment_offset;

  // if the program was compiled without RTTI, use the AST
  auto from_ast = GetOffsetUsingAST(adjusted_info, dynamic_type);
  if (from_ast)
    return *from_ast + adjustment_offset;

  return std::nullopt;
}

std::optional<std::pair<LanguageRuntime::VTableInfo, int64_t>>
MicrosoftABILanguageRuntime::MaybeAdjustForVBTable(
    const VTableInfo &vtable_info, ValueObject &in_value) {
  // if this isn't a vbtable symbol, no adjustment is needed
  if (!vtable_info.symbol->GetName().GetStringRef().contains(
          vbtable_demangled_indicator))
    return std::pair{vtable_info, 0};

  Target &target = m_process->GetTarget();
  Log *log = GetLog(LLDBLog::Object);
  auto ts =
      target.GetScratchTypeSystemForLanguage(lldb::eLanguageTypeC_plus_plus);
  if (!ts) {
    LLDB_LOG_ERROR(log, ts.takeError(),
                   "Failed to get type system for C++: {0}");
    return std::nullopt;
  }
  auto int_size =
      (*ts)->GetBasicTypeFromAST(lldb::eBasicTypeInt).GetByteSize(nullptr);
  if (!int_size) {
    LLDB_LOG_ERROR(log, int_size.takeError(),
                   "Failed to determine size of int: {0}");
    return std::nullopt;
  }

  // The first vbase offset is the second int
  Address first_vbase_offset_addr = vtable_info.addr;
  first_vbase_offset_addr.Slide(*int_size);

  Status error;
  int64_t first_vbase_offset = target.ReadSignedIntegerFromMemory(
      first_vbase_offset_addr, *int_size, INT64_MIN, error, true);
  if (first_vbase_offset == INT64_MIN || error.Fail()) {
    LLDB_LOG_ERROR(log, error.takeError(), "Failed to read vbtable: {0}");
    return std::nullopt;
  }

  auto adjusted_info =
      GetVTableInfoWithOffset(in_value, false, first_vbase_offset);
  if (!adjusted_info) {
    LLDB_LOG_ERROR(log, error.takeError(),
                   "Failed to get vtable info of virtual base: {0}");
    return std::nullopt;
  }
  if (!adjusted_info->symbol->GetName().GetStringRef().contains(
          vftable_demangled_indicator)) {
    LLDB_LOG(log, "Adjusted vtable info was not a vftable (symbol: {0})",
             adjusted_info->symbol->GetName());
    return std::nullopt;
  }

  return std::pair{*adjusted_info, first_vbase_offset};
}

std::optional<int64_t> MicrosoftABILanguageRuntime::GetOffsetUsingRTTI(
    const VTableInfo &vtable_info, const CompilerType &dynamic_type) {
  // The pointer to the RTTI complete object locator sits one pointer size above
  // the vftable:
  // [-8] | RTTI Complete Object Locator
  // [ 0] | VFTable entry #0
  // [ 8] | VFTable entry #1 ... and so on

  Target &target = m_process->GetTarget();
  const addr_t vtable_load_addr = vtable_info.addr.GetLoadAddress(&target);
  if (vtable_load_addr == LLDB_INVALID_ADDRESS)
    return std::nullopt;
  const uint32_t addr_byte_size = m_process->GetAddressByteSize();
  const addr_t complete_object_locator_load_addr =
      vtable_load_addr - addr_byte_size;
  // Check for underflow
  if (complete_object_locator_load_addr >= vtable_load_addr)
    return std::nullopt;

  Address complete_object_locator_addr;
  if (!m_process->GetTarget().ResolveLoadAddress(
          complete_object_locator_load_addr, complete_object_locator_addr))
    return std::nullopt;

  Log *log = GetLog(LLDBLog::Object);
  Status error;
  // Pointer to the RTTI complete object locator
  Address rtti_locator_base_addr;
  if (!target.ReadPointerFromMemory(complete_object_locator_addr, error,
                                    rtti_locator_base_addr, true)) {
    LLDB_LOG_ERROR(
        log, error.takeError(),
        "Failed to read address of RTTI complete object locator: {0}");
    return std::nullopt;
  }

  // Check if we're actually looking at the RTTI complete object locator
  Symbol *symbol = rtti_locator_base_addr.CalculateSymbolContextSymbol();
  if (symbol == nullptr)
    return std::nullopt;
  if (!symbol->GetName().GetStringRef().contains(
          rtti_complete_object_locator_indicator))
    return std::nullopt;

  auto ts = dynamic_type.GetTypeSystem();
  if (!ts)
    return std::nullopt;

  llvm::Expected<uint64_t> int_size =
      ts->GetBasicTypeFromAST(lldb::eBasicTypeInt).GetByteSize(nullptr);
  if (!int_size) {
    LLDB_LOG_ERROR(log, int_size.takeError(),
                   "Failed to get size of int from AST: {0}");
    return std::nullopt;
  }

  // See MSRTTIBuilder::getCompleteObjectLocator in Clang for the layout.
  // We're interested in the offset which is the second int in the struct.
  Address offset_from_top_addr = rtti_locator_base_addr;
  offset_from_top_addr.Slide(*int_size);

  error.Clear();
  const int64_t offset_from_top = target.ReadSignedIntegerFromMemory(
      offset_from_top_addr, *int_size, INT64_MIN, error);
  if (offset_from_top == INT64_MIN || error.Fail()) {
    LLDB_LOG_ERROR(log, error.takeError(),
                   "Failed to read 'offset from top' from RTTI complete object "
                   "locator: {0}");
    return std::nullopt;
  }

  return -offset_from_top;
}
