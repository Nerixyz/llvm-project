//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGERUNTIME_CPLUSPLUS_MICROSOFTABI_MICROSOFTABILANGUAGERUNTIME_H
#define LLDB_SOURCE_PLUGINS_LANGUAGERUNTIME_CPLUSPLUS_MICROSOFTABI_MICROSOFTABILANGUAGERUNTIME_H

#include <map>
#include <mutex>
#include <vector>

#include "lldb/Breakpoint/BreakpointResolver.h"
#include "lldb/Core/Value.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Target/LanguageRuntime.h"
#include "lldb/lldb-private.h"

#include "Plugins/LanguageRuntime/CPlusPlus/CPPLanguageRuntime.h"

namespace lldb_private {

class MicrosoftABILanguageRuntime : public CPPLanguageRuntime {
public:
  static void Initialize();

  static void Terminate();

  static lldb_private::LanguageRuntime *
  CreateInstance(Process *process, lldb::LanguageType language);

  static llvm::StringRef GetPluginNameStatic() { return "microsoft-abi"; }

  static char ID;

  bool isA(const void *ClassID) const override {
    return ClassID == &ID || CPPLanguageRuntime::isA(ClassID);
  }

  static bool classof(const LanguageRuntime *runtime) {
    return runtime->isA(&ID);
  }

  llvm::Expected<LanguageRuntime::VTableInfo>
  GetVTableInfo(ValueObject &in_value, bool check_type) override;

  bool GetDynamicTypeAndAddress(ValueObject &in_value,
                                lldb::DynamicValueType use_dynamic,
                                TypeAndOrName &class_type_or_name,
                                Address &address, Value::ValueType &value_type,
                                llvm::ArrayRef<uint8_t> &local_buffer) override;

  TypeAndOrName FixUpDynamicType(const TypeAndOrName &type_and_or_name,
                                 ValueObject &static_value) override;

  bool CouldHaveDynamicValue(ValueObject &in_value) override;

  llvm::StringRef GetPluginName() override { return GetPluginNameStatic(); }

  lldb::BreakpointResolverSP
  CreateExceptionResolver(const lldb::BreakpointSP &bkpt, bool catch_bp,
                          bool throw_bp) override;

protected:
  llvm::Expected<LanguageRuntime::VTableInfo>
  GetVTableInfoWithOffset(ValueObject &in_value, bool check_type,
                          int64_t offset);

  bool IsVTableSymbolName(llvm::StringRef demangledName) const;

  /// Removes the vtable indicator from a demangled symbol.
  ///
  /// For example `vtable for Foo::Bar` -> `Foo::Bar`
  ///
  /// \param[in] demangledName The demangled name of the vtable symbol
  /// \returns The class name of the vtable
  llvm::StringRef StripVTableSymbolName(llvm::StringRef demangledName) const;

  std::optional<int64_t> GetOffsetToTop(const VTableInfo &vtable_info,
                                        ValueObject &in_value,
                                        const CompilerType &dynamic_type);

private:
  using DynamicTypeCache = std::map<lldb_private::Address, TypeAndOrName>;
  using VTableInfoCache = std::map<lldb_private::Address, VTableInfo>;

  DynamicTypeCache m_dynamic_type_map;
  VTableInfoCache m_vtable_info_map;
  std::mutex m_mutex;

  TypeAndOrName GetTypeInfo(ValueObject &in_value,
                            const VTableInfo &vtable_info);

  TypeAndOrName GetDynamicTypeInfo(const lldb_private::Address &vtable_addr);

  void SetDynamicTypeInfo(const lldb_private::Address &vtable_addr,
                          const TypeAndOrName &type_info);

  MicrosoftABILanguageRuntime(Process *process) : CPPLanguageRuntime(process) {}

  std::optional<int64_t> GetOffsetUsingRTTI(const VTableInfo &vtable_info,
                                            const CompilerType &dynamic_type);

  std::optional<int64_t> GetOffsetUsingAST(const VTableInfo &vtable_info,
                                           const CompilerType &dynamic_type);

  std::optional<std::pair<VTableInfo, int64_t>>
  MaybeAdjustForVBTable(const VTableInfo &vtable_info, ValueObject &in_value);
};

} // namespace lldb_private

#endif
