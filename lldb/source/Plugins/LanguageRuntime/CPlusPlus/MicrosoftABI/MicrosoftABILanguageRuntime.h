//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGERUNTIME_CPLUSPLUS_MICROSOFTABI_MICROSOFTABILANGUAGERUNTIME_H
#define LLDB_SOURCE_PLUGINS_LANGUAGERUNTIME_CPLUSPLUS_MICROSOFTABI_MICROSOFTABILANGUAGERUNTIME_H

#include "Plugins/LanguageRuntime/CPlusPlus/ItaniumABI/ItaniumABILanguageRuntime.h"

namespace lldb_private {

class MicrosoftABILanguageRuntime : public ItaniumABILanguageRuntime {
public:
  static void Initialize();

  static void Terminate();

  static lldb_private::LanguageRuntime *
  CreateInstance(Process *process, lldb::LanguageType language);

  static llvm::StringRef GetPluginNameStatic() { return "microsoft-abi"; }

  static char ID;

  bool isA(const void *ClassID) const override { return ClassID == &ID; }

  static bool classof(const LanguageRuntime *runtime) {
    return runtime->isA(&ID);
  }

  llvm::StringRef GetPluginName() override { return GetPluginNameStatic(); }

protected:
  bool IsVTableSymbolName(llvm::StringRef demangledName) const override;

  llvm::StringRef
  StripVTableSymbolName(llvm::StringRef demangledName) const override;

  std::optional<int64_t>
  GetOffsetToTop(const VTableInfo &vtable_info, ValueObject &in_value,
                 const CompilerType &dynamic_type) override;

private:
  MicrosoftABILanguageRuntime(Process *process)
      : ItaniumABILanguageRuntime(process) {}

  std::optional<int64_t> GetOffsetUsingRTTI(const VTableInfo &vtable_info,
                                            const CompilerType &dynamic_type);

  std::optional<int64_t> GetOffsetUsingAST(const VTableInfo &vtable_info,
                                           const CompilerType &dynamic_type);

  std::optional<std::pair<VTableInfo, int64_t>>
  MaybeAdjustForVBTable(const VTableInfo &vtable_info, ValueObject &in_value);
};

} // namespace lldb_private

#endif
