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

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <utility>

#include "cpp-linenoise/linenoise.hpp"
#include "lldb-eval/api.h"
#include "lldb-eval/runner.h"
#include "lldb/API/SBBreakpoint.h"
#include "lldb/API/SBDebugger.h"
#include "lldb/API/SBError.h"
#include "lldb/API/SBExecutionContext.h"
#include "lldb/API/SBFrame.h"
#include "lldb/API/SBProcess.h"
#include "lldb/API/SBTarget.h"
#include "lldb/API/SBThread.h"
#include "lldb/API/SBValue.h"
#include "tools/cpp/runfiles/runfiles.h"
#include "tools/fuzzer/ast.h"
#include "tools/fuzzer/expr_gen.h"
#include "tools/fuzzer/symbol_table.h"

using bazel::tools::cpp::runfiles::Runfiles;

static constexpr char SOURCE_PATH_KEY[] = "lldb_eval/testdata/fuzzer_binary.cc";
static constexpr char BINARY_PATH_KEY[] = "lldb_eval/testdata/fuzzer_binary";

enum Verbosity {
  ShowMismatchesOrErrors,
  ShowEverything,
};

void eval_and_print_expr(lldb::SBFrame& frame, const std::string& expr,
                         Verbosity verbosity) {
  auto lldb_value = frame.EvaluateExpression(expr.c_str());
  auto lldb_err = lldb_value.GetError();

  lldb::SBError lldb_eval_err;
  auto lldb_eval_value =
      lldb_eval::EvaluateExpression(frame, expr.c_str(), lldb_eval_err);

  bool value_mismatch;
  if (lldb_value.GetValue() != nullptr &&
      lldb_eval_value.GetValue() != nullptr) {
    value_mismatch =
        strcmp(lldb_value.GetValue(), lldb_eval_value.GetValue()) != 0;
  } else {
    // Pointer comparison: Mismatch if one value is null and the other is not
    value_mismatch = lldb_value.GetValue() != lldb_eval_value.GetValue();
  }

  bool type_mismatch;
  if (lldb_value.GetTypeName() != nullptr &&
      lldb_eval_value.GetTypeName() != nullptr) {
    type_mismatch =
        strcmp(lldb_value.GetTypeName(), lldb_eval_value.GetTypeName()) != 0;
  } else {
    // Pointer comparison: Mismatch if one type is null and the other is not
    type_mismatch = lldb_value.GetTypeName() != lldb_eval_value.GetTypeName();
  }
  bool has_error =
      lldb_err.GetCString() != nullptr || lldb_eval_err.GetCString() != nullptr;

  bool must_print = value_mismatch || type_mismatch || has_error ||
                    verbosity == Verbosity::ShowEverything;
  if (!must_print) {
    return;
  }
  printf("expr : `%s`\n", expr.c_str());

  if (value_mismatch) {
    if (lldb_value.GetValue() != nullptr) {
      printf("lldb value     : `%s`\n", lldb_value.GetValue());
    } else {
      printf("lldb value     : No value returned\n");
    }
    if (lldb_eval_value.GetValue() != nullptr) {
      printf("lldb-eval value: `%s`\n", lldb_eval_value.GetValue());
    } else {
      printf("lldb-eval value: No value returned\n");
    }
  } else if (verbosity == Verbosity::ShowEverything) {
    printf("value: `%s`\n", lldb_value.GetValue());
  }

  if (type_mismatch) {
    if (lldb_value.GetTypeName() != nullptr) {
      printf("lldb type     : `%s`\n", lldb_value.GetTypeName());
    } else {
      printf("lldb type     : No type name\n");
    }
    if (lldb_eval_value.GetTypeName() != nullptr) {
      printf("lldb-eval type: `%s`\n", lldb_eval_value.GetTypeName());
    } else {
      printf("lldb-eval type: No type name\n");
    }
  } else if (verbosity == Verbosity::ShowEverything) {
    printf("type: `%s`\n", lldb_value.GetTypeName());
  }

  if (has_error) {
    printf("== Reported errors ==\n");
    if (lldb_err.GetCString() != nullptr) {
      printf("lldb     : %s\n", lldb_err.GetCString());
    } else {
      printf("lldb     : No error reported\n");
    }

    if (lldb_eval_err.GetCString() != nullptr) {
      printf("lldb-eval: %s\n", lldb_eval_err.GetCString());
    } else {
      printf("lldb-eval: No error reported\n");
    }
  }

  printf("============================================================\n");
}

void run_repl(lldb::SBFrame& frame) {
  linenoise::SetMultiLine(true);
  std::string expr;
  for (;;) {
    auto quit = linenoise::Readline("> ", expr);
    if (quit) {
      break;
    }

    eval_and_print_expr(frame, expr, Verbosity::ShowEverything);
    linenoise::AddHistory(expr.c_str());
  }
}

fuzzer::SymbolTable gen_symtab() {
  fuzzer::SymbolTable symtab;

  {
    fuzzer::Type type(fuzzer::ScalarType::Char);

    symtab.add_var(type, fuzzer::VariableExpr("char_min"));
    symtab.add_var(type, fuzzer::VariableExpr("char_max"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::UnsignedChar);

    symtab.add_var(type, fuzzer::VariableExpr("uchar_min"));
    symtab.add_var(type, fuzzer::VariableExpr("uchar_max"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::SignedChar);

    symtab.add_var(type, fuzzer::VariableExpr("schar_min"));
    symtab.add_var(type, fuzzer::VariableExpr("schar_max"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::UnsignedShort);

    symtab.add_var(type, fuzzer::VariableExpr("ushort_min"));
    symtab.add_var(type, fuzzer::VariableExpr("ushort_max"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::SignedShort);

    symtab.add_var(type, fuzzer::VariableExpr("short_min"));
    symtab.add_var(type, fuzzer::VariableExpr("short_max"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::UnsignedInt);

    symtab.add_var(type, fuzzer::VariableExpr("uint_min"));
    symtab.add_var(type, fuzzer::VariableExpr("uint_max"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::SignedInt);

    symtab.add_var(type, fuzzer::VariableExpr("int_min"));
    symtab.add_var(type, fuzzer::VariableExpr("int_max"));
    symtab.add_var(type, fuzzer::VariableExpr("x"));

    // TODO: Change the type of ref when we add support for references
    symtab.add_var(type, fuzzer::VariableExpr("ref"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::UnsignedLong);

    symtab.add_var(type, fuzzer::VariableExpr("ulong_min"));
    symtab.add_var(type, fuzzer::VariableExpr("ulong_max"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::SignedLong);

    symtab.add_var(type, fuzzer::VariableExpr("long_min"));
    symtab.add_var(type, fuzzer::VariableExpr("long_max"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::UnsignedLongLong);

    symtab.add_var(type, fuzzer::VariableExpr("ullong_min"));
    symtab.add_var(type, fuzzer::VariableExpr("ullong_max"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::SignedLongLong);

    symtab.add_var(type, fuzzer::VariableExpr("llong_min"));
    symtab.add_var(type, fuzzer::VariableExpr("llong_max"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::Float);

    symtab.add_var(type, fuzzer::VariableExpr("fnan"));
    symtab.add_var(type, fuzzer::VariableExpr("finf"));
    symtab.add_var(type, fuzzer::VariableExpr("fsnan"));
    symtab.add_var(type, fuzzer::VariableExpr("fmax"));
    symtab.add_var(type, fuzzer::VariableExpr("fdenorm"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::Double);

    symtab.add_var(type, fuzzer::VariableExpr("dnan"));
    symtab.add_var(type, fuzzer::VariableExpr("dinf"));
    symtab.add_var(type, fuzzer::VariableExpr("dsnan"));
    symtab.add_var(type, fuzzer::VariableExpr("dmax"));
    symtab.add_var(type, fuzzer::VariableExpr("ddenorm"));
  }
  {
    fuzzer::Type type(fuzzer::ScalarType::LongDouble);

    symtab.add_var(type, fuzzer::VariableExpr("ldnan"));
    symtab.add_var(type, fuzzer::VariableExpr("ldinf"));
    symtab.add_var(type, fuzzer::VariableExpr("ldsnan"));
    symtab.add_var(type, fuzzer::VariableExpr("ldmax"));
    symtab.add_var(type, fuzzer::VariableExpr("lddenorm"));
  }
  {
    fuzzer::Type type(fuzzer::TaggedType("TestStruct"));

    symtab.add_var(type, fuzzer::VariableExpr("ts"));
  }
  {
    fuzzer::PointerType type{
        fuzzer::QualifiedType(fuzzer::ScalarType::SignedInt)};
    symtab.add_var(type, fuzzer::VariableExpr("p"));
  }
  {
    fuzzer::PointerType type(fuzzer::QualifiedType(fuzzer::ScalarType::Char,
                                                   fuzzer::CvQualifier::Const));
    symtab.add_var(type, fuzzer::VariableExpr("test_str"));
  }

  {
    fuzzer::TaggedType struct_type("TestStruct");
    symtab.add_field(struct_type, "int_field", fuzzer::ScalarType::SignedInt);
    symtab.add_field(struct_type, "flt_field", fuzzer::ScalarType::Float);
    symtab.add_field(struct_type, "ull_field",
                     fuzzer::ScalarType::UnsignedLongLong);
    symtab.add_field(struct_type, "ch_field", fuzzer::ScalarType::Char);
  }

  return symtab;
}

void run_fuzzer(lldb::SBFrame& frame, const unsigned* seed_ptr) {
  std::random_device rd;
  unsigned seed = seed_ptr ? *seed_ptr : rd();
  printf("==== Seed for this run is: %u ====\n", seed);

  auto rng = std::make_unique<fuzzer::DefaultGeneratorRng>(seed);
  // Fuzzer configuration. Refer to `tools/fuzzer/expr_gen.h` to see what
  // parameters are available.
  auto cfg = fuzzer::GenConfig();
  // Disable shift and division for now
  cfg.bin_op_mask[fuzzer::BinOp::Shl] = false;
  cfg.bin_op_mask[fuzzer::BinOp::Shr] = false;

  // Symbol table
  fuzzer::SymbolTable symtab = gen_symtab();

  fuzzer::ExprGenerator gen(std::move(rng), std::move(cfg), std::move(symtab));
  std::vector<std::string> exprs;

  for (int i = 0; i < cfg.num_exprs_to_generate; i++) {
    auto maybe_gen_expr = gen.generate();
    if (!maybe_gen_expr.has_value()) {
      fprintf(stderr, "Warning: Could not generate expression #:%d\n", i);
      continue;
    }
    const auto& gen_expr = maybe_gen_expr.value();

    std::ostringstream os;
    os << gen_expr;
    auto str = os.str();

    exprs.emplace_back(std::move(str));
  }

  for (const auto& e : exprs) {
    eval_and_print_expr(frame, e, Verbosity::ShowMismatchesOrErrors);
  }
}

int main(int argc, char** argv) {
  std::string err;
  std::unique_ptr<Runfiles> runfiles(Runfiles::Create(argv[0], &err));
  if (runfiles == nullptr) {
    fprintf(stderr, "Could not launch the fuzzer: %s\n", err.c_str());
    return 1;
  }

  bool repl_mode = false;
  bool custom_seed = false;
  bool print_help = false;

  unsigned seed = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--repl") == 0) {
      repl_mode = true;
    }
    if (strcmp(argv[i], "--help") == 0) {
      print_help = true;
      break;
    }
    if (strcmp(argv[i], "--seed") == 0 && i < argc - 1) {
      custom_seed = true;

      i++;
      seed = std::stoul(argv[i]);
    }
  }
  if (print_help) {
    printf("Usage: %s [--repl] [--help] [--seed <rng_seed>]\n", argv[0]);
    printf("--help: Print this message\n");
    printf("--repl: REPL mode, evaluate expressions on lldb and lldb-eval\n");
    printf("--seed <rng_seed>: Specify the RNG seed to use\n");

    return 0;
  }

  lldb_eval::SetupLLDBServerEnv(*runfiles);

  auto source_path = runfiles->Rlocation(SOURCE_PATH_KEY);
  auto binary_path = runfiles->Rlocation(BINARY_PATH_KEY);

  lldb::SBDebugger::Initialize();
  {
    auto debugger = lldb::SBDebugger::Create();
    auto proc = lldb_eval::LaunchTestProgram(debugger, source_path, binary_path,
                                             "// BREAK HERE");
    auto thread = proc.GetSelectedThread();
    auto frame = thread.GetSelectedFrame();

    if (repl_mode) {
      run_repl(frame);
    } else {
      const unsigned* seed_ptr = custom_seed ? &seed : nullptr;
      run_fuzzer(frame, seed_ptr);
    }

    proc.Destroy();
  }
  lldb::SBDebugger::Terminate();

  return 0;
}
