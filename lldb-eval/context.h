/*
 * Copyright 2020 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LLDB_EVAL_EXPRESSION_CONTEXT_H_
#define LLDB_EVAL_EXPRESSION_CONTEXT_H_

#include <memory>
#include <string>

#include "clang/Basic/SourceManager.h"
#include "lldb/API/SBExecutionContext.h"
#include "lldb/API/SBFrame.h"
#include "lldb/API/SBType.h"
#include "lldb/API/SBValue.h"

namespace lldb_eval {

enum class ErrorCode : unsigned char {
  kOk = 0,
  kInvalidExpressionSyntax,
  kInvalidNumericLiteral,
  kInvalidOperandType,
  kUndeclaredIdentifier,
  kNotImplemented,
  kUnknown,
};

class Error {
 public:
  void Set(ErrorCode code, std::string message) {
    code_ = code;
    message_ = std::move(message);
  }
  void Clear() { *this = {}; }

  ErrorCode code() const { return code_; }
  const std::string& message() const { return message_; }

  explicit operator bool() const { return code_ != ErrorCode::kOk; }

 private:
  ErrorCode code_ = ErrorCode::kOk;
  std::string message_;
};

class Context {
 public:
  static std::shared_ptr<Context> Create(std::string expr, lldb::SBFrame frame);
  static std::shared_ptr<Context> Create(std::string expr, lldb::SBValue scope);

  // This class cannot be safely moved because of the dependency between `expr_`
  // and `smff_`. Users are supposed to pass around the shared pointer.
  Context(Context&&) = delete;
  Context(const Context&) = delete;
  Context& operator=(Context const&) = delete;

  clang::SourceManager& GetSourceManager() const { return smff_->get(); }
  lldb::SBExecutionContext GetExecutionContext() const { return ctx_; }

 public:
  lldb::SBType ResolveTypeByName(const std::string& name) const;
  lldb::SBValue LookupIdentifier(const std::string& name) const;

 private:
  Context(std::string expr, lldb::SBExecutionContext ctx, lldb::SBValue scope);

 public:
  // Store the expression, since SourceManager doesn't take the ownership.
  std::string expr_;
  std::unique_ptr<clang::SourceManagerForFile> smff_;

  // The expression exists in the context of an LLDB target. Execution context
  // provides information for semantic analysis (e.g. resolving types, looking
  // up variables, etc).
  lldb::SBExecutionContext ctx_;

  // If set, the expression is evaluated in the scope of this value: `scope_` is
  // used as `this` pointer and local variables from the current frame are not
  // available.
  mutable lldb::SBValue scope_;
};

}  // namespace lldb_eval

#endif  // LLDB_EVAL_EXPRESSION_CONTEXT_H_
