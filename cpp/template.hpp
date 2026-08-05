// Django template lexer (tokenize → tokens).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

// Matches django.template.base.TokenType values.
enum class TemplateTokenType {
  Text = 0,
  Var = 1,
  Block = 2,
  Comment = 3,
};

struct TemplateToken {
  TemplateTokenType type = TemplateTokenType::Text;
  std::string contents;
  int lineno = 1;
  // If with_position is true when tokenizing, these are set.
  std::optional<std::pair<int, int>> position;
};

// Tokenize a template string. If with_position, record (start, end) source spans
// like DebugLexer.
[[nodiscard]] std::vector<TemplateToken> template_tokenize(std::string_view source,
                                                           bool with_position);

}  // namespace django::native
