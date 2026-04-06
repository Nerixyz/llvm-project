//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MicrosoftABIRuntime.h"

#include "Plugins/TypeSystem/Clang/TypeSystemClang.h"
#include "lldb/Utility/LLDBLog.h"
#include "clang/AST/RecordLayout.h"

#include <deque>

using namespace lldb;
using namespace lldb_private;

static const llvm::StringRef vtable_demangled_indicator = "::`vftable'";
static const llvm::StringRef rtti_complete_object_locator_indicator =
    "::`RTTI Complete Object Locator'";

static llvm::StringRef stripDemangledVTableSuffix(llvm::StringRef demangled) {
  llvm::StringRef name = demangled.split(vtable_demangled_indicator).first;
  name.consume_front("const ");
  return name;
}

static bool
splitVTableComponents(llvm::StringRef demangled,
                      llvm::SmallVectorImpl<llvm::StringRef> &components) {
  llvm::StringRef suffix = demangled.split(vtable_demangled_indicator).second;
  if (suffix.empty())
    return true; // e.g. MyClass::`vftable'

  // Otherwise, suffix looks like "{for `MyBase2's `MyBase1'}" or
  // "{for `MyBase'}".
  if (!suffix.consume_front("{for `") || !suffix.consume_back("'}"))
    return false;

  suffix.split(components, "'s `");
  return true;
}

MicrosoftABIRuntime::MicrosoftABIRuntime(Process *process)
    : CommonABIRuntime(process) {}

bool MicrosoftABIRuntime::GetDynamicTypeAndAddress(
    ValueObject &in_value, lldb::DynamicValueType use_dynamic,
    const LanguageRuntime::VTableInfo &vtable_info,
    TypeAndOrName &class_type_or_name, Address &dynamic_address) {
  std::optional<size_t> offset_to_top =
      GetOffsetToTop(in_value, class_type_or_name, vtable_info);
  if (!offset_to_top)
    return false;

  lldb::addr_t dynamic_addr =
      in_value.GetPointerValue().address - *offset_to_top;
  if (!m_process->GetTarget().ResolveLoadAddress(dynamic_addr, dynamic_address))
    dynamic_address.SetRawAddress(dynamic_addr);

  return true;
}

bool MicrosoftABIRuntime::IsVTableSymbol(Mangled &mangled) const {
  return mangled.GetDemangledName().GetStringRef().contains(
      vtable_demangled_indicator);
}

TypeAndOrName MicrosoftABIRuntime::GetTypeInfo(
    ValueObject &in_value, const LanguageRuntime::VTableInfo &vtable_info,
    llvm::SmallVectorImpl<llvm::StringRef> &vtable_components) {
  if (!vtable_info.addr.IsSectionOffset() || !vtable_info.symbol)
    return {};

  // See if we have cached info for this type already
  TypeAndOrName type_info = GetDynamicTypeInfo(vtable_info.addr);
  if (type_info)
    return type_info;

  Log *log = GetLog(LLDBLog::Object);
  llvm::StringRef symbol_name =
      vtable_info.symbol->GetMangled().GetDemangledName().GetStringRef();
  if (!splitVTableComponents(symbol_name, vtable_components)) {
    LLDB_LOG(log, "{0:x16}: '{1}' is not a valid vtable symbol",
             in_value.GetPointerValue().address, symbol_name);
    return {};
  }

  llvm::StringRef class_name = stripDemangledVTableSuffix(symbol_name);

  type_info.SetName(class_name);
  TypeSP type_sp = LookupTypeByName(
      class_name, vtable_info.symbol->CalculateSymbolContextModule());
  if (type_sp) {
    LLDB_LOG(log,
             "static-type = '{0}' has dynamic type: uid={1:x}, type-name='{2}'",
             in_value.GetTypeName(), type_sp->GetID(), type_sp->GetName());
    type_info.SetTypeSP(std::move(type_sp));
  }

  if (type_info)
    SetDynamicTypeInfo(vtable_info.addr, type_info);
  return type_info;
}

std::optional<int64_t> MicrosoftABIRuntime::GetOffsetToTop(
    ValueObject &in_value, TypeAndOrName &class_type_or_name,
    const LanguageRuntime::VTableInfo &vtable_info) {
  Log *log = GetLog(LLDBLog::Object);

  llvm::SmallVector<llvm::StringRef, 2> vtable_components;
  class_type_or_name = GetTypeInfo(in_value, vtable_info, vtable_components);
  if (!class_type_or_name) {
    LLDB_LOG(log, "{0:x16}: failed to get type info",
             in_value.GetPointerValue().address);
    return std::nullopt;
  }

  {
    std::lock_guard g(m_mutex);
    auto offset_to_top_it = m_offset_to_top_map.find(vtable_info.addr);
    if (offset_to_top_it != m_offset_to_top_map.end())
      return offset_to_top_it->second;
  }

  CompilerType type = class_type_or_name.GetCompilerType();
  // There can only be one type with a given name, so we've just found
  // duplicate definitions, and this one will do as well as any other. We
  // don't consider something to have a dynamic type if it is the same as
  // the static type.  So compare against the value we were handed.
  if (!type) {
    LLDB_LOG(log, "{0:x16}: failed to get type info (no compiler type)",
             in_value.GetPointerValue().address);
    return std::nullopt;
  }

  CompilerType in_type = in_value.GetCompilerType();
  if (in_type.IsPointerOrReferenceType())
    in_type = in_type.GetPointeeType();
  if (!in_type.GetCompleteType()) {
    LLDB_LOG(log, "{0:x16}: {1} is not a complete type",
             in_value.GetPointerValue().address, in_type.GetTypeName());
    return std::nullopt;
  }
  if (!type.GetCompleteType()) {
    LLDB_LOG(log, "{0:x16}: {1} is not a complete type",
             in_value.GetPointerValue().address,
             type.GetTypeName().GetCString());
    return std::nullopt;
  }

  if (TypeSystemClang::AreTypesSame(in_type, type)) {
    LLDB_LOG(log, "{0:x16}: in type is the same as the dynamic type",
             in_value.GetPointerValue().address);

    // The dynamic type we found was the same type, so we don't have a
    // dynamic type here, but if a derived type encounters this vtable symbol,
    // remember that it has offset 0.
    std::lock_guard g(m_mutex);
    m_offset_to_top_map[vtable_info.addr] = 0;
    return std::nullopt;
  }

  // Unlike in the Itanium ABI, the VTable itself doesn't have a field telling
  // us the offset to the dynamic type.
  // When compiled with RTTI, we can use the complete object locator to get this
  // offset. This is preferred, because it can resolve types across different
  // modules and handle virtual bases with PDB.
  // If we fail there (e.g. no RTTI available), we try to find the offset from
  // the AST we built by doing a BFS. This requires the types to be from the
  // same type system. Because PDB doesn't include the offsets of virtual bases,
  // we don't attempt to resolve these. They're technically present in the AST,
  // but they often have incorrect offsets.
  std::optional<int64_t> offset = GetOffsetToTopFromRTTI(vtable_info, type);
  llvm::StringRef strategy = "RTTI";
  if (!offset) {
    offset = GetOffsetToTopFromAST(in_type, in_value, type, vtable_info,
                                   vtable_components);
    strategy = "AST";
  }

  if (offset) {
    LLDB_LOG(GetLog(LLDBLog::Object),
             "{0:x16}: [{1}] '{2}' has offset {3} in '{4}'",
             in_value.GetPointerValue().address, strategy,
             in_type.GetTypeName(), *offset, type.GetTypeName());

    std::lock_guard g(m_mutex);
    m_offset_to_top_map[vtable_info.addr] = offset;
  }

  return offset;
}

std::optional<int64_t> MicrosoftABIRuntime::GetOffsetToTopFromRTTI(
    const LanguageRuntime::VTableInfo &vtable_info,
    const CompilerType &dynamic_type) {
  // The pointer to the RTTI complete object locator sits one pointer size above
  // the vftable:
  //                    [-8] | RTTI Complete Object Locator
  // vtable_info.addr-> [ 0] | VFTable entry #0
  //                    [ 8] | VFTable entry #1 ... and so on

  Target &target = m_process->GetTarget();
  const addr_t vtable_load_addr = vtable_info.addr.GetLoadAddress(&target);
  if (vtable_load_addr == LLDB_INVALID_ADDRESS)
    return std::nullopt;
  const uint32_t addr_byte_size = m_process->GetAddressByteSize();
  const lldb::addr_t complete_object_locator_load_addr =
      vtable_load_addr - addr_byte_size;
  // Check for underflow.
  if (complete_object_locator_load_addr >= vtable_load_addr)
    return std::nullopt;

  Log *log = GetLog(LLDBLog::Object);
  Status error;
  // Pointer to the RTTI complete object locator.
  lldb::addr_t rtti_locator_base_load_addr = m_process->ReadPointerFromMemory(
      complete_object_locator_load_addr, error);
  if (rtti_locator_base_load_addr == LLDB_INVALID_ADDRESS || error.Fail()) {
    LLDB_LOG_ERROR(log, error.takeError(),
                   "Failed to read address of RTTI complete object locator at "
                   "{1:x16}: {0}",
                   complete_object_locator_load_addr);
    return std::nullopt;
  }

  Address rtti_locator_base_addr;
  if (!m_process->GetTarget().ResolveLoadAddress(rtti_locator_base_load_addr,
                                                 rtti_locator_base_addr)) {
    LLDB_LOG_ERROR(log, error.takeError(),
                   "Failed to resolve load address of RTTI complete object "
                   "locator at {1:x16}: {0}",
                   rtti_locator_base_load_addr);
    return std::nullopt;
  }

  // Check if we're actually looking at the RTTI complete object locator.
  Symbol *symbol = rtti_locator_base_addr.CalculateSymbolContextSymbol();
  if (symbol == nullptr)
    return std::nullopt;
  if (!symbol->GetName().GetStringRef().contains(
          rtti_complete_object_locator_indicator))
    return std::nullopt;

  LLDB_LOG(log, "Found RTTI complete object locator: {0:x16}",
           rtti_locator_base_load_addr);

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
                   "locator at {1:x16}: {0}",
                   offset_from_top_addr.GetOffset());
    return std::nullopt;
  }

  return offset_from_top;
}

static std::optional<size_t>
getOffsetOfBaseFromAST(clang::ASTContext &ast,
                       const clang::CXXRecordDecl *haystack,
                       const clang::CXXRecordDecl *target_base,
                       const llvm::ArrayRef<llvm::StringRef> checkpoints) {
  struct BaseEntry {
    const clang::CXXRecordDecl *decl;
    /// If this is `std::nullopt`, then this entry is on a path where we can't
    /// determine the offset.
    std::optional<size_t> offset;
    llvm::ArrayRef<llvm::StringRef> checkpoints;
    bool found_target = false;
  };
  std::deque<BaseEntry> bases;
  bases.emplace_back(BaseEntry{
      /*decl=*/haystack,
      /*offset=*/0,
      /*checkpoints=*/checkpoints,
  });

  // Go through virtual bases first.
  std::string base_name;
  for (const clang::CXXBaseSpecifier &base : haystack->vbases()) {
    const clang::CXXRecordDecl *base_decl = llvm::cast<clang::CXXRecordDecl>(
        base.getType()->castAs<clang::RecordType>()->getDecl());
    // FIXME: With PDB/CodeView, the offset for this virtual base is wrong if
    // the other bases are not aligned, because the debug info doesn't include
    // any offset.
    if (!checkpoints.empty()) {
      base_name = base_decl->getDeclName().getAsString();
      if (base_name == checkpoints.back())
        return std::nullopt;
    } else if (base_decl == target_base) {
      return std::nullopt;
    }

    // We still need to keep track of this class as `target_base` might be
    // inside this base.
    bases.emplace_back(BaseEntry{
        /*decl=*/base_decl,
        /*offset=*/std::nullopt,
        /*checkpoints=*/checkpoints,
    });
  }

  // BFS over the bases.
  while (!bases.empty()) {
    BaseEntry current = bases.front();
    assert(
        (!current.found_target || !current.checkpoints.empty()) &&
        "If we found the target we should only need to check the checkpoints");
    bases.pop_front();
    const clang::ASTRecordLayout &record_layout =
        ast.getASTRecordLayout(current.decl);

    for (const clang::CXXBaseSpecifier &base : current.decl->bases()) {
      if (base.isVirtual())
        continue; // We already looked through virtual bases.

      const clang::CXXRecordDecl *base_decl = llvm::cast<clang::CXXRecordDecl>(
          base.getType()->castAs<clang::RecordType>()->getDecl());
      bool found_target = base_decl == target_base || current.found_target;
      std::optional<size_t> offset = current.offset;
      if (offset)
        *offset += record_layout.getBaseClassOffset(base_decl).getQuantity();

      if (!current.checkpoints.empty()) {
        base_name = base_decl->getDeclName().getAsString();
        if (base_name == current.checkpoints.back()) {
          // Last component was the base we were looking for.
          if (found_target && current.checkpoints.size() == 1)
            return offset;

          // Found a checkpoint, so we have to take this path.
          bases.clear();
          bases.emplace_back(BaseEntry{
              /*decl=*/base_decl,
              /*offset=*/offset,
              /*checkpoints=*/current.checkpoints.drop_back(),
              /*found_target=*/found_target,
          });
        }
      } else if (found_target) {
        return offset;
      }

      bases.emplace_back(BaseEntry{
          /*decl=*/base_decl,
          /*offset=*/offset,
          /*checkpoints=*/current.checkpoints,
          /*found_target=*/found_target,
      });
    }
  }

  return std::nullopt;
}

std::optional<int64_t> MicrosoftABIRuntime::GetOffsetToTopFromAST(
    const CompilerType &in_type, ValueObject &in_value,
    const CompilerType &dyn_type,
    const LanguageRuntime::VTableInfo &vtable_info,
    llvm::ArrayRef<llvm::StringRef> vtable_components) {
  Log *log = GetLog(LLDBLog::Object);

  CompilerType in_base_type = in_type;
  // if (!vtable_components.empty()) {
  // Find the type of the first vtable component.
  //
  // For example, if we have the symbol
  // MyType::`vftable'{for `Base's `Derived'} (i.e. MyType -> Derived -> Base)
  // and `in_value` is `Derived`, then we will actually search for the offset
  // in `Base`. Note that not every base has to occur in the components,
  // `in_value` might be a base that sits between `Derived` and `Base`.
  // Because we got the vtable for `Base` while `in_value` is
  // `Derived`, we can assume that `Base` sits at offset 0 in `Derived`.
  // ConstString in_name = in_type.GetTypeName();
  // if (in_name != vtable_components.front()) {
  //   TypeSP base =
  //       LookupTypeByName(vtable_components.front(),
  //                        vtable_info.symbol->CalculateSymbolContextModule());
  //   if (base)
  //     in_base_type = base->GetFullCompilerType();

  //   if (!in_base_type) {
  //     LLDB_LOG(log, "{0:x16}: Could not find '{1}' as the target base",
  //              in_value.GetPointerValue().address,
  //              vtable_components.front());
  //     return std::nullopt;
  //   }
  // }
  // }

  clang::CXXRecordDecl *in_decl =
      TypeSystemClang::GetAsCXXRecordDecl(in_base_type.GetOpaqueQualType());
  clang::CXXRecordDecl *target_decl =
      TypeSystemClang::GetAsCXXRecordDecl(dyn_type.GetOpaqueQualType());
  if (!in_decl || !target_decl) {
    LLDB_LOG(log, "{0:x16}: types are not record types (in={1}, target={2})",
             in_value.GetPointerValue().address, in_base_type.GetTypeName(),
             dyn_type.GetTypeName());
    return std::nullopt;
  }

  TypeSystemClangSP ts = dyn_type.GetTypeSystem<TypeSystemClang>();
  if (!ts || ts != in_base_type.GetTypeSystem<TypeSystemClang>()) {
    LLDB_LOG(GetLog(LLDBLog::Object),
             "{0:x16}: '{1}' and '{2}' are from different type systems",
             in_value.GetPointerValue().address,
             in_decl->getDeclName().getAsString(),
             target_decl->getDeclName().getAsString());
    return std::nullopt;
  }

  std::optional<size_t> offset = getOffsetOfBaseFromAST(
      ts->getASTContext(), target_decl, in_decl, vtable_components);
  if (!offset) {
    LLDB_LOG(GetLog(LLDBLog::Object),
             "{0:x16}: failed to find offset of '{1}' in '{2}'",
             in_value.GetPointerValue().address,
             in_decl->getDeclName().getAsString(),
             target_decl->getDeclName().getAsString());
    return std::nullopt;
  }

  return offset;
}
