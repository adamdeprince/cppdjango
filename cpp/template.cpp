#include "template.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {
namespace {

// Find next tag matching ({%.*?%}|{{.*?}}|{#.*?#}) — '.' does not match '\n'.
[[nodiscard]] std::pair<std::size_t, std::size_t> find_next_tag(std::string_view s,
                                                                std::size_t from) {
  std::size_t i = from;
  while (i + 1 < s.size()) {
    if (s[i] != '{') {
      ++i;
      continue;
    }
    const char second = s[i + 1];
    char end_marker = 0;
    if (second == '%') {
      end_marker = '%';
    } else if (second == '{') {
      end_marker = '}';
    } else if (second == '#') {
      end_marker = '#';
    } else {
      ++i;
      continue;
    }
    std::size_t j = i + 2;
    while (j + 1 < s.size()) {
      if (s[j] == '\n') {
        break;
      }
      if (s[j] == end_marker && s[j + 1] == '}') {
        return {i, j + 2};
      }
      ++j;
    }
    ++i;
  }
  return {std::string_view::npos, std::string_view::npos};
}

[[nodiscard]] std::string_view strip_ws(std::string_view s) {
  while (!s.empty() &&
         (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' ||
          s.front() == '\r' || s.front() == '\f' || s.front() == '\v')) {
    s.remove_prefix(1);
  }
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r' ||
          s.back() == '\f' || s.back() == '\v')) {
    s.remove_suffix(1);
  }
  return s;
}

[[nodiscard]] int count_newlines(std::string_view s) {
  int n = 0;
  for (char c : s) {
    if (c == '\n') {
      ++n;
    }
  }
  return n;
}

// content[:9] in ("verbatim", "verbatim ")
[[nodiscard]] bool is_verbatim_start(std::string_view content) {
  if (content == "verbatim") {
    return true;
  }
  return content.size() >= 9 && content.substr(0, 9) == "verbatim ";
}

}  // namespace

std::vector<TemplateToken> template_tokenize(std::string_view source,
                                              bool with_position) {
  std::vector<TemplateToken> result;
  result.reserve(32);

  std::string verbatim;
  int lineno = 1;
  std::size_t last = 0;

  auto emit = [&](std::size_t start, std::size_t end, bool is_tag) {
    if (start >= end) {
      return;
    }
    const std::string_view token_string = source.substr(start, end - start);
    TemplateToken tok;
    tok.lineno = lineno;
    if (with_position) {
      tok.position = std::make_pair(static_cast<int>(start), static_cast<int>(end));
    }

    if (is_tag) {
      const char open = token_string[1];  // '%', '{', or '#'
      if (open == '%') {
        const std::string_view content =
            strip_ws(token_string.substr(2, token_string.size() - 4));
        if (!verbatim.empty()) {
          if (content != std::string_view(verbatim)) {
            tok.type = TemplateTokenType::Text;
            tok.contents = std::string(token_string);
            result.push_back(std::move(tok));
            lineno += count_newlines(token_string);
            return;
          }
          verbatim.clear();
        } else if (is_verbatim_start(content)) {
          verbatim = "end";
          verbatim.append(content);
        }
        tok.type = TemplateTokenType::Block;
        tok.contents = std::string(content);
        result.push_back(std::move(tok));
        lineno += count_newlines(token_string);
        return;
      }
      if (verbatim.empty()) {
        const std::string_view content =
            strip_ws(token_string.substr(2, token_string.size() - 4));
        if (open == '{') {
          tok.type = TemplateTokenType::Var;
        } else {
          tok.type = TemplateTokenType::Comment;
        }
        tok.contents = std::string(content);
        result.push_back(std::move(tok));
        lineno += count_newlines(token_string);
        return;
      }
      // Inside verbatim: non-ending tags are raw text.
      tok.type = TemplateTokenType::Text;
      tok.contents = std::string(token_string);
      result.push_back(std::move(tok));
      lineno += count_newlines(token_string);
      return;
    }

    tok.type = TemplateTokenType::Text;
    tok.contents = std::string(token_string);
    result.push_back(std::move(tok));
    lineno += count_newlines(token_string);
  };

  while (true) {
    auto [ts, te] = find_next_tag(source, last);
    if (ts == std::string_view::npos) {
      emit(last, source.size(), /*is_tag=*/false);
      break;
    }
    emit(last, ts, /*is_tag=*/false);
    emit(ts, te, /*is_tag=*/true);
    last = te;
  }

  return result;
}

}  // namespace django::native
