// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lldb-eval/context.h"

#include <memory>

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "lldb/API/SBExecutionContext.h"
#include "lldb/API/SBFrame.h"
#include "lldb/API/SBProcess.h"
#include "lldb/API/SBTarget.h"
#include "lldb/API/SBThread.h"
#include "lldb/API/SBType.h"
#include "lldb/API/SBValue.h"
#include "lldb/API/SBValueList.h"
#include "llvm/Support/FormatVariadic.h"

namespace lldb_eval {

Context::Context(std::string expr, lldb::SBExecutionContext ctx,
                 lldb::SBValue scope,
                 std::unordered_map<std::string, lldb::SBValue> context_vars)
    : expr_(std::move(expr)),
      ctx_(ctx),
      scope_(scope),
      context_vars_(std::move(context_vars)) {
  // This holds a SourceManager and all of its dependencies.
  smff_ = std::make_unique<clang::SourceManagerForFile>("<expr>", expr_);

  // Disable default diagnostics reporting.
  // TODO(werat): Add custom consumer to keep track of errors.
  clang::DiagnosticsEngine& de = smff_->get().getDiagnostics();
  de.setClient(new clang::IgnoringDiagConsumer);
}

lldb::SBType Context::ResolveTypeByName(const char* name) const {
  // TODO(b/163308825): Do scope-aware type lookup. Look for the types defined
  // in the current scope (function, class, namespace) and prioritize them.

  // Internally types don't have global scope qualifier in their names and
  // LLDB doesn't support queries with it too.
  llvm::StringRef name_ref(name);
  bool global_scope = false;

  if (name_ref.startswith("::")) {
    name_ref = name_ref.drop_front(2);
    global_scope = true;
  }

  // SBTarget::FindTypes will return all matched types, including the ones one
  // in different scopes. I.e. if seaching for "myint", this will also return
  // "ns::myint" and "Foo::myint".
  lldb::SBTypeList types = ctx_.GetTarget().FindTypes(name_ref.data());

  // We've found multiple types, try finding the "correct" one.
  lldb::SBType full_match;
  std::vector<lldb::SBType> partial_matches;

  for (uint32_t i = 0; i < types.GetSize(); ++i) {
    lldb::SBType type = types.GetTypeAtIndex(i);
    llvm::StringRef type_name = type.GetName();

    if (type_name == name_ref) {
      full_match = type;
    } else if (type_name.endswith(name_ref)) {
      partial_matches.push_back(type);
    }
  }

  if (global_scope) {
    // Look only for full matches when looking for a globally qualified type.
    if (full_match.IsValid()) {
      return full_match;
    }
  } else {
    // TODO(b/163308825): We're looking for type, but there may be multiple
    // candidates and which one is correct depends on the currect scope. For now
    // just pick the most "probable" type.

    // Full match is always correct if we're currently in the global scope.
    if (full_match.IsValid()) {
      return full_match;
    }

    // If we have partial matches, pick a "random" one.
    if (partial_matches.size() > 0) {
      return partial_matches.back();
    }
  }

  return lldb::SBType();
}

lldb::SBValue Context::LookupIdentifier(const char* name) const {
  // Lookup context variables first.
  auto context_var = context_vars_.find(name);
  if (context_var != context_vars_.end()) {
    return context_var->second;
  }

  // Internally values don't have global scope qualifier in their names and
  // LLDB doesn't support queries with it too.
  llvm::StringRef name_ref(name);
  bool global_scope = false;

  if (name_ref.startswith("::")) {
    name_ref = name_ref.drop_front(2);
    global_scope = true;
  }

  lldb::SBValue value;

  // If the identifier doesn't refer to the global scope and doesn't have any
  // other scope qualifiers, try looking among the local and instance variables.
  if (!global_scope && name_ref.find("::") == llvm::StringRef::npos) {
    if (!scope_) {
      // Lookup in the current frame.
      lldb::SBFrame frame = ctx_.GetFrame();
      // Try looking for a local variable in current scope.
      if (!value) {
        value = frame.FindVariable(name_ref.data());
      }
      // Try looking for an instance variable (class member).
      if (!value) {
        value =
            frame.FindVariable("this").GetChildMemberWithName(name_ref.data());
      }
    } else {
      // Lookup the variable as a member of the current scope value.
      value = scope_.GetChildMemberWithName(name_ref.data());
    }
  }

  // Try looking for a global or static variable.
  if (!value) {
    // TODO(werat): Implement scope-aware lookup. Relative scopes should be
    // resolved relative to the current scope. I.e. if the current frame is in
    // "ns1::ns2::Foo()", then "ns2::x" should resolve to "ns1::ns2::x".

    // List global variable with the same "basename". There can be many matches
    // from other scopes (namespaces, classes), so we do additional filtering
    // later.
    lldb::SBValueList values = ctx_.GetTarget().FindGlobalVariables(
        name_ref.data(), /*max_matches=*/std::numeric_limits<uint32_t>::max());

    // Find the corrent variable by matching the name. lldb::SBValue::GetName()
    // can return strings like "::globarVar", "ns::i" or "int const ns::foo"
    // depending on the version and the platform.
    for (uint32_t i = 0; i < values.GetSize(); ++i) {
      lldb::SBValue val = values.GetValueAtIndex(i);
      llvm::StringRef val_name = val.GetName();

      if (val_name == name_ref ||
          val_name == llvm::formatv("::{0}", name_ref).str() ||
          val_name.endswith(llvm::formatv(" {0}", name_ref).str())) {
        value = val;
        break;
      }
    }
  }

  return value;
}

std::shared_ptr<Context> Context::Create(
    std::string expr, lldb::SBFrame frame,
    std::unordered_map<std::string, lldb::SBValue> context_vars) {
  return std::shared_ptr<Context>(
      new Context(std::move(expr), lldb::SBExecutionContext(frame),
                  lldb::SBValue(), std::move(context_vars)));
}

std::shared_ptr<Context> Context::Create(
    std::string expr, lldb::SBValue scope,
    std::unordered_map<std::string, lldb::SBValue> context_vars) {
  // SBValues created via SBTarget::CreateValueFromData don't have SBFrame
  // associated with them. But they still have a process/target, so use that
  // instead.
  return std::shared_ptr<Context>(new Context(
      std::move(expr),
      lldb::SBExecutionContext(
          scope.GetProcess().GetSelectedThread().GetSelectedFrame()),
      scope, std::move(context_vars)));
}

}  // namespace lldb_eval
