// Multipart/form-data + Content-Type style header parsing.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace django::native {

// Part classification matching MultiPartParser RAW/FIELD/FILE.
enum class MultipartPartType {
  Raw = 0,
  Field = 1,
  File = 2,
};

struct HeaderParameters {
  std::string main_value;  // lowercased main token (e.g. "form-data")
  std::vector<std::pair<std::string, std::string>> params;  // name -> value
};

// Parse a Content-Type / Content-Disposition style header value.
// Throws std::invalid_argument if max_length is exceeded.
// RFC 2231 *params supported for utf-8 / us-ascii / iso-8859-1.
[[nodiscard]] HeaderParameters parse_header_parameters(
    std::string_view line, std::optional<std::size_t> max_length);

struct MultipartHeaderEntry {
  std::string name;   // lowercased
  std::string value;  // main value
  std::vector<std::pair<std::string, std::string>> params;
};

struct MultipartHeadersResult {
  MultipartPartType type = MultipartPartType::Raw;
  std::vector<MultipartHeaderEntry> headers;
};

// Parse a raw header block (without the trailing CRLFCRLF).
[[nodiscard]] MultipartHeadersResult parse_multipart_headers(std::string_view header_block);

struct MultipartPart {
  MultipartPartType type = MultipartPartType::Raw;
  std::vector<MultipartHeaderEntry> headers;
  std::string body;  // raw body bytes (as std::string)
  std::string name;
  std::string filename;
  std::string content_type;
  std::string transfer_encoding;
};

// Parse a complete multipart body. `boundary` is the boundary token *without*
// the leading "--" (as stored on MultiPartParser._boundary).
[[nodiscard]] std::vector<MultipartPart> parse_multipart_message(
    std::string_view body, std::string_view boundary);

// Find multipart boundary in data. `boundary` should include the "--" prefix
// used by BoundaryIter (i.e. b"--" + boundary_token).
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> find_multipart_boundary(
    std::string_view data, std::string_view boundary);

// Sanitize an upload filename (after html.unescape).
[[nodiscard]] std::optional<std::string> sanitize_multipart_filename(
    std::string_view file_name);

// Split a complete multipart body into raw part payloads (headers+body).
// separator is b"--" + boundary_token.
[[nodiscard]] std::vector<std::string> split_multipart_parts(
    std::string_view body, std::string_view separator);

// Streaming BoundaryIter helper: given a buffered chunk, find boundary or
// apply rollback. Returns:
//   found=true  → yield data[0:end], unget data[next:], done
//   found=false → if chunk large enough, yield data[0:-rollback], unget tail
//                 else yield whole chunk and done
struct BoundaryChunkResult {
  bool found = false;
  bool done = false;
  std::size_t yield_end = 0;   // exclusive end of bytes to yield
  std::size_t unget_start = 0; // start of bytes to push back (may be size = none)
};

[[nodiscard]] BoundaryChunkResult boundary_chunk_slice(std::string_view chunk,
                                                       std::string_view boundary,
                                                       std::size_t rollback);

// Find end of multipart header block (index of \r\n\r\n), or npos.
[[nodiscard]] std::optional<std::size_t> find_header_block_end(
    std::string_view chunk) noexcept;

}  // namespace django::native
