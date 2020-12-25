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

#include "lldb-eval/parser.h"

#include <stdlib.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/HeaderSearch.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/LiteralSupport.h"
#include "clang/Lex/ModuleLoader.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/Token.h"
#include "lldb-eval/ast.h"
#include "lldb-eval/defines.h"
#include "lldb-eval/value.h"
#include "lldb/API/SBType.h"
#include "lldb/API/SBValue.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Host.h"

namespace {

void StringReplace(std::string& str, const std::string& old_value,
                   const std::string& new_value) {
  size_t pos = str.find(old_value);
  if (pos != std::string::npos) {
    str.replace(pos, old_value.length(), new_value);
  }
}

template <typename T>
constexpr unsigned type_width() {
  return static_cast<unsigned>(sizeof(T)) * CHAR_BIT;
}

inline void TokenKindsJoinImpl(std::ostringstream& os,
                               clang::tok::TokenKind k) {
  os << "'" << clang::tok::getTokenName(k) << "'";
}

template <typename... Ts>
inline void TokenKindsJoinImpl(std::ostringstream& os, clang::tok::TokenKind k,
                               Ts... ks) {
  TokenKindsJoinImpl(os, k);
  os << ", ";
  TokenKindsJoinImpl(os, ks...);
}

template <typename... Ts>
inline std::string TokenKindsJoin(clang::tok::TokenKind k, Ts... ks) {
  std::ostringstream os;
  TokenKindsJoinImpl(os, k, ks...);

  return os.str();
}

std::string FormatDiagnostics(const clang::SourceManager& sm,
                              const std::string& message,
                              clang::SourceLocation loc) {
  // Get the source buffer and the location of the current token.
  llvm::StringRef text = sm.getBufferData(sm.getFileID(loc));
  size_t loc_offset = sm.getCharacterData(loc) - text.data();

  // Look for the start of the line.
  size_t line_start = text.rfind('\n', loc_offset);
  line_start = line_start == llvm::StringRef::npos ? 0 : line_start + 1;

  // Look for the end of the line.
  size_t line_end = text.find('\n', loc_offset);
  line_end = line_end == llvm::StringRef::npos ? text.size() : line_end;

  // Get a view of the current line in the source code and the position of the
  // diagnostics pointer.
  llvm::StringRef line = text.slice(line_start, line_end);
  int32_t arrow = sm.getPresumedColumnNumber(loc);

  // Calculate the padding in case we point outside of the expression (this can
  // happen if the parser expected something, but got EOF).
  size_t expr_rpad = std::max(0, arrow - static_cast<int32_t>(line.size()));
  size_t arrow_rpad = std::max(0, static_cast<int32_t>(line.size()) - arrow);

  return llvm::formatv("{0}: {1}\n{2}\n{3}", loc.printToString(sm), message,
                       llvm::fmt_pad(line, 0, expr_rpad),
                       llvm::fmt_pad("^", arrow - 1, arrow_rpad));
}

lldb::BasicType PickIntegerType(const clang::NumericLiteralParser& literal,
                                const llvm::APInt& value) {
  unsigned int_size = type_width<int>();
  unsigned long_size = type_width<long>();
  unsigned long_long_size = type_width<long long>();

  // Binary, Octal, Hexadecimal and literals with a U suffix are allowed to be
  // an unsigned integer.
  bool unsigned_is_allowed = literal.isUnsigned || literal.getRadix() != 10;

  // Try int/unsigned int.
  if (!literal.isLong && !literal.isLongLong && value.isIntN(int_size)) {
    if (!literal.isUnsigned && value.isIntN(int_size - 1)) {
      return lldb::eBasicTypeInt;
    }
    if (unsigned_is_allowed) {
      return lldb::eBasicTypeUnsignedInt;
    }
  }
  // Try long/unsigned long.
  if (!literal.isLongLong && value.isIntN(long_size)) {
    if (!literal.isUnsigned && value.isIntN(long_size - 1)) {
      return lldb::eBasicTypeLong;
    }
    if (unsigned_is_allowed) {
      return lldb::eBasicTypeUnsignedLong;
    }
  }
  // Try long long/unsigned long long.
  if (value.isIntN(long_long_size)) {
    if (!literal.isUnsigned && value.isIntN(long_long_size - 1)) {
      return lldb::eBasicTypeLongLong;
    }
    if (unsigned_is_allowed) {
      return lldb::eBasicTypeUnsignedLongLong;
    }
  }

  // If we still couldn't decide a type, we probably have something that does
  // not fit in a signed long long, but has no U suffix. Also known as:
  //
  //  warning: integer literal is too large to be represented in a signed
  //  integer type, interpreting as unsigned [-Wimplicitly-unsigned-literal]
  //
  return lldb::eBasicTypeUnsignedLongLong;
}

}  // namespace

namespace lldb_eval {

std::string TypeDeclaration::GetName() const {
  // Full name is a combination of a base name and pointer operators.
  std::string name = GetBaseName();

  // In LLDB pointer operators are separated with a single whitespace.
  if (ptr_operators_.size() > 0) {
    name.append(" ");
  }
  for (auto tok : ptr_operators_) {
    if (tok == clang::tok::star) {
      name.append("*");
    } else if (tok == clang::tok::amp) {
      name.append("&");
    }
  }
  return name;
}

std::string TypeDeclaration::GetBaseName() const {
  // TODO(werat): Implement more robust textual type representation.
  std::string base_name = llvm::formatv(
      "{0:$[ ]}", llvm::make_range(typenames_.begin(), typenames_.end()));

  // TODO(werat): Handle these type aliases and detect invalid type combinations
  // (e.g. "long char") during the TypeDeclaration construction.
  StringReplace(base_name, "short int", "short");
  StringReplace(base_name, "long int", "long");

  return base_name;
}

Parser::Parser(std::shared_ptr<Context> ctx) : ctx_(std::move(ctx)) {
  target_ = ctx_->GetExecutionContext().GetTarget();

  clang::SourceManager& sm = ctx_->GetSourceManager();
  clang::DiagnosticsEngine& de = sm.getDiagnostics();

  auto tOpts = std::make_shared<clang::TargetOptions>();
  tOpts->Triple = llvm::sys::getDefaultTargetTriple();

  ti_.reset(clang::TargetInfo::CreateTargetInfo(de, tOpts));

  lang_opts_ = std::make_unique<clang::LangOptions>();
  lang_opts_->Bool = true;
  lang_opts_->WChar = true;
  lang_opts_->CPlusPlus = true;
  lang_opts_->CPlusPlus11 = true;
  lang_opts_->CPlusPlus14 = true;
  lang_opts_->CPlusPlus17 = true;

  tml_ = std::make_unique<clang::TrivialModuleLoader>();

  auto hOpts = std::make_shared<clang::HeaderSearchOptions>();
  hs_ = std::make_unique<clang::HeaderSearch>(hOpts, sm, de, *lang_opts_,
                                              ti_.get());

  auto pOpts = std::make_shared<clang::PreprocessorOptions>();
  pp_ = std::make_unique<clang::Preprocessor>(pOpts, de, *lang_opts_, sm, *hs_,
                                              *tml_);
  pp_->Initialize(*ti_);
  pp_->EnterMainSourceFile();

  // Initialize the token.
  token_.setKind(clang::tok::unknown);
}

ExprResult Parser::Run(Error& error) {
  ConsumeToken();
  auto expr = ParseExpression();
  Expect(clang::tok::eof);

  error = error_;
  error_.Clear();

  // Explicitly return ErrorNode if there was an error during the parsing. Some
  // routines raise an error, but don't change the return value (e.g. Expect).
  if (error) {
    return std::make_unique<ErrorNode>();
  }
  return expr;
}

std::string Parser::TokenDescription(const clang::Token& token) {
  const auto& spelling = pp_->getSpelling(token);
  const auto* kind_name = token.getName();
  return llvm::formatv("<'{0}' ({1})>", spelling, kind_name);
}

void Parser::Expect(clang::tok::TokenKind kind) {
  if (token_.isNot(kind)) {
    BailOut(ErrorCode::kUnknown,
            llvm::formatv("expected {0}, got: {1}", TokenKindsJoin(kind),
                          TokenDescription(token_)),
            token_.getLocation());
  }
}

template <typename... Ts>
void Parser::ExpectOneOf(clang::tok::TokenKind k, Ts... ks) {
  static_assert((std::is_same_v<Ts, clang::tok::TokenKind> && ...),
                "ExpectOneOf can be only called with values of type "
                "clang::tok::TokenKind");

  if (!token_.isOneOf(k, ks...)) {
    BailOut(ErrorCode::kUnknown,
            llvm::formatv("expected any of ({0}), got: {1}",
                          TokenKindsJoin(k, ks...), TokenDescription(token_)),
            token_.getLocation());
  }
}

void Parser::ConsumeToken() {
  if (token_.is(clang::tok::eof)) {
    // Don't do anything if we're already at eof. This can happen if an error
    // occurred during parsing and we're trying to bail out.
    return;
  }
  pp_->Lex(token_);
}

void Parser::BailOut(ErrorCode code, const std::string& error,
                     clang::SourceLocation loc) {
  if (error_) {
    // If error is already set, then the parser is in the "bail-out" mode. Don't
    // do anything and keep the original error.
    return;
  }

  error_.Set(code, FormatDiagnostics(ctx_->GetSourceManager(), error, loc));
  token_.setKind(clang::tok::eof);
}

// Parse an expression.
//
//  expression:
//    assignment_expression
//
ExprResult Parser::ParseExpression() { return ParseAssignmentExpression(); }

// Parse an assigment_expression.
//
//  assignment_expression:
//    conditional_expression
//
ExprResult Parser::ParseAssignmentExpression() {
  return ParseConditionalExpression();
}

// Parse a conditional_expression
//
//  conditional_expression:
//    logical_or_expression
//    logical_or_expression "?" expression ":" assignment_expression
//
ExprResult Parser::ParseConditionalExpression() {
  auto lhs = ParseLogicalOrExpression();

  if (token_.is(clang::tok::question)) {
    ConsumeToken();
    auto true_val = ParseExpression();
    Expect(clang::tok::colon);
    ConsumeToken();
    auto false_val = ParseAssignmentExpression();
    lhs = std::make_unique<TernaryOpNode>(std::move(lhs), std::move(true_val),
                                          std::move(false_val));
  }

  return lhs;
}

// Parse a logical_or_expression.
//
//  logical_or_expression:
//    logical_and_expression {"||" logical_and_expression}
//
ExprResult Parser::ParseLogicalOrExpression() {
  auto lhs = ParseLogicalAndExpression();

  while (token_.is(clang::tok::pipepipe)) {
    clang::tok::TokenKind kind = token_.getKind();
    ConsumeToken();
    auto rhs = ParseLogicalAndExpression();
    lhs = std::make_unique<BinaryOpNode>(kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

// Parse a logical_and_expression.
//
//  logical_and_expression:
//    inclusive_or_expression {"&&" inclusive_or_expression}
//
ExprResult Parser::ParseLogicalAndExpression() {
  auto lhs = ParseInclusiveOrExpression();

  while (token_.is(clang::tok::ampamp)) {
    clang::tok::TokenKind kind = token_.getKind();
    ConsumeToken();
    auto rhs = ParseInclusiveOrExpression();
    lhs = std::make_unique<BinaryOpNode>(kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

// Parse an inclusive_or_expression.
//
//  inclusive_or_expression:
//    exclusive_or_expression {"|" exclusive_or_expression}
//
ExprResult Parser::ParseInclusiveOrExpression() {
  auto lhs = ParseExclusiveOrExpression();

  while (token_.is(clang::tok::pipe)) {
    clang::tok::TokenKind kind = token_.getKind();
    ConsumeToken();
    auto rhs = ParseExclusiveOrExpression();
    lhs = std::make_unique<BinaryOpNode>(kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

// Parse an exclusive_or_expression.
//
//  exclusive_or_expression:
//    and_expression {"^" and_expression}
//
ExprResult Parser::ParseExclusiveOrExpression() {
  auto lhs = ParseAndExpression();

  while (token_.is(clang::tok::caret)) {
    clang::tok::TokenKind kind = token_.getKind();
    ConsumeToken();
    auto rhs = ParseAndExpression();
    lhs = std::make_unique<BinaryOpNode>(kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

// Parse an and_expression.
//
//  and_expression:
//    equality_expression {"&" equality_expression}
//
ExprResult Parser::ParseAndExpression() {
  auto lhs = ParseEqualityExpression();

  while (token_.is(clang::tok::amp)) {
    clang::tok::TokenKind kind = token_.getKind();
    ConsumeToken();
    auto rhs = ParseEqualityExpression();
    lhs = std::make_unique<BinaryOpNode>(kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

// Parse an equality_expression.
//
//  equality_expression:
//    relational_expression {"==" relational_expression}
//    relational_expression {"!=" relational_expression}
//
ExprResult Parser::ParseEqualityExpression() {
  auto lhs = ParseRelationalExpression();

  while (token_.isOneOf(clang::tok::equalequal, clang::tok::exclaimequal)) {
    clang::tok::TokenKind kind = token_.getKind();
    ConsumeToken();
    auto rhs = ParseRelationalExpression();
    lhs = std::make_unique<BinaryOpNode>(kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

// Parse a relational_expression.
//
//  relational_expression:
//    shift_expression {"<" shift_expression}
//    shift_expression {">" shift_expression}
//    shift_expression {"<=" shift_expression}
//    shift_expression {">=" shift_expression}
//
ExprResult Parser::ParseRelationalExpression() {
  auto lhs = ParseShiftExpression();

  while (token_.isOneOf(clang::tok::less, clang::tok::greater,
                        clang::tok::lessequal, clang::tok::greaterequal)) {
    clang::tok::TokenKind kind = token_.getKind();
    ConsumeToken();
    auto rhs = ParseShiftExpression();
    lhs = std::make_unique<BinaryOpNode>(kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

// Parse a shift_expression.
//
//  shift_expression:
//    additive_expression {"<<" additive_expression}
//    additive_expression {">>" additive_expression}
//
ExprResult Parser::ParseShiftExpression() {
  auto lhs = ParseAdditiveExpression();

  while (token_.isOneOf(clang::tok::lessless, clang::tok::greatergreater)) {
    clang::tok::TokenKind kind = token_.getKind();
    ConsumeToken();
    auto rhs = ParseAdditiveExpression();
    lhs = std::make_unique<BinaryOpNode>(kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

// Parse an additive_expression.
//
//  additive_expression:
//    multiplicative_expression {"+" multiplicative_expression}
//    multiplicative_expression {"-" multiplicative_expression}
//
ExprResult Parser::ParseAdditiveExpression() {
  auto lhs = ParseMultiplicativeExpression();

  while (token_.isOneOf(clang::tok::plus, clang::tok::minus)) {
    clang::tok::TokenKind kind = token_.getKind();
    ConsumeToken();
    auto rhs = ParseMultiplicativeExpression();
    lhs = std::make_unique<BinaryOpNode>(kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

// Parse a multiplicative_expression.
//
//  multiplicative_expression:
//    cast_expression {"*" cast_expression}
//    cast_expression {"/" cast_expression}
//    cast_expression {"%" cast_expression}
//
ExprResult Parser::ParseMultiplicativeExpression() {
  auto lhs = ParseCastExpression();

  while (token_.isOneOf(clang::tok::star, clang::tok::slash,
                        clang::tok::percent)) {
    clang::tok::TokenKind kind = token_.getKind();
    ConsumeToken();
    auto rhs = ParseCastExpression();
    lhs = std::make_unique<BinaryOpNode>(kind, std::move(lhs), std::move(rhs));
  }

  return lhs;
}

// Parse a cast_expression.
//
//  cast_expression:
//    unary_expression
//    "(" type_id ")" cast_expression
//
ExprResult Parser::ParseCastExpression() {
  // This can be a C-style cast, try parsing the contents as a type declaration.
  if (token_.is(clang::tok::l_paren)) {
    // Enable lexer backtracking, so that we can rollback in case it's not
    // actually a type declaration.
    TentativeParsingAction tentative_parsing(this);

    // Consume the token only after enabling the backtracking.
    ConsumeToken();

    // Try parsing the type declaration. If the returned value is not valid,
    // then we should rollback and try parsing the expression.
    TypeDeclaration type_decl = ParseTypeId();

    // Try resolving base type of the type declaration.
    // TODO(werat): Resolve the type and the declarators during parsing to save
    // time and produce more accurate diagnostics.
    lldb::SBType type = ResolveTypeFromTypeDecl(type_decl);

    if (type) {
      // Successfully parsed the type declaration. Commit the backtracked
      // tokens and parse the cast_expression.
      tentative_parsing.Commit();

      // Apply type declarators (i.e. pointer/reference qualifiers).
      type = ResolveTypeDeclarators(type, type_decl);
      if (!type) {
        return std::make_unique<ErrorNode>();
      }

      Expect(clang::tok::r_paren);
      ConsumeToken();
      auto rhs = ParseCastExpression();

      return std::make_unique<CStyleCastNode>(type, std::move(rhs));

    } else {
      // Failed to parse the contents of the parentheses as a type declaration.
      // Rollback the lexer and try parsing it as unary_expression.
      tentative_parsing.Rollback();
    }
  }

  return ParseUnaryExpression();
}

// Parse an unary_expression.
//
//  unary_expression:
//    postfix_expression
//    "++" cast_expression
//    "--" cast_expression
//    unary_operator cast_expression
//
//  unary_operator:
//    "&"
//    "*"
//    "+"
//    "-"
//    "~"
//    "!"
//
ExprResult Parser::ParseUnaryExpression() {
  if (token_.isOneOf(clang::tok::plusplus, clang::tok::minusminus,
                     clang::tok::star, clang::tok::amp, clang::tok::plus,
                     clang::tok::minus, clang::tok::exclaim,
                     clang::tok::tilde)) {
    clang::tok::TokenKind kind = token_.getKind();
    ConsumeToken();
    auto rhs = ParseCastExpression();
    return std::make_unique<UnaryOpNode>(kind, std::move(rhs));
  }

  return ParsePostfixExpression();
}

// Parse a postfix_expression.
//
//  postfix_expression:
//    primary_expression {"[" expression "]"}
//    primary_expression {"." id_expression}
//    primary_expression {"->" id_expression}
//    primary_expression {"++"}
//    primary_expression {"--"}
//
ExprResult Parser::ParsePostfixExpression() {
  auto lhs = ParsePrimaryExpression();

  while (token_.isOneOf(clang::tok::l_square, clang::tok::period,
                        clang::tok::arrow, clang::tok::plusplus,
                        clang::tok::minusminus)) {
    switch (token_.getKind()) {
      case clang::tok::period:
      case clang::tok::arrow: {
        auto type = token_.getKind() == clang::tok::period
                        ? MemberOfNode::Type::OF_OBJECT
                        : MemberOfNode::Type::OF_POINTER;
        ConsumeToken();
        auto member_id = ParseIdExpression();
        lhs = std::make_unique<MemberOfNode>(type, std::move(lhs),
                                             std::move(member_id));
        break;
      }
      case clang::tok::plusplus:
      case clang::tok::minusminus: {
        BailOut(ErrorCode::kNotImplemented,
                llvm::formatv("We don't support postfix inc/dec yet: {0}",
                              TokenDescription(token_)),
                token_.getLocation());
        return std::make_unique<ErrorNode>();
      }
      case clang::tok::l_square: {
        ConsumeToken();
        auto rhs = ParseExpression();
        Expect(clang::tok::r_square);
        ConsumeToken();
        lhs = std::make_unique<BinaryOpNode>(clang::tok::l_square,
                                             std::move(lhs), std::move(rhs));
        break;
      }

      default:
        lldb_eval_unreachable("Invalid token");
    }
  }

  return lhs;
}

// Parse a primary_expression.
//
//  primary_expression:
//    numeric_literal
//    boolean_literal
//    pointer_literal
//    id_expression
//    "this"
//    "(" expression ")"
//
ExprResult Parser::ParsePrimaryExpression() {
  if (token_.is(clang::tok::numeric_constant)) {
    return ParseNumericLiteral();
  } else if (token_.isOneOf(clang::tok::kw_true, clang::tok::kw_false)) {
    return ParseBooleanLiteral();
  } else if (token_.is(clang::tok::kw_nullptr)) {
    return ParsePointerLiteral();
  } else if (token_.isOneOf(clang::tok::coloncolon, clang::tok::identifier)) {
    // Save the source location for the diagnostics message.
    clang::SourceLocation loc = token_.getLocation();
    auto identifier = ParseIdExpression();
    auto value = ctx_->LookupIdentifier(identifier);
    if (!value) {
      BailOut(ErrorCode::kUndeclaredIdentifier,
              llvm::formatv("use of undeclared identifier '{0}'", identifier),
              loc);
      return std::make_unique<ErrorNode>();
    }
    return std::make_unique<IdentifierNode>(identifier,
                                            Value(value, /*is_rvalue*/ false));
  } else if (token_.is(clang::tok::kw_this)) {
    // Save the source location for the diagnostics message.
    clang::SourceLocation loc = token_.getLocation();
    ConsumeToken();
    auto value = ctx_->LookupIdentifier("this");
    if (!value) {
      BailOut(ErrorCode::kUndeclaredIdentifier,
              "invalid use of 'this' outside of a non-static member function",
              loc);
      return std::make_unique<ErrorNode>();
    }
    // Special case for "this" pointer. As per C++ standard, it's a prvalue.
    return std::make_unique<IdentifierNode>("this",
                                            Value(value, /*is_rvalue*/ true));
  } else if (token_.is(clang::tok::l_paren)) {
    ConsumeToken();
    auto expr = ParseExpression();
    Expect(clang::tok::r_paren);
    ConsumeToken();
    return expr;
  }

  BailOut(ErrorCode::kInvalidExpressionSyntax,
          llvm::formatv("Unexpected token: {0}", TokenDescription(token_)),
          token_.getLocation());
  return std::make_unique<ErrorNode>();
}

// Parse a type_id.
//
//  type_id:
//    type_specifier_seq {abstract_declarator}
//
TypeDeclaration Parser::ParseTypeId() {
  TypeDeclaration type_decl;

  // type_specifier_seq is required here, start with trying to parse it.
  ParseTypeSpecifierSeq(&type_decl);

  //
  //  abstract_declarator:
  //    ptr_operator {abstract_declarator}
  //
  while (IsPtrOperator(token_)) {
    ParsePtrOperator(&type_decl);
  }

  return type_decl;
}

// Parse a type_specifier_seq.
//
//  type_specifier_seq:
//    type_specifier {type_specifier_seq}
//
void Parser::ParseTypeSpecifierSeq(TypeDeclaration* type_decl) {
  while (true) {
    // TODO(b/161677840): Check if produced type specifiers can be combined
    // together. For example, "long long" is legal, but "char char" is not.
    bool type_specifier = ParseTypeSpecifier(type_decl);
    if (!type_specifier) {
      break;
    }
  }
}

// Parse a type_specifier.
//
//  type_specifier:
//    simple_type_specifier
//    cv_qualifier
//
//  simple_type_specifier:
//    {"::"} {nested_name_specifier} type_name
//    "char"
//    "char16_t"
//    "char32_t"
//    "wchar_t"
//    "bool"
//    "short"
//    "int"
//    "long"
//    "signed"
//    "unsigned"
//    "float"
//    "double"
//    "void"
//
// Returns TRUE if a type_specifier was successfully parsed at this location.
//
bool Parser::ParseTypeSpecifier(TypeDeclaration* type_decl) {
  if (IsCvQualifier(token_)) {
    // Just ignore CV quialifiers, we don't use them in type casting.
    ConsumeToken();
    return true;
  }

  if (IsSimpleTypeSpecifierKeyword(token_)) {
    type_decl->typenames_.push_back(pp_->getSpelling(token_));
    ConsumeToken();
    return true;
  }

  // The type_specifier must be a user-defined type. Try parsing a
  // simple_type_specifier.
  {
    // Try parsing optional global scope operator.
    bool global_scope = false;
    if (token_.is(clang::tok::coloncolon)) {
      global_scope = true;
      ConsumeToken();
    }

    // Try parsing optional nested_name_specifier.
    auto nested_name_specifier = ParseNestedNameSpecifier();

    // Try parsing required type_name.
    auto type_name = ParseTypeName();

    // If there is a type_name, then this is indeed a simple_type_specifier.
    // Global and qualified (namespace/class) scopes can be empty, since they're
    // optional. In this case type_name is type we're looking for.
    if (!type_name.empty()) {
      // Construct the fully qualified typename.
      auto type_specifier = llvm::formatv("{0}{1}{2}", global_scope ? "::" : "",
                                          nested_name_specifier, type_name);

      type_decl->typenames_.push_back(type_specifier);
      return true;
    }
  }

  // No type_specifier was found here.
  return false;
}

// Parse nested_name_specifier.
//
//  nested_name_specifier:
//    type_name "::"
//    namespace_name '::'
//    nested_name_specifier identifier "::"
//    nested_name_specifier simple_template_id "::"
//
std::string Parser::ParseNestedNameSpecifier() {
  // The first token in nested_name_specifier is always an identifier.
  if (token_.isNot(clang::tok::identifier)) {
    return "";
  }

  // If the next token is scope ("::"), then this is indeed a
  // nested_name_specifier
  if (pp_->LookAhead(0).is(clang::tok::coloncolon)) {
    // This nested_name_specifier is a single identifier.
    std::string identifier = pp_->getSpelling(token_);
    ConsumeToken();
    Expect(clang::tok::coloncolon);
    ConsumeToken();
    // Continue parsing the nested_name_specifier.
    return identifier + "::" + ParseNestedNameSpecifier();
  }

  // If the next token starts a template argument list, then we have a
  // simple_template_id here.
  if (pp_->LookAhead(0).is(clang::tok::less)) {
    // We don't know whether this will be a nested_name_identifier or just a
    // type_name. Prepare to rollback if this is not a nested_name_identifier.
    TentativeParsingAction tentative_parsing(this);

    // TODO(werat): Parse just the simple_template_id?
    auto type_name = ParseTypeName();

    // If we did parse the type_name successfully and it's followed by the scope
    // operator ("::"), then this is indeed a nested_name_specifier. Commit the
    // tentative parsing and continue parsing nested_name_specifier.
    if (!type_name.empty() && token_.is(clang::tok::coloncolon)) {
      tentative_parsing.Commit();
      ConsumeToken();
      // Continue parsing the nested_name_specifier.
      return type_name + "::" + ParseNestedNameSpecifier();
    }

    // Not a nested_name_specifier, but could be just a type_name or something
    // else entirely. Rollback the parser and try a different path.
    tentative_parsing.Rollback();
  }

  return "";
}

// Parse a type_name.
//
//  type_name:
//    class_name
//    enum_name
//    typedef_name
//    simple_template_id
//
//  class_name
//    identifier
//
//  enum_name
//    identifier
//
//  typedef_name
//    identifier
//
std::string Parser::ParseTypeName() {
  // Typename always starts with an identifier.
  if (token_.isNot(clang::tok::identifier)) {
    return "";
  }

  // If the next token starts a template argument list, parse this type_name as
  // a simple_template_id.
  if (pp_->LookAhead(0).is(clang::tok::less)) {
    // Parse the template_name. In this case it's just an identifier.
    std::string template_name = pp_->getSpelling(token_);
    ConsumeToken();
    // Consume the "<" token.
    ConsumeToken();

    // Short-circuit for missing template_argument_list.
    if (token_.is(clang::tok::greater)) {
      ConsumeToken();
      return llvm::formatv("{0}<>", template_name);
    }

    // Try parsing template_argument_list.
    auto template_argument_list = ParseTemplateArgumentList();

    // TODO(werat): Handle ">>" situations.
    if (token_.is(clang::tok::greater)) {
      ConsumeToken();
      return llvm::formatv("{0}<{1}>", template_name, template_argument_list);
    }

    // Failed to parse a simple_template_id.
    return "";
  }

  // Otherwise look for a class_name, enum_name or a typedef_name.
  std::string identifier = pp_->getSpelling(token_);
  ConsumeToken();

  return identifier;
}

// Parse a template_argument_list.
//
//  template_argument_list:
//    template_argument
//    template_argument_list "," template_argument
//
std::string Parser::ParseTemplateArgumentList() {
  // Parse template arguments one by one.
  std::vector<std::string> arguments;

  do {
    // Eat the comma if this is not the first iteration.
    if (arguments.size() > 0) {
      ConsumeToken();
    }

    // Try parsing a template_argument. If this fails, then this is actually not
    // a template_argument_list.
    auto argument = ParseTemplateArgument();
    if (argument.empty()) {
      return "";
    }

    arguments.push_back(argument);

  } while (token_.is(clang::tok::comma));

  // Internally in LLDB/Clang nested template type names have extra spaces to
  // avoid having ">>". Add the extra space before the closing ">" if the
  // template argument is also a template.
  if (arguments.back().back() == '>') {
    arguments.back().push_back(' ');
  }

  return llvm::formatv("{0:$[, ]}",
                       llvm::make_range(arguments.begin(), arguments.end()));
}

// Parse a template_argument.
//
//  template_argument:
//    type_id
//    id_expression
//
std::string Parser::ParseTemplateArgument() {
  // There is no way to know at this point whether there is going to be a
  // type_id or something else. Try different options one by one.

  {
    // [temp.arg](http://eel.is/c++draft/temp.arg#2)
    //
    // In a template-argument, an ambiguity between a type-id and an expression
    // is resolved to a type-id, regardless of the form of the corresponding
    // template-parameter.

    // Therefore, first try parsing type_id.
    TentativeParsingAction tentative_parsing(this);

    TypeDeclaration type_decl = ParseTypeId();

    if (type_decl.IsValid() && ResolveTypeFromTypeDecl(type_decl)) {
      // Successfully parsed a type_id, check if the next token can finish the
      // template_argument. If so, commit the parsed tokens and return parsed
      // template_argument.
      if (token_.isOneOf(clang::tok::comma, clang::tok::greater)) {
        tentative_parsing.Commit();
        return type_decl.GetName();
      }
    }
    // Failed to parse a type_id. Rollback the parser and try something else.
    tentative_parsing.Rollback();
  }

  {
    // The next candidate is an id_expression. This can fail too, so prepare to
    // rollback again.
    TentativeParsingAction tentative_parsing(this);

    // Parse an id_expression.
    auto id_expression = ParseIdExpression();

    // If we've parsed the id_expression successfully and the next token can
    // finish the template_argument, then we're done here.
    if (!id_expression.empty() &&
        token_.isOneOf(clang::tok::comma, clang::tok::greater)) {
      tentative_parsing.Commit();
      return id_expression;
    }
    // Failed to parse a id_expression.
    tentative_parsing.Rollback();
  }

  // TODO(b/164399865): Another valid option here is a constant_expression. We
  // definitely don't want to support constant arithmetic like "Foo<1+2>", but
  // simple constants should be covered.
  // We can probably use ParsePrimaryExpression here, but need to figure out the
  // "stringification", since ParsePrimaryExpression returns ExprResult (and
  // potentially a whole expression, not just a single constant.)

  // This is not a template_argument.
  return "";
}

// Parse a ptr_operator.
//
//  ptr_operator:
//    "*" {cv_qualifier_seq}
//    "&"
//
void Parser::ParsePtrOperator(TypeDeclaration* type_decl) {
  ExpectOneOf(clang::tok::star, clang::tok::amp);

  if (token_.is(clang::tok::star)) {
    type_decl->ptr_operators_.push_back(clang::tok::star);
    ConsumeToken();

    //
    //  cv_qualifier_seq:
    //    cv_qualifier {cv_qualifier_seq}
    //
    //  cv_qualifier:
    //    "const"
    //    "volatile"
    //
    while (IsCvQualifier(token_)) {
      // Just ignore CV quialifiers, we don't use them in type casting.
      ConsumeToken();
    }

  } else if (token_.is(clang::tok::amp)) {
    type_decl->ptr_operators_.push_back(clang::tok::amp);
    ConsumeToken();
  }
}

lldb::SBType Parser::ResolveTypeFromTypeDecl(const TypeDeclaration& type_decl) {
  if (!type_decl.IsValid()) {
    return lldb::SBType();
  }

  // Resolve the type in the current expression context.
  return ctx_->ResolveTypeByName(type_decl.GetBaseName());
}

lldb::SBType Parser::ResolveTypeDeclarators(lldb::SBType type,
                                            const TypeDeclaration& type_decl) {
  // Resolve pointers/references.
  for (clang::tok::TokenKind tk : type_decl.ptr_operators_) {
    if (tk == clang::tok::star) {
      // Pointers to reference types are forbidden.
      if (type.IsReferenceType()) {
        std::string msg = llvm::formatv(
            "'type name' declared as a pointer to a reference of type '{0}'",
            type.GetName());
        BailOut(ErrorCode::kInvalidOperandType, msg, token_.getLocation());
        return lldb::SBType();
      }
      // Get pointer type for the base type: e.g. int* -> int**.
      type = type.GetPointerType();

    } else if (tk == clang::tok::amp) {
      // References to references are forbidden.
      if (type.IsReferenceType()) {
        BailOut(ErrorCode::kInvalidOperandType,
                "type name declared as a reference to a reference",
                token_.getLocation());
        return lldb::SBType();
      }
      // Get reference type for the base type: e.g. int -> int&.
      type = type.GetReferenceType();
    }
  }

  return type;
}

bool Parser::IsSimpleTypeSpecifierKeyword(clang::Token token) const {
  return token.isOneOf(
      clang::tok::kw_char, clang::tok::kw_char16_t, clang::tok::kw_char32_t,
      clang::tok::kw_wchar_t, clang::tok::kw_bool, clang::tok::kw_short,
      clang::tok::kw_int, clang::tok::kw_long, clang::tok::kw_signed,
      clang::tok::kw_unsigned, clang::tok::kw_float, clang::tok::kw_double,
      clang::tok::kw_void);
}

bool Parser::IsCvQualifier(clang::Token token) const {
  return token.isOneOf(clang::tok::kw_const, clang::tok::kw_volatile);
}

bool Parser::IsPtrOperator(clang::Token token) const {
  return token.isOneOf(clang::tok::star, clang::tok::amp);
}

// Parse an id_expression.
//
//  id_expression:
//    unqualified_id
//    qualified_id
//
//  qualified_id:
//    {"::"} {nested_name_specifier} unqualified_id
//    {"::"} identifier
//
//  identifier:
//    ? clang::tok::identifier ?
//
std::string Parser::ParseIdExpression() {
  // Try parsing optional global scope operator.
  bool global_scope = false;
  if (token_.is(clang::tok::coloncolon)) {
    global_scope = true;
    ConsumeToken();
  }

  // Try parsing optional nested_name_specifier.
  auto nested_name_specifier = ParseNestedNameSpecifier();

  // If nested_name_specifier is present, then it's qualified_id production.
  // Follow the first production rule.
  if (!nested_name_specifier.empty()) {
    // Parse unqualified_id and construct a fully qualified id expression.
    auto unqualified_id = ParseUnqualifiedId();

    return llvm::formatv("{0}{1}{2}", global_scope ? "::" : "",
                         nested_name_specifier, unqualified_id);
  }

  // No nested_name_specifier, but with global scope -- this is also a
  // qualified_id production. Follow the second production rule.
  else if (global_scope) {
    Expect(clang::tok::identifier);
    std::string identifier = pp_->getSpelling(token_);
    ConsumeToken();
    return llvm::formatv("{0}{1}", global_scope ? "::" : "", identifier);
  }

  // This is unqualified_id production.
  return ParseUnqualifiedId();
}

// Parse an unqualified_id.
//
//  unqualified_id:
//    identifier
//
//  identifier:
//    ? clang::tok::identifier ?
//
std::string Parser::ParseUnqualifiedId() {
  Expect(clang::tok::identifier);
  std::string identifier = pp_->getSpelling(token_);
  ConsumeToken();
  return identifier;
}

// Parse a numeric_literal.
//
//  numeric_literal:
//    ? clang::tok::numeric_constant ?
//
ExprResult Parser::ParseNumericLiteral() {
  Expect(clang::tok::numeric_constant);
  ExprResult numeric_constant = ParseNumericConstant(token_);
  ConsumeToken();
  return numeric_constant;
}

// Parse an boolean_literal.
//
//  boolean_literal:
//    "true"
//    "false"
//
ExprResult Parser::ParseBooleanLiteral() {
  ExpectOneOf(clang::tok::kw_true, clang::tok::kw_false);
  bool literal_value = token_.is(clang::tok::kw_true);
  ConsumeToken();
  return std::make_unique<LiteralNode>(
      CreateValueFromBool(target_, literal_value));
}

// Parse an pointer_literal.
//
//  pointer_literal:
//    "nullptr"
//
ExprResult Parser::ParsePointerLiteral() {
  Expect(clang::tok::kw_nullptr);
  ConsumeToken();
  return std::make_unique<LiteralNode>(CreateValueNullptr(target_));
}

ExprResult Parser::ParseNumericConstant(clang::Token token) {
  // Parse numeric constant, it can be either integer or float.
  std::string tok_spelling = pp_->getSpelling(token);

#if LLVM_VERSION_MAJOR >= 11
  clang::NumericLiteralParser literal(
      tok_spelling, token.getLocation(), pp_->getSourceManager(),
      pp_->getLangOpts(), pp_->getTargetInfo(), pp_->getDiagnostics());
#else
  clang::NumericLiteralParser literal(tok_spelling, token.getLocation(), *pp_);
#endif

  if (literal.hadError) {
    BailOut(
        ErrorCode::kInvalidNumericLiteral,
        "Failed to parse token as numeric-constant: " + TokenDescription(token),
        token.getLocation());
    return std::make_unique<ErrorNode>();
  }

  // Check for floating-literal and integer-literal. Fail on anything else (i.e.
  // fixed-point literal, who needs them anyway??).
  if (literal.isFloatingLiteral()) {
    return ParseFloatingLiteral(literal, token);
  }
  if (literal.isIntegerLiteral()) {
    return ParseIntegerLiteral(literal, token);
  }

  // Don't care about anything else.
  BailOut(ErrorCode::kInvalidNumericLiteral,
          "numeric-constant should be either float or integer literal: " +
              TokenDescription(token),
          token.getLocation());
  return std::make_unique<ErrorNode>();
}

ExprResult Parser::ParseFloatingLiteral(clang::NumericLiteralParser& literal,
                                        clang::Token token) {
  const llvm::fltSemantics& format = literal.isFloat
                                         ? llvm::APFloat::IEEEsingle()
                                         : llvm::APFloat::IEEEdouble();
  llvm::APFloat raw_value(format);
  llvm::APFloat::opStatus result = literal.GetFloatValue(raw_value);

  // Overflow is always an error, but underflow is only an error if we
  // underflowed to zero (APFloat reports denormals as underflow).
  if ((result & llvm::APFloat::opOverflow) ||
      ((result & llvm::APFloat::opUnderflow) && raw_value.isZero())) {
    BailOut(ErrorCode::kInvalidNumericLiteral,
            llvm::formatv("float underflow/overflow happened: {0}",
                          TokenDescription(token)),
            token.getLocation());
    return std::make_unique<ErrorNode>();
  }

  lldb::BasicType type =
      literal.isFloat ? lldb::eBasicTypeFloat : lldb::eBasicTypeDouble;

  Value value =
      CreateValueFromAPFloat(target_, raw_value, target_.GetBasicType(type));

  return std::make_unique<LiteralNode>(std::move(value));
}

ExprResult Parser::ParseIntegerLiteral(clang::NumericLiteralParser& literal,
                                       clang::Token token) {
  // Create a value big enough to fit all valid numbers.
  llvm::APInt raw_value(type_width<uintmax_t>(), 0);

  if (literal.GetIntegerValue(raw_value)) {
    BailOut(ErrorCode::kInvalidNumericLiteral,
            llvm::formatv("integer literal is too large to be represented in "
                          "any integer type: {0}",
                          TokenDescription(token)),
            token.getLocation());
    return std::make_unique<ErrorNode>();
  }

  lldb::BasicType type = PickIntegerType(literal, raw_value);

  // TODO(werat): fix this hack.
  bool is_unsigned = (type == lldb::eBasicTypeUnsignedInt ||
                      type == lldb::eBasicTypeUnsignedLong ||
                      type == lldb::eBasicTypeUnsignedLongLong);

  Value value =
      CreateValueFromAPInt(target_, llvm::APSInt(raw_value, is_unsigned),
                           target_.GetBasicType(type));

  return std::make_unique<LiteralNode>(std::move(value));
}

}  // namespace lldb_eval
