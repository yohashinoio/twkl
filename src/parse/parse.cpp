/**
 * parse.cpp
 *
 * These codes are licensed under Apache-2.0 License.
 * See the LICENSE for details.
 *
 * Copyright (c) 2022 Hiramoto Ittou.
 */

#include <pch/pch.hpp>
#include <ast/ast_adapted.hpp>
#include <parse/parse.hpp>
#include <parse/enum.hpp>

namespace x3     = boost::spirit::x3;
namespace fusion = boost::fusion;

namespace miko::parse
{

//===----------------------------------------------------------------------===//
// Error handling
//===----------------------------------------------------------------------===//

struct with_error_handling {
  template <typename Iterator, typename Context>
  x3::error_handler_result on_error([[maybe_unused]] Iterator&       first,
                                    [[maybe_unused]] const Iterator& last,
                                    const x3::expectation_failure<Iterator>& x,
                                    Context const& context) const
  {
    ++total_errors;

    auto&& error_handler = x3::get<x3::error_handler_tag>(context).get();
    error_handler(x.where(),
                  format_error_message_without_filename(
                    "expected: " + boost::core::demangle(x.which().c_str())));
    return x3::error_handler_result::fail;
  }

  static std::size_t total_errors;
};

std::size_t with_error_handling::total_errors = 0;

//===----------------------------------------------------------------------===//
// Annotations
//===----------------------------------------------------------------------===//

// tag used to get the position cache from the context
struct position_cache_tag;

struct annotate_position {
  template <typename T, typename Iterator, typename Context>
  inline void on_success(const Iterator& first,
                         const Iterator& last,
                         T&              ast,
                         const Context&  context)
  {
    auto&& position_cache = x3::get<position_cache_tag>(context);
    position_cache.annotate(ast, first, last);
  }
};

//===----------------------------------------------------------------------===//
// Semantic actions
//===----------------------------------------------------------------------===//

namespace action
{

const auto assign_attr_to_val = [](auto&& ctx) -> void {
  x3::_val(ctx) = std::move(x3::_attr(ctx));
};

const auto assign_unaryop_to_val = [](auto&& ctx) -> void {
  x3::_val(ctx)
    = ast::unary_op_expr{fusion::at_c<0>(x3::_attr(ctx)) /* operator */,
                         fusion::at_c<1>(x3::_attr(ctx)) /* right hand side*/};
};

const auto assign_binop_to_val = [](auto&& ctx) -> void {
  x3::_val(ctx) = ast::binary_op_expr{
    x3::_val(ctx),
    fusion::at_c<0>(x3::_attr(ctx)) /* operator */,
    fusion::at_c<1>(x3::_attr(ctx)) /* right hand side */};
};

const auto assign_variable_to_val = [](auto&& ctx) -> void {
  x3::_val(ctx) = ast::variable_expr{x3::_attr(ctx) /* name */};
};

const auto assign_function_call_to_val = [](auto&& ctx) -> void {
  x3::_val(ctx)
    = ast::function_call_expr{fusion::at_c<0>(x3::_attr(ctx)) /* callee */,
                              fusion::at_c<1>(x3::_attr(ctx)) /* arguments */};
};

const auto assign_compound_statement_to_val = [](auto&& ctx) -> void {
  x3::_val(ctx) = ast::compound_statement{x3::_attr(ctx)};
};

const auto char_to_string = [](auto&& ctx) -> void {
  x3::_val(ctx) = std::string{x3::_attr(ctx)};
};

} // namespace action

//===----------------------------------------------------------------------===//
// Parsing expression grammar
//===----------------------------------------------------------------------===//

namespace peg
{

//===----------------------------------------------------------------------===//
// Symbol table
//===----------------------------------------------------------------------===//

struct variable_qualifier_tag : x3::symbols<variable_qualifier_id> {
  variable_qualifier_tag()
  {
    // clang-format off
    add
      ("mutable", variable_qualifier_id::mutable_)
    ;
    // clang-format on
  }
} variable_qualifier;

//===----------------------------------------------------------------------===//
// Common rules
//===----------------------------------------------------------------------===//

const auto identifier
  = x3::rule<struct identifier_tag, std::string>{"identifier"}
= x3::raw[x3::lexeme[(x3::alpha | x3::lit('_'))
                     >> *(x3::alnum | x3::lit('_'))]];

const auto keyword = x3::rule<struct keyword_tag, std::string>{"identifier"}
= x3::raw[x3::lexeme[(x3::alpha | x3::lit('_'))
                     >> *(x3::alnum | x3::lit('_'))]];

const auto data_type = x3::rule<struct data_type_tag, std::string>{"data type"}
= x3::string("i32");

const auto integer = x3::rule<struct integer_tag, int>{"integral number"}
= x3::int_;

//===----------------------------------------------------------------------===//
// Expression rules
//===----------------------------------------------------------------------===//

const x3::rule<struct assignment_tag, ast::expression> assignment{"assignment"};
const x3::rule<struct equality_tag, ast::expression>   equality{"equality"};
const x3::rule<struct relational_tag, ast::expression> relational{"relational"};
const x3::rule<struct addition_tag, ast::expression>   addition{"addition"};
const x3::rule<struct multiplication_tag, ast::expression> multiplication{
  "multiplication"};
const x3::rule<struct unary_tag, ast::expression>      unary{"unary"};
const x3::rule<struct primary_tag, ast::expression>    primary{"primary"};
const x3::rule<struct expression_tag, ast::expression> expression{"expression"};

const auto assignment_operator
  = x3::rule<struct assignment_operator_tag, std::string>{"assignment operator"}
= x3::string("=");

const auto equality_operator
  = x3::rule<struct equality_operator_tag, std::string>{"equality operator"}
= x3::string("==") | x3::string("!=");

const auto relational_operator
  = x3::rule<struct relational_operator_tag, std::string>{"relational operator"}
= x3::string("<=") | x3::string(">=") /* <= and >= must come first */
  | x3::string("<") | x3::string(">");

const auto additive_operator
  = x3::rule<struct additive_operator_tag, std::string>{"additive operator"}
= x3::char_("+-")[action::char_to_string];

const auto multitive_operator
  = x3::rule<struct multitive_operator_tag, std::string>{"multitive operator"}
= x3::char_("*/")[action::char_to_string];

const auto unary_operator
  = x3::rule<struct unary_operator_tag, std::string>{"unary operator"}
= x3::char_("+-")[action::char_to_string];

const auto argument_list
  = x3::rule<struct argument_list_tag,
             std::vector<ast::expression>>{"argument list"}
= -(expression >> *(x3::lit(',') > expression));

const auto expression_def = assignment;

const auto assignment_def
  = equality[action::assign_attr_to_val]
    >> *(assignment_operator > equality)[action::assign_binop_to_val];

const auto equality_def
  = relational[action::assign_attr_to_val]
    >> *(equality_operator > relational)[action::assign_binop_to_val];

const auto relational_def
  = addition[action::assign_attr_to_val]
    >> *(relational_operator > addition)[action::assign_binop_to_val];

const auto addition_def
  = multiplication[action::assign_attr_to_val]
    >> *(additive_operator > multiplication)[action::assign_binop_to_val];

const auto multiplication_def
  = unary[action::assign_attr_to_val]
    >> *(multitive_operator > unary)[action::assign_binop_to_val];

const auto unary_def = (unary_operator > primary)[action::assign_unaryop_to_val]
                       | primary[action::assign_attr_to_val];

const auto primary_def
  = (x3::lit('(') > expression > x3::lit(')'))[action::assign_attr_to_val]
    | integer[action::assign_attr_to_val]
    | (identifier >> x3::lit("(") > argument_list
       > x3::lit(")"))[action::assign_function_call_to_val]
    | identifier[action::assign_variable_to_val];

BOOST_SPIRIT_DEFINE(assignment,
                    expression,
                    equality,
                    relational,
                    addition,
                    multiplication,
                    unary,
                    primary)

//===----------------------------------------------------------------------===//
// Statement rules
//===----------------------------------------------------------------------===//

const x3::rule<struct statement_tag, ast::statement> statement{"statement"};

const x3::rule<struct compound_statement_tag, ast::compound_statement>
  compound_statement{"compound statement"};

const auto expression_statement
  = x3::rule<struct expression_statement_tag,
             ast::expression>{"expression statement"}
= expression > x3::lit(';');

const auto variable_def_statement
  = x3::rule<struct variable_def_statement_tag,
             ast::variable_def_statement>{"variable definition"}
= x3::lit("var") > -variable_qualifier > identifier
  > -(x3::lit('=') > expression) > x3::lit(';');

const auto return_statement
  = x3::rule<struct return_statement_tag,
             ast::return_statement>{"return statement"}
= x3::lit("ret") > expression > x3::lit(';');

const auto compound_statement_or_statement
  = x3::rule<struct compound_statement_or_statement,
             ast::compound_statement>{"compound statement or statement"}
= compound_statement[action::assign_compound_statement_to_val]
  | statement[action::assign_compound_statement_to_val];

const auto if_statement
  = x3::rule<struct if_statement_tag, ast::if_statement>{"if else statement"}
= x3::lit("if") > x3::lit('(') > expression > x3::lit(')')
  > compound_statement_or_statement
  > -(x3::lit("else") > compound_statement_or_statement);

const auto for_statement
  = x3::rule<struct for_statement_tag, ast::for_statement>{"for statement"}
= x3::lit("for") > x3::lit('(')
  > -expression /* TODO: support to statement */ > x3::lit(';')
  > -expression /* cond */
  > x3::lit(';') >> -expression /* loop */ > x3::lit(')')
  > compound_statement_or_statement;

const auto statement_def = x3::lit(';') /* null statements */ | return_statement
                           | variable_def_statement | if_statement
                           | for_statement
                           | expression_statement; // TODO: support to block

const auto compound_statement_def = x3::lit('{') > *statement > x3::lit('}');

BOOST_SPIRIT_DEFINE(compound_statement, statement)

//===----------------------------------------------------------------------===//
// Top level rules
//===----------------------------------------------------------------------===//

const auto parameter_list = x3::rule<struct parameter_list_tag,
                                     std::vector<std::string>>{"parameter list"}
= -(identifier >> *(x3::lit(',') > identifier));

const auto function_proto
  = x3::rule<struct function_proto_tag,
             ast::function_declare>{"function prototype"}
= identifier > x3::lit('(') > parameter_list > x3::lit(')');

const auto function_declare
  = x3::rule<struct function_declare_tag,
             ast::function_declare>{"function declaration"}
= x3::lit("extern") > function_proto > x3::lit(';');

const auto function_define
  = x3::rule<struct function_define_tag,
             ast::function_define>{"function definition"}
= x3::lit("func") > function_proto > compound_statement;

const auto top_level_stmt = x3::rule<struct top_level_stmt_tag,
                                     ast::top_level_stmt>{"top level statement"}
= function_declare | function_define;

//===----------------------------------------------------------------------===//
// Comment rules
//===----------------------------------------------------------------------===//

const x3::rule<struct block_comment_tag> block_comment{"block comment"};

const auto single_line_comment
  = x3::rule<struct single_line_comment_tag>{"single line comment"}
= x3::lit("//") >> *(x3::char_ - x3::eol) >> (x3::eol | x3::eoi);

const auto block_comment_def = x3::lit("/*")
                               >> *(block_comment | (x3::char_ - x3::lit("*/")))
                               >> x3::lit("*/");

const auto comment = x3::rule<struct comment_tag>{"comment"}
= single_line_comment | block_comment;

BOOST_SPIRIT_DEFINE(block_comment)

//===----------------------------------------------------------------------===//
// Skipper rule
//===----------------------------------------------------------------------===//

const auto skipper = x3::rule<struct skipper_tag>{"skipper"}
= x3::space | comment;

//===----------------------------------------------------------------------===//
// Program rule
//===----------------------------------------------------------------------===//

const auto program = x3::rule<struct program_tag, ast::program>{"program"}
= *top_level_stmt > x3::eoi;

//===----------------------------------------------------------------------===//
// Expression tags
//===----------------------------------------------------------------------===//

struct argument_list_tag
  : with_error_handling
  , annotate_position {};

struct assignment_tag
  : with_error_handling
  , annotate_position {};

struct expression_tag
  : with_error_handling
  , annotate_position {};

struct equality_tag
  : with_error_handling
  , annotate_position {};

struct relational_tag
  : with_error_handling
  , annotate_position {};

struct addition_tag
  : with_error_handling
  , annotate_position {};

struct multiplication_tag
  : with_error_handling
  , annotate_position {};

struct unary_tag
  : with_error_handling
  , annotate_position {};

struct primary_tag
  : with_error_handling
  , annotate_position {};

//===----------------------------------------------------------------------===//
// Statement tags
//===----------------------------------------------------------------------===//

struct statement_tag
  : with_error_handling
  , annotate_position {};

struct compound_statement_tag
  : with_error_handling
  , annotate_position {};

struct expression_statement_tag
  : with_error_handling
  , annotate_position {};

struct variable_def_statement
  : with_error_handling
  , annotate_position {};

struct return_statement_tag
  : with_error_handling
  , annotate_position {};

struct if_statement_tag
  : with_error_handling
  , annotate_position {};

struct for_statement_tag
  : with_error_handling
  , annotate_position {};

//===----------------------------------------------------------------------===//
// Top level statement tags
//===----------------------------------------------------------------------===//

struct parameter_list_tag
  : with_error_handling
  , annotate_position {};

struct function_proto_tag
  : with_error_handling
  , annotate_position {};

struct function_declare_tag
  : with_error_handling
  , annotate_position {};

struct function_define_tag
  : with_error_handling
  , annotate_position {};

struct top_level_stmt_tag
  : with_error_handling
  , annotate_position {};

//===----------------------------------------------------------------------===//
// Program tag
//===----------------------------------------------------------------------===//

struct program_tag
  : with_error_handling
  , annotate_position {};

} // namespace peg

parser::parser(std::string&& input, const std::filesystem::path& file_path)
  : input{std::move(input)}
  , first{this->input.cbegin()}
  , last{this->input.cend()}
  , positions{first, last}
  , file_path{file_path}
{
  parse();
}

parser::parser(const std::string& input, const std::filesystem::path& file_path)
  : input{input}
  , first{input.cbegin()}
  , last{input.cend()}
  , positions{first, last}
  , file_path{file_path}
{
  parse();
}

[[nodiscard]] const ast::program& parser::get_ast() const noexcept
{
  return ast;
}

[[nodiscard]] const position_cache& parser::get_positions() const noexcept
{
  return positions;
}

void parser::parse()
{
  x3::error_handler<input_iterator_type> error_handler{first,
                                                       last,
                                                       std::cerr,
                                                       file_path.string()};

  const auto parser = x3::with<x3::error_handler_tag>(std::ref(
    error_handler))[x3::with<position_cache_tag>(positions)[peg::program]];

  const auto success = x3::phrase_parse(first, last, parser, peg::skipper, ast);

  if (!success || first != last) {
    throw std::runtime_error{
      format("%zu errors generated.\n", with_error_handling::total_errors)};
  }
}

} // namespace miko::parse
