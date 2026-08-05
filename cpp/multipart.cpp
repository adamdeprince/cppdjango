#include "multipart.hpp"

#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {
namespace {

[[nodiscard]] bool is_ws(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

[[nodiscard]] std::string_view lstrip_view(std::string_view s) {
  while (!s.empty() && is_ws(s.front())) {
    s.remove_prefix(1);
  }
  return s;
}

[[nodiscard]] std::string_view rstrip_view(std::string_view s) {
  while (!s.empty() && is_ws(s.back())) {
    s.remove_suffix(1);
  }
  return s;
}

[[nodiscard]] std::string_view strip_view(std::string_view s) {
  return rstrip_view(lstrip_view(s));
}

[[nodiscard]] std::string ascii_lower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') {
      out += static_cast<char>(c - 'A' + 'a');
    } else {
      out += c;
    }
  }
  return out;
}

[[nodiscard]] int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Percent-decode (no '+'), return raw bytes as std::string.
[[nodiscard]] std::string percent_decode_bytes(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      const int hi = hex_value(in[i + 1]);
      const int lo = hex_value(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += in[i];
  }
  return out;
}

[[nodiscard]] std::string utf8_decode_replace(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  std::size_t i = 0;
  while (i < bytes.size()) {
    const auto b0 = static_cast<unsigned char>(bytes[i]);
    if (b0 < 0x80) {
      out += static_cast<char>(b0);
      ++i;
      continue;
    }
    auto fffd = [&] { out += "\xEF\xBF\xBD"; };
    int need = 0;
    char32_t cp = 0;
    if ((b0 & 0xE0) == 0xC0) {
      need = 1;
      cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
      need = 2;
      cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
      need = 3;
      cp = b0 & 0x07;
    } else {
      fffd();
      ++i;
      continue;
    }
    if (i + static_cast<std::size_t>(need) >= bytes.size()) {
      fffd();
      break;
    }
    bool ok = true;
    for (int j = 1; j <= need; ++j) {
      const auto bj =
          static_cast<unsigned char>(bytes[i + static_cast<std::size_t>(j)]);
      if ((bj & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (bj & 0x3F);
    }
    if (!ok) {
      fffd();
      ++i;
      continue;
    }
    // re-encode (valid sequences stay identical)
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    i += static_cast<std::size_t>(need) + 1;
  }
  return out;
}

// unquote with encoding for RFC2231. Supports utf-8, us-ascii, iso-8859-1.
[[nodiscard]] std::string unquote_encoded(std::string_view value,
                                          std::string_view encoding) {
  const std::string raw = percent_decode_bytes(value);
  const std::string enc = ascii_lower(encoding);
  if (enc == "utf-8" || enc == "utf8") {
    return utf8_decode_replace(raw);
  }
  // us-ascii / iso-8859-1 / latin-1: each byte → code unit (UTF-8 encode if needed)
  if (enc == "us-ascii" || enc == "iso-8859-1" || enc == "latin-1" ||
      enc == "iso8859-1" || enc == "latin1") {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char b : raw) {
      if (b < 0x80) {
        out += static_cast<char>(b);
      } else {
        // Latin-1 byte as Unicode codepoint in UTF-8
        out += static_cast<char>(0xC0 | (b >> 6));
        out += static_cast<char>(0x80 | (b & 0x3F));
      }
    }
    return out;
  }
  // Unknown encoding: treat as latin-1
  std::string out;
  out.reserve(raw.size());
  for (unsigned char b : raw) {
    if (b < 0x80) {
      out += static_cast<char>(b);
    } else {
      out += static_cast<char>(0xC0 | (b >> 6));
      out += static_cast<char>(0x80 | (b & 0x3F));
    }
  }
  return out;
}

// Count unescaped quotes in s[0:end).
[[nodiscard]] int unescaped_quote_count(std::string_view s, std::size_t end) {
  int count = 0;
  for (std::size_t i = 0; i < end && i < s.size(); ++i) {
    if (s[i] == '"') {
      // Count if not escaped: preceding backslash count is even
      std::size_t bs = 0;
      std::size_t j = i;
      while (j > 0 && s[j - 1] == '\\') {
        ++bs;
        --j;
      }
      if (bs % 2 == 0) {
        ++count;
      }
    }
  }
  return count;
}

// Yield semicolon-separated params from s which starts with ';'.
// Mirrors django.utils.http._parseparam.
void parse_param_segments(std::string_view s,
                          std::vector<std::string_view>& out) {
  while (!s.empty() && s.front() == ';') {
    s.remove_prefix(1);
    std::size_t end = s.find(';');
    while (end != std::string_view::npos &&
           (unescaped_quote_count(s, end) % 2) != 0) {
      end = s.find(';', end + 1);
    }
    if (end == std::string_view::npos) {
      end = s.size();
    }
    std::string_view f = strip_view(s.substr(0, end));
    out.push_back(f);
    s = s.substr(end);
  }
}

[[nodiscard]] std::string unescape_quoted(std::string_view value) {
  // value includes surrounding quotes already stripped
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && i + 1 < value.size()) {
      // Python: value.replace("\\\\", "\\").replace('\\"', '"')
      // Applied as sequential replaces on full string - order matters.
      // Actually Python does: replace \\\\ first then \"
      // For sequential scan, handle \\ and \"
      if (value[i + 1] == '\\' || value[i + 1] == '"') {
        out += value[i + 1];
        ++i;
        continue;
      }
    }
    out += value[i];
  }
  return out;
}

// Python does: value.replace("\\\\", "\\").replace('\\"', '"')
// which is global sequential replace, not escape-sequence parsing.
[[nodiscard]] std::string python_unescape_quoted(std::string value) {
  // Replace \\ first
  std::string tmp;
  tmp.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') {
      tmp += '\\';
      ++i;
    } else {
      tmp += value[i];
    }
  }
  // Then \"
  std::string out;
  out.reserve(tmp.size());
  for (std::size_t i = 0; i < tmp.size(); ++i) {
    if (tmp[i] == '\\' && i + 1 < tmp.size() && tmp[i + 1] == '"') {
      out += '"';
      ++i;
    } else {
      out += tmp[i];
    }
  }
  return out;
}

[[nodiscard]] bool is_printable_unicode(char32_t cp) {
  if (cp == ' ') {
    return true;
  }
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) {
    return false;
  }
  if (cp >= 0xD800 && cp <= 0xDFFF) {
    return false;
  }
  return true;
}

[[nodiscard]] std::optional<std::string> get_param(
    const std::vector<std::pair<std::string, std::string>>& params,
    std::string_view key) {
  for (const auto& [k, v] : params) {
    if (k == key) {
      return v;
    }
  }
  return std::nullopt;
}

}  // namespace

HeaderParameters parse_header_parameters(std::string_view line,
                                         std::optional<std::size_t> max_length) {
  if (line.empty()) {
    return {};
  }
  if (max_length.has_value() && line.size() > *max_length) {
    throw std::invalid_argument("Unable to parse header parameters (value too long).");
  }

  std::vector<std::string_view> parts;
  // _parseparam(";" + line)
  std::string prefixed;
  prefixed.reserve(line.size() + 1);
  prefixed.push_back(';');
  prefixed.append(line);
  parse_param_segments(prefixed, parts);

  HeaderParameters result;
  if (parts.empty()) {
    return result;
  }
  result.main_value = ascii_lower(parts[0]);

  for (std::size_t pi = 1; pi < parts.size(); ++pi) {
    const std::string_view p = parts[pi];
    const std::size_t eq = p.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    bool has_encoding = false;
    std::string name = ascii_lower(strip_view(p.substr(0, eq)));
    if (!name.empty() && name.back() == '*') {
      name.pop_back();
      // p.count("'") == 2
      int quotes = 0;
      for (char c : p) {
        if (c == '\'') {
          ++quotes;
        }
      }
      if (quotes == 2) {
        has_encoding = true;
      }
    }
    std::string value(strip_view(p.substr(eq + 1)));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = python_unescape_quoted(value.substr(1, value.size() - 2));
    }
    if (has_encoding) {
      // encoding, lang, value = value.split("'")  — exactly 3 parts
      const std::size_t q1 = value.find('\'');
      const std::size_t q2 = value.find('\'', q1 == std::string::npos ? 0 : q1 + 1);
      if (q1 != std::string::npos && q2 != std::string::npos) {
        const std::string encoding = value.substr(0, q1);
        // lang = value.substr(q1+1, q2-q1-1) unused
        const std::string encoded = value.substr(q2 + 1);
        value = unquote_encoded(encoded, encoding);
      }
    }
    result.params.emplace_back(std::move(name), std::move(value));
  }
  return result;
}

MultipartHeadersResult parse_multipart_headers(std::string_view header_block) {
  MultipartHeadersResult result;
  result.type = MultipartPartType::Raw;

  std::size_t start = 0;
  while (start <= header_block.size()) {
    std::size_t end = header_block.find("\r\n", start);
    std::string_view line;
    if (end == std::string_view::npos) {
      line = header_block.substr(start);
      start = header_block.size() + 1;
    } else {
      line = header_block.substr(start, end - start);
      start = end + 2;
    }
    if (line.empty()) {
      continue;
    }
    // header_name, value_and_params = line.decode().split(":", 1)
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;  // Invalid header
    }
    std::string name = ascii_lower(rstrip_view(line.substr(0, colon)));
    std::string_view value_and_params = lstrip_view(line.substr(colon + 1));
    HeaderParameters hp;
    try {
      hp = parse_header_parameters(value_and_params, std::nullopt);
    } catch (const std::invalid_argument&) {
      continue;
    }

    if (name == "content-disposition") {
      result.type = MultipartPartType::Field;
      if (get_param(hp.params, "filename").has_value()) {
        result.type = MultipartPartType::File;
      }
    }

    MultipartHeaderEntry entry;
    entry.name = std::move(name);
    entry.value = std::move(hp.main_value);
    entry.params = std::move(hp.params);
    result.headers.push_back(std::move(entry));
  }
  return result;
}

std::optional<std::pair<std::size_t, std::size_t>> find_multipart_boundary(
    std::string_view data, std::string_view boundary) {
  const std::size_t index = data.find(boundary);
  if (index == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t end = index;
  const std::size_t next = index + boundary.size();
  if (end > 0 && data[end - 1] == '\n') {
    --end;
  }
  if (end > 0 && data[end - 1] == '\r') {
    --end;
  }
  return std::make_pair(end, next);
}

std::optional<std::string> sanitize_multipart_filename(std::string_view file_name) {
  std::string name(file_name);
  auto last_seg = [](std::string& s, char sep) {
    const auto pos = s.rfind(sep);
    if (pos != std::string::npos) {
      s = s.substr(pos + 1);
    }
  };
  last_seg(name, '/');
  last_seg(name, '\\');

  std::string cleaned;
  cleaned.reserve(name.size());
  for (std::size_t i = 0; i < name.size();) {
    const auto b0 = static_cast<unsigned char>(name[i]);
    char32_t cp = 0;
    int need = 0;
    if (b0 < 0x80) {
      cp = b0;
      need = 0;
    } else if ((b0 & 0xE0) == 0xC0 && i + 1 < name.size()) {
      cp = b0 & 0x1F;
      need = 1;
    } else if ((b0 & 0xF0) == 0xE0 && i + 2 < name.size()) {
      cp = b0 & 0x0F;
      need = 2;
    } else if ((b0 & 0xF8) == 0xF0 && i + 3 < name.size()) {
      cp = b0 & 0x07;
      need = 3;
    } else {
      ++i;
      continue;
    }
    bool ok = true;
    for (int j = 1; j <= need; ++j) {
      const auto bj =
          static_cast<unsigned char>(name[i + static_cast<std::size_t>(j)]);
      if ((bj & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (bj & 0x3F);
    }
    if (!ok) {
      ++i;
      continue;
    }
    if (is_printable_unicode(cp)) {
      cleaned.append(name, i, static_cast<std::size_t>(need) + 1);
    }
    i += static_cast<std::size_t>(need) + 1;
  }

  if (cleaned.empty() || cleaned == "." || cleaned == "..") {
    return std::nullopt;
  }
  return cleaned;
}

std::vector<std::string> split_multipart_parts(std::string_view body,
                                               std::string_view separator) {
  std::vector<std::string> parts;
  std::size_t pos = 0;
  while (pos < body.size()) {
    const std::size_t index = body.find(separator, pos);
    if (index == std::string_view::npos) {
      break;
    }
    std::size_t end = index;
    if (end > pos && body[end - 1] == '\n') {
      --end;
    }
    if (end > pos && body[end - 1] == '\r') {
      --end;
    }
    if (end > pos) {
      std::size_t start = pos;
      if (start + 1 < end && body[start] == '\r' && body[start + 1] == '\n') {
        start += 2;
      } else if (start < end && body[start] == '\n') {
        start += 1;
      }
      if (end > start) {
        parts.emplace_back(body.substr(start, end - start));
      }
    }
    pos = index + separator.size();
    if (pos + 1 < body.size() && body[pos] == '-' && body[pos + 1] == '-') {
      break;
    }
    if (pos + 1 < body.size() && body[pos] == '\r' && body[pos + 1] == '\n') {
      pos += 2;
    } else if (pos < body.size() && body[pos] == '\n') {
      pos += 1;
    }
  }
  return parts;
}

BoundaryChunkResult boundary_chunk_slice(std::string_view chunk,
                                         std::string_view boundary,
                                         std::size_t rollback) {
  BoundaryChunkResult r;
  auto found = find_multipart_boundary(chunk, boundary);
  if (found) {
    r.found = true;
    r.done = true;
    r.yield_end = found->first;
    r.unget_start = found->second;
    return r;
  }
  // No boundary: protect trailing partial boundary bytes via rollback.
  if (rollback == 0 || chunk.size() <= rollback) {
    r.found = false;
    r.done = true;
    r.yield_end = chunk.size();
    r.unget_start = chunk.size();
    return r;
  }
  r.found = false;
  r.done = false;
  r.yield_end = chunk.size() - rollback;
  r.unget_start = chunk.size() - rollback;
  return r;
}

std::optional<std::size_t> find_header_block_end(std::string_view chunk) noexcept {
  const auto pos = chunk.find("\r\n\r\n");
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  return pos;
}

std::vector<MultipartPart> parse_multipart_message(std::string_view body,
                                                   std::string_view boundary) {
  std::string separator = "--";
  separator.append(boundary);
  const auto raw_parts = split_multipart_parts(body, separator);

  std::vector<MultipartPart> parts;
  parts.reserve(raw_parts.size());

  for (const auto& raw : raw_parts) {
    MultipartPart part;
    const std::size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      // No headers — RAW with full content as body, or empty
      part.type = MultipartPartType::Raw;
      part.body = raw;
      parts.push_back(std::move(part));
      continue;
    }
    const std::string_view header_block = std::string_view(raw).substr(0, header_end);
    auto headers = parse_multipart_headers(header_block);
    part.type = headers.type;
    part.headers = std::move(headers.headers);
    part.body = raw.substr(header_end + 4);

    // Populate convenience fields
    for (const auto& h : part.headers) {
      if (h.name == "content-disposition") {
        if (auto n = get_param(h.params, "name")) {
          part.name = *n;
        }
        if (auto f = get_param(h.params, "filename")) {
          part.filename = *f;
        }
      } else if (h.name == "content-type") {
        part.content_type = h.value;
        if (auto cs = get_param(h.params, "charset")) {
          // keep charset in params; content_type is main value only (matches Django)
          (void)cs;
        }
      } else if (h.name == "content-transfer-encoding") {
        part.transfer_encoding = h.value;
      }
    }
    parts.push_back(std::move(part));
  }
  return parts;
}

}  // namespace django::native
