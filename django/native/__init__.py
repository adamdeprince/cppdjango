"""
Native C++ acceleration layer for this Django fork.

Public import surface for accelerated primitives. Prefer this package over
importing ``django._native`` directly so pure-Python fallbacks remain
available when the extension is missing or disabled.

Dispatch policy:
    1. If ``DJANGO_NATIVE`` is disabled, use :mod:`django.native.fallbacks`.
    2. Else if ``django._native`` imports, call into the C++ extension.
    3. Else use pure-Python fallbacks.
"""

from __future__ import annotations

import datetime
import unicodedata
from types import ModuleType

from django.native import fallbacks
from django.native._loader import (
    AVAILABLE,
    DISABLED_BY_ENV,
    get_native_module,
    is_native_enabled,
)

__all__ = [
    "AVAILABLE",
    "DISABLED_BY_ENV",
    "add",
    "compile_route",
    "compiler",
    "converter_int_to_python",
    "converter_int_to_url",
    "converter_uuid_to_python",
    "converter_uuid_to_url",
    "cxx_standard",
    "escapejs",
    "fallbacks",
    "get_native_module",
    "get_valid_filename",
    "html_escape",
    "is_native_enabled",
    "match_route",
    "parse_date",
    "parse_datetime",
    "parse_duration",
    "parse_qsl",
    "parse_time",
    "find_multipart_boundary",
    "parse_filter_expression",
    "parse_header_parameters",
    "parse_multipart_headers",
    "parse_multipart_message",
    "parse_variable",
    "resolve_dict_lookups",
    "sanitize_multipart_filename",
    "slugify",
    "slugify_core",
    "smart_split",
    "split_multipart_parts",
    "template_tokenize",
    "unescape_string_literal",
    "version",
]


def _impl() -> ModuleType | None:
    return get_native_module()


def add(a: int, b: int) -> int:
    impl = _impl()
    if impl is not None:
        return impl.add(a, b)
    return fallbacks.add(a, b)


def version() -> str:
    impl = _impl()
    if impl is not None:
        return impl.version()
    return fallbacks.version()


def cxx_standard() -> str:
    impl = _impl()
    if impl is not None:
        return impl.cxx_standard()
    return fallbacks.cxx_standard()


def compiler() -> str:
    impl = _impl()
    if impl is not None:
        return impl.compiler()
    return fallbacks.compiler()


def html_escape(text: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.html_escape(text)
    return fallbacks.html_escape(text)


def escapejs(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.escapejs(value)
    return fallbacks.escapejs(value)


def slugify_core(value: str, allow_unicode: bool = False) -> str:
    impl = _impl()
    if impl is not None:
        return impl.slugify_core(value, allow_unicode)
    return fallbacks.slugify_core(value, allow_unicode)


def slugify(value: str, allow_unicode: bool = False) -> str:
    """
    Full slugify: Unicode normalize in Python, core transform via native/fallback.

    Normalization stays in Python (unicodedata). The C++ core handles lower
    (ASCII mode), character filtering, dash collapsing, and strip.
    """
    if allow_unicode:
        value = unicodedata.normalize("NFKC", value).lower()
        return slugify_core(value, allow_unicode=True)
    value = (
        unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode("ascii")
    )
    return slugify_core(value, allow_unicode=False)


def get_valid_filename(name: str) -> str | None:
    """Sanitize filename; ``None`` means empty / '.' / '..' (caller raises)."""
    impl = _impl()
    if impl is not None:
        return impl.get_valid_filename(name)
    return fallbacks.get_valid_filename(name)


def parse_date(value: str) -> datetime.date | None:
    """Regex-path date parse (used after ``date.fromisoformat`` fails)."""
    impl = _impl()
    if impl is not None:
        return impl.parse_date(value)
    return fallbacks.parse_date(value)


def parse_time(value: str) -> datetime.time | None:
    impl = _impl()
    if impl is not None:
        return impl.parse_time(value)
    return fallbacks.parse_time(value)


def parse_datetime(value: str) -> datetime.datetime | None:
    impl = _impl()
    if impl is not None:
        return impl.parse_datetime(value)
    return fallbacks.parse_datetime(value)


def parse_duration(value: str) -> datetime.timedelta | None:
    impl = _impl()
    if impl is not None:
        return impl.parse_duration(value)
    return fallbacks.parse_duration(value)


def _is_utf8_encoding(encoding: str) -> bool:
    normalized = encoding.lower().replace("_", "-").replace(" ", "")
    return normalized in {"utf-8", "utf8"}


def parse_qsl(
    qs: str,
    *,
    encoding: str = "utf-8",
    max_num_fields: int | None = None,
) -> list[tuple[str, str]]:
    """
    Parse a query string into ``(key, value)`` pairs.

    Matches ``urllib.parse.parse_qsl`` with ``keep_blank_values=True`` and
    ``errors='replace'``. UTF-8 uses the C++ path when available; other
    encodings use the pure-Python fallback (codec support).
    """
    impl = _impl()
    if impl is not None and _is_utf8_encoding(encoding):
        return impl.parse_qsl_utf8(qs, max_num_fields)
    return fallbacks.parse_qsl(qs, encoding=encoding, max_num_fields=max_num_fields)


def compile_route(route: str, is_endpoint: bool = False):
    """Compile a path() route for native matching, or None if unsupported."""
    impl = _impl()
    if impl is not None:
        return impl.compile_route(route, is_endpoint)
    return fallbacks.compile_route(route, is_endpoint)


def match_route(route, path: str):
    """Match path against a CompiledRoute; None or (remaining, pairs)."""
    impl = _impl()
    if impl is not None and route is not None:
        return impl.match_route(route, path)
    return fallbacks.match_route(route, path)


def converter_int_to_python(value: str) -> int:
    impl = _impl()
    if impl is not None:
        return impl.converter_int_to_python(value)
    return fallbacks.converter_int_to_python(value)


def converter_int_to_url(value) -> str:
    impl = _impl()
    if impl is not None:
        return impl.converter_int_to_url(value)
    return fallbacks.converter_int_to_url(value)


def converter_uuid_to_python(value: str):
    impl = _impl()
    if impl is not None:
        return impl.converter_uuid_to_python(value)
    return fallbacks.converter_uuid_to_python(value)


def converter_uuid_to_url(value) -> str:
    impl = _impl()
    if impl is not None:
        return impl.converter_uuid_to_url(value)
    return fallbacks.converter_uuid_to_url(value)


def converter_str_to_python(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.converter_str_to_python(value)
    return fallbacks.converter_str_to_python(value)


def converter_str_to_url(value) -> str:
    impl = _impl()
    if impl is not None:
        return impl.converter_str_to_url(value)
    return fallbacks.converter_str_to_url(value)


def converter_slug_to_python(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.converter_slug_to_python(value)
    return fallbacks.converter_slug_to_python(value)


def converter_slug_to_url(value) -> str:
    impl = _impl()
    if impl is not None:
        return impl.converter_slug_to_url(value)
    return fallbacks.converter_slug_to_url(value)


def converter_path_to_python(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.converter_path_to_python(value)
    return fallbacks.converter_path_to_python(value)


def converter_path_to_url(value) -> str:
    impl = _impl()
    if impl is not None:
        return impl.converter_path_to_url(value)
    return fallbacks.converter_path_to_url(value)


def reverse_quote(decoded: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.reverse_quote(decoded)
    return fallbacks.reverse_quote(decoded)


def is_valid_slug(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_valid_slug(value)
    return fallbacks.is_valid_slug(value)


def is_valid_integer_string(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_valid_integer_string(value)
    return fallbacks.is_valid_integer_string(value)


def form_integer_to_python(value):
    impl = _impl()
    if impl is not None:
        return impl.form_integer_to_python(value)
    return fallbacks.form_integer_to_python(value)


def is_valid_ipv4(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_valid_ipv4(value)
    return fallbacks.is_valid_ipv4(value)


def is_valid_ipv6(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_valid_ipv6(value)
    return fallbacks.is_valid_ipv6(value)


def clean_ipv6_address(ip: str, unpack_ipv4: bool = False, max_length: int = 39):
    """Return compressed IPv6 string, or None if invalid (Python raises)."""
    impl = _impl()
    if impl is not None:
        return impl.clean_ipv6_address(ip, unpack_ipv4, max_length)
    return fallbacks.clean_ipv6_address(ip, unpack_ipv4, max_length)


def is_valid_ipv46(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_valid_ipv46(value)
    return fallbacks.is_valid_ipv46(value)


def is_valid_email(value: str, allowlist=None) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_valid_email(value, allowlist)
    return fallbacks.is_valid_email(value, allowlist)


def has_null_characters(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.has_null_characters(value)
    return fallbacks.has_null_characters(value)


def char_field_strip(value: str, strip: bool = True) -> str:
    impl = _impl()
    if impl is not None:
        return impl.char_field_strip(value, strip)
    return fallbacks.char_field_strip(value, strip)


def b62_encode(value: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.b62_encode(value)
    return fallbacks.b62_encode(value)


def b62_decode(s: str) -> int:
    impl = _impl()
    if impl is not None:
        return impl.b62_decode(s)
    return fallbacks.b62_decode(s)


def signing_b64_encode(data: bytes) -> bytes:
    impl = _impl()
    if impl is not None:
        return impl.signing_b64_encode(data)
    return fallbacks.signing_b64_encode(data)


def signing_b64_decode(data: str) -> bytes:
    impl = _impl()
    if impl is not None:
        return impl.signing_b64_decode(data)
    return fallbacks.signing_b64_decode(data)


def constant_time_compare(a, b) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.constant_time_compare(a, b)
    return fallbacks.constant_time_compare(a, b)


def signer_sep_unsafe(sep: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.signer_sep_unsafe(sep)
    return fallbacks.signer_sep_unsafe(sep)


def truncate_chars(text: str, length: int, truncate_suffix: str = "…") -> str:
    impl = _impl()
    if impl is not None:
        return impl.truncate_chars(text, length, truncate_suffix)
    return fallbacks.truncate_chars(text, length, truncate_suffix)


def truncate_words(text: str, length: int, truncate_suffix: str = "…") -> str:
    impl = _impl()
    if impl is not None:
        return impl.truncate_words(text, length, truncate_suffix)
    return fallbacks.truncate_words(text, length, truncate_suffix)


def querydict_urlencode(pairs, safe: str = "") -> str:
    impl = _impl()
    if impl is not None:
        return impl.querydict_urlencode(list(pairs), safe)
    return fallbacks.querydict_urlencode(pairs, safe)


def url_precheck(value: str, max_length: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.url_precheck(value, max_length)
    return fallbacks.url_precheck(value, max_length)


def salted_hmac_digest(algorithm: str, key_salt: bytes, secret: bytes, value: bytes) -> bytes:
    impl = _impl()
    if impl is not None:
        return impl.salted_hmac_digest(algorithm, key_salt, secret, value)
    return fallbacks.salted_hmac_digest(algorithm, key_salt, secret, value)


def pbkdf2_hmac(algorithm: str, password: bytes, salt: bytes, iterations: int, dklen: int = 0) -> bytes:
    impl = _impl()
    if impl is not None:
        return impl.pbkdf2_hmac(algorithm, password, salt, iterations, dklen)
    return fallbacks.pbkdf2_hmac(algorithm, password, salt, iterations, dklen)


def secure_random_string(length: int, allowed_chars: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.secure_random_string(length, allowed_chars)
    return fallbacks.secure_random_string(length, allowed_chars)


def trim_url(url: str, limit: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.trim_url(url, limit)
    return fallbacks.trim_url(url, limit)


def urlize_word_split(text: str):
    impl = _impl()
    if impl is not None:
        return impl.urlize_word_split(text)
    return fallbacks.urlize_word_split(text)


def urlize_is_email_simple(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.urlize_is_email_simple(value)
    return fallbacks.urlize_is_email_simple(value)


def urlize_simple_url_match(middle: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.urlize_simple_url_match(middle)
    return fallbacks.urlize_simple_url_match(middle)


def urlize_simple_url_2_match(middle: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.urlize_simple_url_2_match(middle)
    return fallbacks.urlize_simple_url_2_match(middle)


def trim_urlize_punctuation(word: str):
    impl = _impl()
    if impl is not None:
        return impl.trim_urlize_punctuation(word)
    return fallbacks.trim_urlize_punctuation(word)


def is_valid_domain_name(value: str, accept_idna: bool = True, max_length: int = 255) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_valid_domain_name(value, accept_idna, max_length)
    return fallbacks.is_valid_domain_name(value, accept_idna, max_length)


def url_structure_precheck(value: str, max_length: int, schemes_csv: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.url_structure_precheck(value, max_length, schemes_csv)
    return fallbacks.url_structure_precheck(value, max_length, schemes_csv)


def truncate_chars_html(text: str, length: int, suffix: str = "…"):
    impl = _impl()
    if impl is not None:
        return impl.truncate_chars_html(text, length, suffix)
    return fallbacks.truncate_chars_html(text, length, suffix)


def truncate_words_html(text: str, length: int, suffix: str = "…"):
    impl = _impl()
    if impl is not None:
        return impl.truncate_words_html(text, length, suffix)
    return fallbacks.truncate_words_html(text, length, suffix)


def decimal_digit_counts(digits: str, exponent: int):
    impl = _impl()
    if impl is not None:
        return impl.decimal_digit_counts(digits, exponent)
    return fallbacks.decimal_digit_counts(digits, exponent)


def sanitize_separators_ascii(value, decimal_sep, thousand_sep, use_thousand):
    impl = _impl()
    if impl is not None:
        return impl.sanitize_separators_ascii(value, decimal_sep, thousand_sep, use_thousand)
    return fallbacks.sanitize_separators_ascii(value, decimal_sep, thousand_sep, use_thousand)


def querydict_urlencode_bytes(pairs, safe: str = "") -> str:
    impl = _impl()
    if impl is not None:
        return impl.querydict_urlencode_bytes(list(pairs), safe)
    return fallbacks.querydict_urlencode_bytes(pairs, safe)


def template_tokenize(source: str, with_position: bool = False):
    impl = _impl()
    if impl is not None:
        return impl.template_tokenize(source, with_position)
    return fallbacks.template_tokenize(source, with_position)


def find_multipart_boundary(data: bytes, boundary: bytes):
    impl = _impl()
    if impl is not None:
        return impl.find_multipart_boundary(data, boundary)
    return fallbacks.find_multipart_boundary(data, boundary)


def boundary_chunk_slice(data: bytes, boundary: bytes, rollback: int):
    impl = _impl()
    if impl is not None:
        return impl.boundary_chunk_slice(data, boundary, rollback)
    return fallbacks.boundary_chunk_slice(data, boundary, rollback)


def find_header_block_end(chunk: bytes):
    impl = _impl()
    if impl is not None:
        return impl.find_header_block_end(chunk)
    return fallbacks.find_header_block_end(chunk)


def sanitize_multipart_filename(file_name: str) -> str | None:
    impl = _impl()
    if impl is not None:
        return impl.sanitize_multipart_filename(file_name)
    return fallbacks.sanitize_multipart_filename(file_name)


def split_multipart_parts(body: bytes, separator: bytes):
    impl = _impl()
    if impl is not None:
        return impl.split_multipart_parts(body, separator)
    return fallbacks.split_multipart_parts(body, separator)


def parse_header_parameters(line: str, max_length: int | None = 10_000):
    impl = _impl()
    if impl is not None:
        return impl.parse_header_parameters(line, max_length)
    return fallbacks.parse_header_parameters(line, max_length)


def parse_multipart_headers(header_block: bytes):
    impl = _impl()
    if impl is not None:
        return impl.parse_multipart_headers(header_block)
    return fallbacks.parse_multipart_headers(header_block)


def parse_multipart_message(body: bytes, boundary: bytes):
    impl = _impl()
    if impl is not None:
        return impl.parse_multipart_message(body, boundary)
    return fallbacks.parse_multipart_message(body, boundary)


def smart_split(text: str):
    impl = _impl()
    if impl is not None:
        return impl.smart_split(text)
    return fallbacks.smart_split(text)


def unescape_string_literal(s: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.unescape_string_literal(s)
    return fallbacks.unescape_string_literal(s)


def parse_variable(var: str):
    impl = _impl()
    if impl is not None:
        return impl.parse_variable(var)
    return fallbacks.parse_variable(var)


def parse_filter_expression(token: str):
    impl = _impl()
    if impl is not None:
        return impl.parse_filter_expression(token)
    return fallbacks.parse_filter_expression(token)


def resolve_dict_lookups(context, lookups):
    impl = _impl()
    if impl is not None:
        return impl.resolve_dict_lookups(context, lookups)
    return fallbacks.resolve_dict_lookups(context, lookups)


def filter_addslashes(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.filter_addslashes(value)
    return fallbacks.filter_addslashes(value)


def filter_capfirst(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.filter_capfirst(value)
    return fallbacks.filter_capfirst(value)


def filter_lower(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.filter_lower(value)
    return fallbacks.filter_lower(value)


def filter_upper(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.filter_upper(value)
    return fallbacks.filter_upper(value)


def filter_cut(value: str, arg: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.filter_cut(value, arg)
    return fallbacks.filter_cut(value, arg)


def filter_wordcount(value: str) -> int:
    impl = _impl()
    if impl is not None:
        return impl.filter_wordcount(value)
    return fallbacks.filter_wordcount(value)


def filter_ljust(value: str, width: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.filter_ljust(value, width)
    return fallbacks.filter_ljust(value, width)


def filter_rjust(value: str, width: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.filter_rjust(value, width)
    return fallbacks.filter_rjust(value, width)


def filter_center(value: str, width: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.filter_center(value, width)
    return fallbacks.filter_center(value, width)


def url_quote(value: str, safe: str = "") -> str:
    impl = _impl()
    if impl is not None:
        return impl.url_quote(value, safe)
    return fallbacks.url_quote(value, safe)


def escape_leading_slashes(url: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.escape_leading_slashes(url)
    return fallbacks.escape_leading_slashes(url)


def context_lookup(dicts, key):
    impl = _impl()
    if impl is not None:
        return impl.context_lookup(dicts, key)
    return fallbacks.context_lookup(dicts, key)


def phone2numeric(phone: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.phone2numeric(phone)
    return fallbacks.phone2numeric(phone)


def normalize_newlines(text: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.normalize_newlines(text)
    return fallbacks.normalize_newlines(text)


def parse_cookie(cookie: str) -> dict:
    impl = _impl()
    if impl is not None:
        return impl.parse_cookie(cookie)
    return fallbacks.parse_cookie(cookie)


def cookie_unquote(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.cookie_unquote(value)
    return fallbacks.cookie_unquote(value)


def parse_http_date(date: str, current_year: int | None = None) -> int:
    impl = _impl()
    if impl is not None:
        if current_year is None:
            from datetime import UTC, datetime

            current_year = datetime.now(tz=UTC).year
        return impl.parse_http_date(date, current_year)
    return fallbacks.parse_http_date(date)


def http_date(epoch_seconds=None) -> str:
    impl = _impl()
    if impl is not None:
        return impl.http_date(epoch_seconds)
    return fallbacks.http_date(epoch_seconds)


def base36_to_int(s: str) -> int:
    impl = _impl()
    if impl is not None:
        return impl.base36_to_int(s)
    return fallbacks.base36_to_int(s)


def int_to_base36(i: int) -> str:
    impl = _impl()
    if impl is not None and 0 <= i <= 0xFFFFFFFFFFFFFFFF:
        return impl.int_to_base36(i)
    return fallbacks.int_to_base36(i)


def parse_etags(etag_str: str):
    impl = _impl()
    if impl is not None:
        return impl.parse_etags(etag_str)
    return fallbacks.parse_etags(etag_str)


def quote_etag(etag_str: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.quote_etag(etag_str)
    return fallbacks.quote_etag(etag_str)


def is_same_domain(host: str, pattern: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_same_domain(host, pattern)
    return fallbacks.is_same_domain(host, pattern)


def split_domain_port(host: str):
    impl = _impl()
    if impl is not None:
        return impl.split_domain_port(host)
    return fallbacks.split_domain_port(host)


def validate_host(host: str, allowed_hosts) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.validate_host(host, list(allowed_hosts))
    return fallbacks.validate_host(host, allowed_hosts)


def content_disposition_header(as_attachment: bool, filename=None):
    impl = _impl()
    if impl is not None:
        return impl.content_disposition_header(as_attachment, filename)
    return fallbacks.content_disposition_header(as_attachment, filename)


def urlsafe_base64_encode(s: bytes) -> str:
    impl = _impl()
    if impl is not None:
        return impl.urlsafe_base64_encode(s)
    return fallbacks.urlsafe_base64_encode(s)


def urlsafe_base64_decode(s: str) -> bytes:
    impl = _impl()
    if impl is not None:
        return impl.urlsafe_base64_decode(s)
    return fallbacks.urlsafe_base64_decode(s)


def strip_spaces_between_tags(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.strip_spaces_between_tags(value)
    return fallbacks.strip_spaces_between_tags(value)


def camel_case_to_spaces(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.camel_case_to_spaces(value)
    return fallbacks.camel_case_to_spaces(value)


def pluralize_suffix(singular: bool, arg: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.pluralize_suffix(singular, arg)
    return fallbacks.pluralize_suffix(singular, arg)


def yesno(tri_state: int, arg: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.yesno(tri_state, arg)
    return fallbacks.yesno(tri_state, arg)


def get_digit(value, arg):
    impl = _impl()
    if impl is not None:
        return impl.get_digit(value, arg)
    return fallbacks.get_digit(value, arg)


def widthratio(value: float, max_value: float, max_width: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.widthratio(value, max_value, max_width)
    return fallbacks.widthratio(value, max_value, max_width)


def get_mod_func(callback: str):
    impl = _impl()
    if impl is not None:
        return impl.get_mod_func(callback)
    return fallbacks.get_mod_func(callback)


def iri_to_uri(iri):
    impl = _impl()
    if impl is not None:
        return impl.iri_to_uri(iri)
    return fallbacks.iri_to_uri(iri)


def uri_to_iri(uri):
    impl = _impl()
    if impl is not None:
        return impl.uri_to_iri(uri)
    return fallbacks.uri_to_iri(uri)


def escape_uri_path(path: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.escape_uri_path(path)
    return fallbacks.escape_uri_path(path)


def filepath_to_uri(path):
    impl = _impl()
    if impl is not None:
        return impl.filepath_to_uri(path)
    return fallbacks.filepath_to_uri(path)


def filter_title(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.filter_title(value)
    return fallbacks.filter_title(value)


def filter_slice_string(value: str, start=None, stop=None, step=None) -> str:
    impl = _impl()
    if impl is not None:
        return impl.filter_slice_string(value, start, stop, step)
    return fallbacks.filter_slice_string(value, start, stop, step)


def divisibleby(value, arg) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.divisibleby(value, arg)
    return fallbacks.divisibleby(value, arg)


def filter_add_int(value, arg):
    impl = _impl()
    if impl is not None:
        return impl.filter_add_int(value, arg)
    return fallbacks.filter_add_int(value, arg)


def linebreaks(value: str, autoescape: bool = False) -> str:
    impl = _impl()
    if impl is not None:
        return impl.linebreaks(value, autoescape)
    return fallbacks.linebreaks(value, autoescape)


def linebreaksbr(value: str, autoescape: bool = False) -> str:
    impl = _impl()
    if impl is not None:
        return impl.linebreaksbr(value, autoescape)
    return fallbacks.linebreaksbr(value, autoescape)


def strip_tags(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.strip_tags(value)
    return fallbacks.strip_tags(value)


def utf8_length(value: str) -> int:
    impl = _impl()
    if impl is not None:
        return impl.utf8_length(value)
    return fallbacks.utf8_length(value)


def utf8_first(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.utf8_first(value)
    return fallbacks.utf8_first(value)


def utf8_last(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.utf8_last(value)
    return fallbacks.utf8_last(value)


def make_list_chars(value: str) -> list:
    impl = _impl()
    if impl is not None:
        return impl.make_list_chars(value)
    return fallbacks.make_list_chars(value)


def linenumbers(value: str, autoescape: bool = True) -> str:
    impl = _impl()
    if impl is not None:
        return impl.linenumbers(value, autoescape)
    return fallbacks.linenumbers(value, autoescape)


def wordwrap(text: str, width: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.wordwrap(text, width)
    return fallbacks.wordwrap(text, width)


def join_strings(parts, sep: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.join_strings(list(parts), sep)
    return fallbacks.join_strings(parts, sep)


def filter_default(value, arg):
    impl = _impl()
    if impl is not None:
        return impl.filter_default(value, arg)
    return fallbacks.filter_default(value, arg)


def filter_default_if_none(value, arg):
    impl = _impl()
    if impl is not None:
        return impl.filter_default_if_none(value, arg)
    return fallbacks.filter_default_if_none(value, arg)


def sequence_random(value):
    impl = _impl()
    if impl is not None:
        return impl.sequence_random(value)
    return fallbacks.sequence_random(value)


def dictsort(value, arg, reverse: bool = False):
    impl = _impl()
    if impl is not None:
        return impl.dictsort(value, arg, reverse)
    return fallbacks.dictsort(value, arg, reverse)


def unordered_list(value, autoescape: bool = True) -> str:
    impl = _impl()
    if impl is not None:
        return impl.unordered_list(value, autoescape)
    return fallbacks.unordered_list(value, autoescape)


def format_number(
    number: str,
    decimal_sep: str,
    decimal_pos=None,
    grouping=0,
    thousand_sep: str = "",
    use_grouping: bool = False,
) -> str:
    impl = _impl()
    if impl is not None:
        return impl.format_number(
            number, decimal_sep, decimal_pos, grouping, thousand_sep, use_grouping
        )
    return fallbacks.format_number(
        number, decimal_sep, decimal_pos, grouping, thousand_sep, use_grouping
    )


def php_date_format(parts: dict, format_string: str):
    impl = _impl()
    if impl is not None:
        return impl.php_date_format(parts, format_string)
    return fallbacks.php_date_format(parts, format_string)


def timesince_partials(d, now, depth: int = 2):
    impl = _impl()
    if impl is not None:
        return impl.timesince_partials(d, now, depth)
    return fallbacks.timesince_partials(d, now, depth)


def avoid_wrapping(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.avoid_wrapping(value)
    return fallbacks.avoid_wrapping(value)


def filesize_parts(bytes_):
    impl = _impl()
    if impl is not None:
        return impl.filesize_parts(bytes_)
    return fallbacks.filesize_parts(bytes_)

# --- cache / conditional / Vary ----------------------------------------------

def cc_delim_split(header: str):
    impl = _impl()
    if impl is not None:
        return impl.cc_delim_split(header)
    return fallbacks.cc_delim_split(header)


def parse_cache_control(header: str):
    impl = _impl()
    if impl is not None:
        return impl.parse_cache_control(header)
    return fallbacks.parse_cache_control(header)


def get_max_age_from_cc(header: str):
    impl = _impl()
    if impl is not None:
        return impl.get_max_age_from_cc(header)
    return fallbacks.get_max_age_from_cc(header)


def if_match_passes(target_etag, etags) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.if_match_passes(target_etag or "", list(etags))
    return fallbacks.if_match_passes(target_etag, etags)


def if_none_match_passes(target_etag, etags) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.if_none_match_passes(target_etag or "", list(etags))
    return fallbacks.if_none_match_passes(target_etag, etags)


def if_unmodified_since_passes(last_modified, if_unmodified_since) -> bool:
    # None last_modified cannot pass (must have a value ≤ since).
    if last_modified is None:
        return False
    impl = _impl()
    if impl is not None:
        return impl.if_unmodified_since_passes(last_modified, if_unmodified_since)
    return fallbacks.if_unmodified_since_passes(last_modified, if_unmodified_since)


def if_modified_since_passes(last_modified, if_modified_since) -> bool:
    # Missing Last-Modified means the resource is considered modified.
    if last_modified is None:
        return True
    impl = _impl()
    if impl is not None:
        return impl.if_modified_since_passes(last_modified, if_modified_since)
    return fallbacks.if_modified_since_passes(last_modified, if_modified_since)


def patch_vary_headers_value(existing_vary: str, newheaders) -> str:
    impl = _impl()
    if impl is not None:
        return impl.patch_vary_headers(existing_vary or "", list(newheaders))
    return fallbacks.patch_vary_headers_value(existing_vary, newheaders)


def has_vary_header_value(vary_header: str, header_query: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.has_vary_header(vary_header or "", header_query)
    return fallbacks.has_vary_header_value(vary_header, header_query)


def merge_cache_control(existing: str, kwargs_triples) -> str:
    """kwargs_triples: iterable of (name, value_str, is_bool_true)."""
    impl = _impl()
    if impl is not None:
        return impl.merge_cache_control(existing or "", list(kwargs_triples))
    return fallbacks.merge_cache_control(existing, kwargs_triples)


# --- datastructures / forms / template ---------------------------------------

def mvd_last_value(values):
    impl = _impl()
    if impl is not None:
        # Only accelerate homogeneous str lists; else Python.
        try:
            strs = [str(v) for v in values]
        except Exception:
            return fallbacks.mvd_last_value(values)
        return impl.mvd_last_value(strs)
    return fallbacks.mvd_last_value(values)


def node_add_action(self_connector, conn_type, data_is_node, data_negated,
                    data_connector, data_len) -> int:
    impl = _impl()
    if impl is not None:
        return impl.node_add_action(
            self_connector, conn_type, data_is_node, data_negated,
            data_connector or "", data_len,
        )
    return fallbacks.node_add_action(
        self_connector, conn_type, data_is_node, data_negated,
        data_connector, data_len,
    )


def form_add_prefix(prefix, field_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.form_add_prefix(prefix or "", field_name)
    return fallbacks.form_add_prefix(prefix, field_name)


def form_add_initial_prefix(prefix, field_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.form_add_initial_prefix(prefix or "", field_name)
    return fallbacks.form_add_initial_prefix(prefix, field_name)


def pretty_name(name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.pretty_name(name or "")
    return fallbacks.pretty_name(name)


def form_auto_id(auto_id, html_name: str) -> str:
    impl = _impl()
    if impl is not None and isinstance(auto_id, str):
        return impl.form_auto_id(auto_id, html_name)
    return fallbacks.form_auto_id(auto_id, html_name)


def checkbox_bool_value(key_present: bool, value: str = "") -> bool:
    impl = _impl()
    if impl is not None:
        return impl.checkbox_bool_value(key_present, value if value is not None else "")
    return fallbacks.checkbox_bool_value(key_present, value)


def flatatt_build(key_values, boolean_keys) -> str:
    impl = _impl()
    if impl is not None:
        return impl.flatatt_build(list(key_values), list(boolean_keys))
    return fallbacks.flatatt_build(key_values, boolean_keys)


def json_script_escape(json_str: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.json_script_escape(json_str)
    return fallbacks.json_script_escape(json_str)


def json_script_wrap(escaped_json: str, element_id: str = "") -> str:
    impl = _impl()
    if impl is not None:
        return impl.json_script_wrap(escaped_json, element_id or "")
    return fallbacks.json_script_wrap(escaped_json, element_id)


def floatformat_simple(decimal_str: str, p: int):
    impl = _impl()
    if impl is not None:
        return impl.floatformat_simple(decimal_str, p)
    return fallbacks.floatformat_simple(decimal_str, p)

# --- ORM / forms / response / template (workstreams 1-4) ---------------------

def sql_quote_name(name: str, style: str = "double") -> str:
    # Collations and other identifiers may be SimpleLazyObject / Promise.
    name = str(name)
    impl = _impl()
    if impl is not None:
        return impl.sql_quote_name(name, style)
    return fallbacks.sql_quote_name(name, style)


def where_needed_counts(connector: str, n_children: int):
    impl = _impl()
    if impl is not None:
        return impl.where_needed_counts(connector, n_children)
    return fallbacks.where_needed_counts(connector, n_children)


def where_combine_sql(connector: str, parts, negated: bool = False, resolved: bool = False) -> str:
    impl = _impl()
    if impl is not None:
        return impl.where_combine_sql(connector, list(parts), negated, resolved)
    return fallbacks.where_combine_sql(connector, parts, negated, resolved)


def sql_in_placeholders(n: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_in_placeholders(n)
    return fallbacks.sql_in_placeholders(n)


def sql_isnull_sql(negated: bool = False) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_isnull_sql(negated)
    return fallbacks.sql_isnull_sql(negated)


def sql_comparison_rhs(lookup_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_comparison_rhs(lookup_name)
    return fallbacks.sql_comparison_rhs(lookup_name)


def is_form_empty_string(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_form_empty_string(value)
    return fallbacks.is_form_empty_string(value)


def field_str_has_changed(initial: str, data: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.field_str_has_changed(initial, data)
    return fallbacks.field_str_has_changed(initial, data)


def boolean_field_to_python(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.boolean_field_to_python(value)
    return fallbacks.boolean_field_to_python(value)


def null_boolean_to_python(value: str):
    impl = _impl()
    if impl is not None:
        return impl.null_boolean_to_python(value)
    return fallbacks.null_boolean_to_python(value)


def header_key_valid(key: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.header_key_valid(key)
    return fallbacks.header_key_valid(key)


def header_value_no_newlines(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.header_value_no_newlines(value)
    return fallbacks.header_value_no_newlines(value)


def charset_from_content_type(content_type: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.charset_from_content_type(content_type)
    return fallbacks.charset_from_content_type(content_type)


def path_ends_with_slash(path: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.path_ends_with_slash(path)
    return fallbacks.path_ends_with_slash(path)


def force_append_slash_path(full_path: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.force_append_slash_path(full_path)
    return fallbacks.force_append_slash_path(full_path)


def serialize_header_lines(headers) -> list:
    impl = _impl()
    if impl is not None:
        return impl.serialize_header_lines(list(headers))
    return fallbacks.serialize_header_lines(headers)


def stringformat_simple(value: str, spec: str):
    impl = _impl()
    if impl is not None:
        return impl.stringformat_simple(value, spec)
    return fallbacks.stringformat_simple(value, spec)


def floatformat_ascii(decimal_str: str, p: int):
    impl = _impl()
    if impl is not None:
        return impl.floatformat_ascii(decimal_str, p)
    return fallbacks.floatformat_ascii(decimal_str, p)

# --- workstreams 1-5 extensions ----------------------------------------------

def sql_join_dotted(parts) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_join_dotted(list(parts))
    return fallbacks.sql_join_dotted(parts)


def sql_pattern_wrap(value: str, kind: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_pattern_wrap(value, kind)
    return fallbacks.sql_pattern_wrap(value, kind)


def choice_valid_value(text_value: str, choice_keys) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.choice_valid_value(text_value, [str(k) for k in choice_keys])
    return fallbacks.choice_valid_value(text_value, choice_keys)


def is_decimal_string(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_decimal_string(value)
    return fallbacks.is_decimal_string(value)


def form_float_to_python(value: str):
    impl = _impl()
    if impl is not None:
        return impl.form_float_to_python(value)
    return fallbacks.form_float_to_python(value)


def is_valid_session_key(key: str, min_length: int = 8, check_charset: bool = False) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_valid_session_key(key or "", min_length, check_charset)
    return fallbacks.is_valid_session_key(key, min_length, check_charset)


def is_valid_samesite(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_valid_samesite(value or "")
    return fallbacks.is_valid_samesite(value)


def cookie_delete_secure(key: str, samesite: str = "") -> bool:
    impl = _impl()
    if impl is not None:
        return impl.cookie_delete_secure(key or "", samesite or "")
    return fallbacks.cookie_delete_secure(key, samesite)


def cookie_max_age_seconds(total_seconds: float) -> int:
    impl = _impl()
    if impl is not None:
        return impl.cookie_max_age_seconds(total_seconds)
    return fallbacks.cookie_max_age_seconds(total_seconds)


def signing_split(signed_value: str, sep: str = ":"):
    impl = _impl()
    if impl is not None:
        return impl.signing_split(signed_value, sep)
    return fallbacks.signing_split(signed_value, sep)


def signing_is_compressed(b64_payload: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.signing_is_compressed(b64_payload)
    return fallbacks.signing_is_compressed(b64_payload)


def where_child_outcome(child_kind: int, negated: bool, full_needed: int, empty_needed: int):
    impl = _impl()
    if impl is not None:
        return impl.where_child_outcome(child_kind, negated, full_needed, empty_needed)
    return fallbacks.where_child_outcome(child_kind, negated, full_needed, empty_needed)

# --- Query / SQLCompiler depth -----------------------------------------------

def sql_comma_join(parts) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_comma_join(list(parts))
    return fallbacks.sql_comma_join(parts)


def sql_order_by_clause(parts) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_order_by_clause(list(parts))
    return fallbacks.sql_order_by_clause(parts)


def sql_group_by_clause(parts) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_group_by_clause(list(parts))
    return fallbacks.sql_group_by_clause(parts)


def sql_expr_as(expr_sql: str, quoted_alias: str = "") -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_expr_as(expr_sql, quoted_alias or "")
    return fallbacks.sql_expr_as(expr_sql, quoted_alias)


def sql_limit_offset_clause(limit, offset: int = 0) -> str:
    impl = _impl()
    if impl is not None:
        # nanobind may not accept None for object in all builds; use sentinel path.
        if limit is None:
            # only OFFSET (or empty)
            if offset:
                return f"OFFSET {int(offset)}"
            return ""
        return impl.sql_limit_offset_clause(int(limit), int(offset or 0))
    return fallbacks.sql_limit_offset_clause(limit, offset)


def join_promoter_effective_connector(connector: str, negated: bool) -> str:
    impl = _impl()
    if impl is not None:
        return impl.join_promoter_effective_connector(connector, negated)
    return fallbacks.join_promoter_effective_connector(connector, negated)


def join_promoter_should_promote(effective_connector: str, votes: int, num_children: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.join_promoter_should_promote(effective_connector, votes, num_children)
    return fallbacks.join_promoter_should_promote(effective_connector, votes, num_children)


def join_promoter_should_demote(effective_connector: str, votes: int, num_children: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.join_promoter_should_demote(effective_connector, votes, num_children)
    return fallbacks.join_promoter_should_demote(effective_connector, votes, num_children)


def quote_name_is_alias(in_alias_map_not_table: bool, in_extra_select: bool,
                        external_alias_not_table: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.quote_name_is_alias(
            in_alias_map_not_table, in_extra_select, external_alias_not_table
        )
    return fallbacks.quote_name_is_alias(
        in_alias_map_not_table, in_extra_select, external_alias_not_table
    )


def q_is_empty(n_children: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.q_is_empty(n_children)
    return fallbacks.q_is_empty(n_children)


def split_lookup_path(path: str):
    impl = _impl()
    if impl is not None:
        return impl.split_lookup_path(path)
    return fallbacks.split_lookup_path(path)


def lookup_path_head(path: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.lookup_path_head(path)
    return fallbacks.lookup_path_head(path)


def q_combine_empty_flags(self_empty: bool, other_empty: bool) -> int:
    impl = _impl()
    if impl is not None:
        return impl.q_combine_empty_flags(self_empty, other_empty)
    return fallbacks.q_combine_empty_flags(self_empty, other_empty)

# --- build_filter / lookup path resolution -----------------------------------

def join_lookup_path(parts) -> str:
    impl = _impl()
    if impl is not None:
        return impl.join_lookup_path(list(parts))
    return fallbacks.join_lookup_path(parts)


def lookup_field_parts(lookup_splitted, n_lookup_parts: int):
    impl = _impl()
    if impl is not None:
        return impl.lookup_field_parts(list(lookup_splitted), n_lookup_parts)
    return fallbacks.lookup_field_parts(lookup_splitted, n_lookup_parts)


def lookup_or_exact(lookups):
    impl = _impl()
    if impl is not None:
        return impl.lookup_or_exact(list(lookups) if lookups else [])
    return fallbacks.lookup_or_exact(lookups)


def refs_expression_match(lookup_parts, annotation_keys):
    impl = _impl()
    if impl is not None:
        return impl.refs_expression_match(list(lookup_parts), list(annotation_keys))
    return fallbacks.refs_expression_match(lookup_parts, annotation_keys)


def next_numbered_alias(prefix: str, alias_map_size: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.next_numbered_alias(prefix, alias_map_size)
    return fallbacks.next_numbered_alias(prefix, alias_map_size)


def alias_refcount_add(current: int, amount: int, clamp_non_negative: bool = False) -> int:
    impl = _impl()
    if impl is not None:
        return impl.alias_refcount_add(current, amount, clamp_non_negative)
    return fallbacks.alias_refcount_add(current, amount, clamp_non_negative)


def alias_refcount_increased(pre: dict, post: dict):
    impl = _impl()
    if impl is not None:
        return impl.alias_refcount_increased(dict(pre), dict(post))
    return fallbacks.alias_refcount_increased(pre, post)


def lookup_invalid_without_field(n_lookup_parts: int, n_field_parts: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.lookup_invalid_without_field(n_lookup_parts, n_field_parts)
    return fallbacks.lookup_invalid_without_field(n_lookup_parts, n_field_parts)


def split_order_by_item(item: str):
    impl = _impl()
    if impl is not None:
        return impl.split_order_by_item(item)
    return fallbacks.split_order_by_item(item)

# --- QuerySet / sessions / forms / compiler (1-4) ----------------------------

def values_list_flags(flat: bool, named: bool, n_fields: int) -> int:
    impl = _impl()
    if impl is not None:
        return impl.values_list_flags(flat, named, n_fields)
    return fallbacks.values_list_flags(flat, named, n_fields)


def unique_field_alias(base: str, start_counter: int, existing_keys) -> str:
    impl = _impl()
    if impl is not None:
        return impl.unique_field_alias(base, start_counter, list(existing_keys))
    return fallbacks.unique_field_alias(base, start_counter, existing_keys)


def session_cache_key(prefix: str, session_key: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.session_cache_key(prefix, session_key)
    return fallbacks.session_cache_key(prefix, session_key)


def session_expiry_age_seconds(cookie_age: int, modification_age=None, expiry=None) -> int:
    """Match SessionBase.get_expiry_age for None/0/int expiry (not datetime)."""
    impl = _impl()
    if impl is not None:
        if expiry is None or expiry == 0:
            return impl.session_expiry_age_seconds(cookie_age, None, None)
        return impl.session_expiry_age_seconds(cookie_age, modification_age, int(expiry))
    return fallbacks.session_expiry_age_seconds(cookie_age, modification_age, expiry)


def session_delta_seconds(days: int, seconds: int) -> int:
    impl = _impl()
    if impl is not None:
        return impl.session_delta_seconds(days, seconds)
    return fallbacks.session_delta_seconds(days, seconds)


def session_key_missing(session_key: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.session_key_missing(session_key or "")
    return fallbacks.session_key_missing(session_key)


def sql_for_update(no_key=False, nowait=False, skip_locked=False, of=None) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_for_update(no_key, nowait, skip_locked, list(of or ()))
    return fallbacks.sql_for_update(no_key, nowait, skip_locked, of)


def sql_combinator_keyword(combinator: str, all: bool = False) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_combinator_keyword(combinator, all)
    return fallbacks.sql_combinator_keyword(combinator, all)


def sql_combinator_join(combinator_sql: str, parts, wrap_parens: bool = False) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_combinator_join(combinator_sql, list(parts), wrap_parens)
    return fallbacks.sql_combinator_join(combinator_sql, parts, wrap_parens)


def sql_distinct_clause(fields, allow_on: bool = False) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_distinct_clause(list(fields), allow_on)
    return fallbacks.sql_distinct_clause(fields, allow_on)


def multi_choice_has_changed(initial, data) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.multi_choice_has_changed([str(x) for x in initial], [str(x) for x in data])
    return fallbacks.multi_choice_has_changed(initial, data)


def json_looks_valid(value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.json_looks_valid(value)
    return fallbacks.json_looks_valid(value)


def sql_from_tables(clauses) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_from_tables(list(clauses))
    return fallbacks.sql_from_tables(clauses)

# --- QuerySet surface (#1) ---------------------------------------------------

def queryset_count_from_cache(has_cache: bool, cache_len: int) -> int:
    impl = _impl()
    if impl is not None:
        return impl.queryset_count_from_cache(has_cache, cache_len)
    return fallbacks.queryset_count_from_cache(has_cache, cache_len)


def queryset_exists_from_cache(has_cache: bool, cache_nonempty: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.queryset_exists_from_cache(has_cache, cache_nonempty)
    return fallbacks.queryset_exists_from_cache(has_cache, cache_nonempty)


def queryset_use_cache_for_first_last(has_cache: bool, ordered: bool, cache_nonempty: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.queryset_use_cache_for_first_last(has_cache, ordered, cache_nonempty)
    return fallbacks.queryset_use_cache_for_first_last(has_cache, ordered, cache_nonempty)


def iterator_chunk_validate(chunk_size_none: bool, chunk_size: int, has_prefetch: bool) -> int:
    impl = _impl()
    if impl is not None:
        return impl.iterator_chunk_validate(chunk_size_none, chunk_size, has_prefetch)
    return fallbacks.iterator_chunk_validate(chunk_size_none, chunk_size, has_prefetch)


def iterator_chunk_size_or_default(chunk_size_none: bool, chunk_size: int, default_size: int = 2000) -> int:
    impl = _impl()
    if impl is not None:
        return impl.iterator_chunk_size_or_default(chunk_size_none, chunk_size, default_size)
    return fallbacks.iterator_chunk_size_or_default(chunk_size_none, chunk_size, default_size)


def in_bulk_empty(id_list_is_none: bool, id_list_len: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.in_bulk_empty(id_list_is_none, id_list_len)
    return fallbacks.in_bulk_empty(id_list_is_none, id_list_len)


def in_bulk_filter_key(field_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.in_bulk_filter_key(field_name)
    return fallbacks.in_bulk_filter_key(field_name)


def in_bulk_batch_ranges(n_ids: int, batch_size: int):
    impl = _impl()
    if impl is not None:
        return impl.in_bulk_batch_ranges(n_ids, batch_size)
    return fallbacks.in_bulk_batch_ranges(n_ids, batch_size)


def get_result_kind(num_results: int, limit: int = 0) -> int:
    impl = _impl()
    if impl is not None:
        return impl.get_result_kind(num_results, limit)
    return fallbacks.get_result_kind(num_results, limit)

# --- bulk SQL / QuerySet write guards ----------------------------------------

def bulk_insert_sql(row_sqls) -> str:
    impl = _impl()
    if impl is not None:
        return impl.bulk_insert_sql(list(row_sqls))
    return fallbacks.bulk_insert_sql(row_sqls)


def bulk_placeholder_row(cols) -> str:
    impl = _impl()
    if impl is not None:
        return impl.bulk_placeholder_row(list(cols))
    return fallbacks.bulk_placeholder_row(cols)


def validate_positive_batch_size(is_none: bool, batch_size: int = 0) -> int:
    impl = _impl()
    if impl is not None:
        return impl.validate_positive_batch_size(is_none, batch_size)
    return fallbacks.validate_positive_batch_size(is_none, batch_size)


def effective_batch_size(user_set: bool, user_batch: int, max_batch: int, n_objs: int) -> int:
    impl = _impl()
    if impl is not None:
        return impl.effective_batch_size(user_set, user_batch, max_batch, n_objs)
    return fallbacks.effective_batch_size(user_set, user_batch, max_batch, n_objs)


def queryset_write_guard(combinator: bool, is_sliced: bool, has_distinct_fields: bool,
                         has_values_fields: bool) -> int:
    impl = _impl()
    if impl is not None:
        return impl.queryset_write_guard(
            combinator, is_sliced, has_distinct_fields, has_values_fields
        )
    return fallbacks.queryset_write_guard(
        combinator, is_sliced, has_distinct_fields, has_values_fields
    )


def sql_update_set_clause(assignments) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_update_set_clause(list(assignments))
    return fallbacks.sql_update_set_clause(assignments)


def multi_batch_needs_atomic(n_batches: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.multi_batch_needs_atomic(n_batches)
    return fallbacks.multi_batch_needs_atomic(n_batches)


def key_has_lookup_sep(key: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.key_has_lookup_sep(key)
    return fallbacks.key_has_lookup_sep(key)


def keys_without_lookup_sep(keys):
    impl = _impl()
    if impl is not None:
        # Native already returns a Python list (built in C++).
        return impl.keys_without_lookup_sep(list(keys))
    return fallbacks.keys_without_lookup_sep(keys)


def create_defaults_use_update(create_defaults_is_none: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.create_defaults_use_update(create_defaults_is_none)
    return fallbacks.create_defaults_use_update(create_defaults_is_none)


def join_sorted_comma(names) -> str:
    impl = _impl()
    if impl is not None:
        return impl.join_sorted_comma(list(names))
    return fallbacks.join_sorted_comma(names)


def bulk_create_conflict_kind(ignore_conflicts: bool, update_conflicts: bool) -> int:
    impl = _impl()
    if impl is not None:
        return impl.bulk_create_conflict_kind(ignore_conflicts, update_conflicts)
    return fallbacks.bulk_create_conflict_kind(ignore_conflicts, update_conflicts)


def contains_preflight(has_values_fields: bool, pk_set: bool) -> int:
    impl = _impl()
    if impl is not None:
        return impl.contains_preflight(has_values_fields, pk_set)
    return fallbacks.contains_preflight(has_values_fields, pk_set)


def aggregate_distinct_fields_error(has_distinct_fields: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.aggregate_distinct_fields_error(has_distinct_fields)
    return fallbacks.aggregate_distinct_fields_error(has_distinct_fields)


def filter_after_slice_error(has_filters: bool, is_sliced: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.filter_after_slice_error(has_filters, is_sliced)
    return fallbacks.filter_after_slice_error(has_filters, is_sliced)


def prohibited_filter_kwargs(keys):
    impl = _impl()
    if impl is not None:
        # Native already returns a Python list (built in C++).
        return impl.prohibited_filter_kwargs(list(keys))
    return fallbacks.prohibited_filter_kwargs(keys)


def select_for_update_options_conflict(nowait: bool, skip_locked: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.select_for_update_options_conflict(nowait, skip_locked)
    return fallbacks.select_for_update_options_conflict(nowait, skip_locked)


def union_empty_self_kind(nonempty_other_count: int) -> int:
    impl = _impl()
    if impl is not None:
        return impl.union_empty_self_kind(nonempty_other_count)
    return fallbacks.union_empty_self_kind(nonempty_other_count)


def combinator_return_empty_self(self_is_empty: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.combinator_return_empty_self(self_is_empty)
    return fallbacks.combinator_return_empty_self(self_is_empty)


def save_force_conflict(
    force_insert: bool, force_update: bool, has_update_fields: bool
) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.save_force_conflict(force_insert, force_update, has_update_fields)
    return fallbacks.save_force_conflict(force_insert, force_update, has_update_fields)


def save_skip_empty_update_fields(
    update_fields_is_none: bool, n_update_fields: int
) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.save_skip_empty_update_fields(
            update_fields_is_none, n_update_fields
        )
    return fallbacks.save_skip_empty_update_fields(
        update_fields_is_none, n_update_fields
    )


def save_force_update_no_pk(
    pk_set: bool, force_update: bool, has_update_fields: bool
) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.save_force_update_no_pk(pk_set, force_update, has_update_fields)
    return fallbacks.save_force_update_no_pk(pk_set, force_update, has_update_fields)


def collector_add_empty(n_objs: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.collector_add_empty(n_objs)
    return fallbacks.collector_add_empty(n_objs)


def collector_delete_empty(n_models: int, n_fast_deletes: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.collector_delete_empty(n_models, n_fast_deletes)
    return fallbacks.collector_delete_empty(n_models, n_fast_deletes)


def collector_single_fast_path(n_models: int, n_instances: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.collector_single_fast_path(n_models, n_instances)
    return fallbacks.collector_single_fast_path(n_models, n_instances)


def can_fast_delete_result(
    from_field_blocks: bool,
    model_ok: bool,
    has_signal_listeners: bool,
    parents_ok: bool,
    relations_ok: bool,
    no_bulk_related: bool,
) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.can_fast_delete_result(
            from_field_blocks,
            model_ok,
            has_signal_listeners,
            parents_ok,
            relations_ok,
            no_bulk_related,
        )
    return fallbacks.can_fast_delete_result(
        from_field_blocks,
        model_ok,
        has_signal_listeners,
        parents_ok,
        relations_ok,
        no_bulk_related,
    )


def sql_assignment(quoted_col: str, rhs: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_assignment(quoted_col, rhs)
    return fallbacks.sql_assignment(quoted_col, rhs)


def sql_null_assignment(quoted_col: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_null_assignment(quoted_col)
    return fallbacks.sql_null_assignment(quoted_col)


def sql_parenthesized_list(cols) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_parenthesized_list(list(cols))
    return fallbacks.sql_parenthesized_list(cols)


def sql_values_row(placeholders: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_values_row(placeholders)
    return fallbacks.sql_values_row(placeholders)


def sql_aggregate_subquery(select_sql: str, inner_sql: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_aggregate_subquery(select_sql, inner_sql)
    return fallbacks.sql_aggregate_subquery(select_sql, inner_sql)


def sql_space_join(parts) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_space_join(list(parts))
    return fallbacks.sql_space_join(parts)


def row_count_or_zero(is_none: bool, row_count: int = 0) -> int:
    impl = _impl()
    if impl is not None:
        return impl.row_count_or_zero(is_none, row_count)
    return fallbacks.row_count_or_zero(is_none, row_count)


def queryset_sliced_error(is_sliced: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.queryset_sliced_error(is_sliced)
    return fallbacks.queryset_sliced_error(is_sliced)


def clear_none_arg(single_none: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.clear_none_arg(single_none)
    return fallbacks.clear_none_arg(single_none)


def only_none_arg_error(single_none: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.only_none_arg_error(single_none)
    return fallbacks.only_none_arg_error(single_none)


def reverse_standard_ordering(standard_ordering: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.reverse_standard_ordering(standard_ordering)
    return fallbacks.reverse_standard_ordering(standard_ordering)


def queryset_index_validate(is_int: bool, is_slice: bool, has_negative: bool) -> int:
    impl = _impl()
    if impl is not None:
        return impl.queryset_index_validate(is_int, is_slice, has_negative)
    return fallbacks.queryset_index_validate(is_int, is_slice, has_negative)


def qs_and_empty_kind(self_empty: bool, other_empty: bool) -> int:
    impl = _impl()
    if impl is not None:
        return impl.qs_and_empty_kind(self_empty, other_empty)
    return fallbacks.qs_and_empty_kind(self_empty, other_empty)


def qs_or_empty_kind(self_empty: bool, other_empty: bool) -> int:
    impl = _impl()
    if impl is not None:
        return impl.qs_or_empty_kind(self_empty, other_empty)
    return fallbacks.qs_or_empty_kind(self_empty, other_empty)


def date_kind_valid(kind: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.date_kind_valid(kind)
    return fallbacks.date_kind_valid(kind)


def datetime_kind_valid(kind: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.datetime_kind_valid(kind)
    return fallbacks.datetime_kind_valid(kind)


def date_order_valid(order: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.date_order_valid(order)
    return fallbacks.date_order_valid(order)


def order_by_desc_prefix(order: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.order_by_desc_prefix(order)
    return fallbacks.order_by_desc_prefix(order)


def earliest_missing_fields(has_fields: bool, has_get_latest_by: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.earliest_missing_fields(has_fields, has_get_latest_by)
    return fallbacks.earliest_missing_fields(has_fields, has_get_latest_by)


def save_base_needs_atomic(has_parents: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.save_base_needs_atomic(has_parents)
    return fallbacks.save_base_needs_atomic(has_parents)


def save_created_flag(updated: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.save_created_flag(updated)
    return fallbacks.save_created_flag(updated)


def do_update_empty_values_kind(has_update_fields: bool, exists: bool) -> int:
    impl = _impl()
    if impl is not None:
        return impl.do_update_empty_values_kind(has_update_fields, exists)
    return fallbacks.do_update_empty_values_kind(has_update_fields, exists)


def clean_field_skip(name_in_exclude: bool, generated: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.clean_field_skip(name_in_exclude, generated)
    return fallbacks.clean_field_skip(name_in_exclude, generated)


def clean_field_skip_blank_empty(blank: bool, in_empty_values: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.clean_field_skip_blank_empty(blank, in_empty_values)
    return fallbacks.clean_field_skip_blank_empty(blank, in_empty_values)


def validation_has_errors(n_error_keys: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.validation_has_errors(n_error_keys)
    return fallbacks.validation_has_errors(n_error_keys)


def is_non_field_errors_key(name: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_non_field_errors_key(name)
    return fallbacks.is_non_field_errors_key(name)


def fixed_timezone_name(offset_minutes: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.fixed_timezone_name(int(offset_minutes))
    return fallbacks.fixed_timezone_name(offset_minutes)


def datetime_is_aware(utcoffset_not_none: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.datetime_is_aware(utcoffset_not_none)
    return fallbacks.datetime_is_aware(utcoffset_not_none)


def datetime_is_naive(utcoffset_is_none: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.datetime_is_naive(utcoffset_is_none)
    return fallbacks.datetime_is_naive(utcoffset_is_none)


def mark_safe_kind(has_html: bool, is_callable: bool) -> int:
    impl = _impl()
    if impl is not None:
        return impl.mark_safe_kind(has_html, is_callable)
    return fallbacks.mark_safe_kind(has_html, is_callable)


def lookup_head(lookup: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.lookup_head(lookup)
    return fallbacks.lookup_head(lookup)



def queryset_is_ordered(
    is_empty_qs: bool,
    has_extra_order: bool,
    has_order_by: bool,
    default_ordering: bool,
    has_meta_ordering: bool,
    has_group_by: bool,
) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.queryset_is_ordered(
            is_empty_qs,
            has_extra_order,
            has_order_by,
            default_ordering,
            has_meta_ordering,
            has_group_by,
        )
    return fallbacks.queryset_is_ordered(
        is_empty_qs,
        has_extra_order,
        has_order_by,
        default_ordering,
        has_meta_ordering,
        has_group_by,
    )


def annotation_alias_conflicts(alias_in_names: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.annotation_alias_conflicts(alias_in_names)
    return fallbacks.annotation_alias_conflicts(alias_in_names)


def complex_filter_is_q(is_q_instance: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.complex_filter_is_q(is_q_instance)
    return fallbacks.complex_filter_is_q(is_q_instance)


def using_is_none(using_is_none: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.using_is_none(using_is_none)
    return fallbacks.using_is_none(using_is_none)


def refresh_fields_empty(n_fields: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.refresh_fields_empty(n_fields)
    return fallbacks.refresh_fields_empty(n_fields)


def refresh_fields_have_lookup_sep(fields) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.refresh_fields_have_lookup_sep(list(fields))
    return fallbacks.refresh_fields_have_lookup_sep(fields)


def unique_check_excluded(check_names, exclude) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.unique_check_excluded(list(check_names), list(exclude))
    return fallbacks.unique_check_excluded(check_names, exclude)


def unique_lookup_skip_value(
    is_none: bool, is_empty_str: bool, empty_as_null: bool
) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.unique_lookup_skip_value(is_none, is_empty_str, empty_as_null)
    return fallbacks.unique_lookup_skip_value(is_none, is_empty_str, empty_as_null)


def unique_check_incomplete(n_check: int, n_kwargs: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.unique_check_incomplete(n_check, n_kwargs)
    return fallbacks.unique_check_incomplete(n_check, n_kwargs)


def unique_error_is_single_field(n_check: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.unique_error_is_single_field(n_check)
    return fallbacks.unique_error_is_single_field(n_check)


def in_lookup_empty(n_rhs: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.in_lookup_empty(n_rhs)
    return fallbacks.in_lookup_empty(n_rhs)


def sql_lhs_rhs(lhs: str, rhs_op: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_lhs_rhs(lhs, rhs_op)
    return fallbacks.sql_lhs_rhs(lhs, rhs_op)


def sql_or_join(parts) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_or_join(list(parts))
    return fallbacks.sql_or_join(parts)


def is_password_usable(encoded_is_none: bool, starts_with_unusable: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.is_password_usable(encoded_is_none, starts_with_unusable)
    return fallbacks.is_password_usable(encoded_is_none, starts_with_unusable)


def identify_hasher_kind(
    encoded_len: int,
    has_dollar: bool,
    starts_md5_dollar: bool,
    starts_sha1_dollar: bool,
) -> int:
    impl = _impl()
    if impl is not None:
        return impl.identify_hasher_kind(
            encoded_len, has_dollar, starts_md5_dollar, starts_sha1_dollar
        )
    return fallbacks.identify_hasher_kind(
        encoded_len, has_dollar, starts_md5_dollar, starts_sha1_dollar
    )


def hasher_algorithm_prefix(encoded: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.hasher_algorithm_prefix(encoded)
    return fallbacks.hasher_algorithm_prefix(encoded)


def cache_default_key(key_prefix: str, version: int, key: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.cache_default_key(key_prefix, int(version), key)
    return fallbacks.cache_default_key(key_prefix, version, key)


def cache_timeout_kind(
    is_default_sentinel: bool, is_none: bool, timeout: int = 0
) -> int:
    impl = _impl()
    if impl is not None:
        return impl.cache_timeout_kind(is_default_sentinel, is_none, timeout)
    return fallbacks.cache_timeout_kind(is_default_sentinel, is_none, timeout)


def file_multiple_chunks(size: int, chunk_size: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.file_multiple_chunks(int(size), int(chunk_size))
    return fallbacks.file_multiple_chunks(size, chunk_size)


def mask_hash(hash: str, show: int = 6, mask_char: str = "*") -> str:
    impl = _impl()
    if impl is not None:
        ch = mask_char if isinstance(mask_char, str) and mask_char else "*"
        return impl.mask_hash(hash, show, ch[0])
    return fallbacks.mask_hash(hash, show, mask_char)


def result_cache_populated(cache_is_none: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.result_cache_populated(cache_is_none)
    return fallbacks.result_cache_populated(cache_is_none)


def prefetch_still_needed(has_lookups: bool, prefetch_done: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.prefetch_still_needed(has_lookups, prefetch_done)
    return fallbacks.prefetch_still_needed(has_lookups, prefetch_done)


def queryset_cache_truthy(cache_len: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.queryset_cache_truthy(cache_len)
    return fallbacks.queryset_cache_truthy(cache_len)


def sticky_filter_active(sticky: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.sticky_filter_active(sticky)
    return fallbacks.sticky_filter_active(sticky)


def csrf_token_length_ok(len_: int, secret_len: int, token_len: int) -> int:
    impl = _impl()
    if impl is not None:
        return impl.csrf_token_length_ok(len_, secret_len, token_len)
    return fallbacks.csrf_token_length_ok(len_, secret_len, token_len)


def csrf_token_chars_valid(token: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.csrf_token_chars_valid(token)
    return fallbacks.csrf_token_chars_valid(token)


def csrf_unmask_token(token: str, secret_len: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.csrf_unmask_token(token, secret_len)
    return fallbacks.csrf_unmask_token(token, secret_len)


def csrf_mask_secret(secret: str, mask: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.csrf_mask_secret(secret, mask)
    return fallbacks.csrf_mask_secret(secret, mask)


def hsts_header_value(seconds: int, include_subdomains: bool, preload: bool) -> str:
    impl = _impl()
    if impl is not None:
        return impl.hsts_header_value(seconds, include_subdomains, preload)
    return fallbacks.hsts_header_value(seconds, include_subdomains, preload)


def https_redirect_url(host: str, full_path: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.https_redirect_url(host, full_path)
    return fallbacks.https_redirect_url(host, full_path)


def referrer_policy_header(policies) -> str:
    impl = _impl()
    if impl is not None:
        return impl.referrer_policy_header(list(policies))
    return fallbacks.referrer_policy_header(policies)


def route_looks_like_regex(route: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.route_looks_like_regex(route)
    return fallbacks.route_looks_like_regex(route)


def route_simple_match(is_endpoint: bool, route: str, path: str):
    impl = _impl()
    if impl is not None:
        return tuple(impl.route_simple_match(is_endpoint, route, path))
    return fallbacks.route_simple_match(is_endpoint, route, path)


def engine_loaders_app_dirs_conflict(app_dirs: bool, loaders_defined: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.engine_loaders_app_dirs_conflict(app_dirs, loaders_defined)
    return fallbacks.engine_loaders_app_dirs_conflict(app_dirs, loaders_defined)


def template_cache_key_plain(template_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.template_cache_key_plain(template_name)
    return fallbacks.template_cache_key_plain(template_name)


def to_language(locale: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.to_language(locale)
    return fallbacks.to_language(locale)


def to_locale(language: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.to_locale(language)
    return fallbacks.to_locale(language)


def plural_index_default(n: int) -> int:
    impl = _impl()
    if impl is not None:
        return impl.plural_index_default(n)
    return fallbacks.plural_index_default(n)


def language_code_too_long(len_: int, max_len: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.language_code_too_long(len_, max_len)
    return fallbacks.language_code_too_long(len_, max_len)


def sql_create_table(quoted_table: str, columns_sql: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_create_table(quoted_table, columns_sql)
    return fallbacks.sql_create_table(quoted_table, columns_sql)


def migration_describe(class_name: str, constructor_args: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.migration_describe(class_name, constructor_args)
    return fallbacks.migration_describe(class_name, constructor_args)


def migration_formatted_description(category: str, description: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.migration_formatted_description(category, description)
    return fallbacks.migration_formatted_description(category, description)


def http_status_code_valid(code: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.http_status_code_valid(code)
    return fallbacks.http_status_code_valid(code)


def weak_etag_if_strong(etag: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.weak_etag_if_strong(etag)
    return fallbacks.weak_etag_if_strong(etag)


def accepts_gzip(accept_encoding: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.accepts_gzip(accept_encoding)
    return fallbacks.accepts_gzip(accept_encoding)


def gzip_content_too_short(content_len: int, min_len: int = 200) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.gzip_content_too_short(content_len, min_len)
    return fallbacks.gzip_content_too_short(content_len, min_len)


def host_needs_www_prefix(host: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.host_needs_www_prefix(host)
    return fallbacks.host_needs_www_prefix(host)


def www_redirect_url(scheme: str, host: str, path: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.www_redirect_url(scheme, host, path)
    return fallbacks.www_redirect_url(scheme, host, path)


def xframe_options_value(setting_value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.xframe_options_value(setting_value or "")
    return fallbacks.xframe_options_value(setting_value)


# --- Fat middleware bodies (chain iteration stays Python) -------------------


def security_process_request(
    redirect_enabled,
    is_secure,
    path_lstrip,
    full_path,
    redirect_host,
    request_host,
    exempt_patterns,
):
    impl = _impl()
    if impl is not None:
        return impl.security_process_request(
            bool(redirect_enabled),
            bool(is_secure),
            str(path_lstrip),
            str(full_path),
            str(redirect_host or ""),
            str(request_host or ""),
            list(exempt_patterns or ()),
        )
    raise RuntimeError("native security_process_request unavailable")


def security_process_response(
    is_secure,
    has_sts_header,
    sts_seconds,
    sts_include_subdomains,
    sts_preload,
    content_type_nosniff,
    has_content_type_options,
    referrer_policy,
    has_referrer_policy,
    cross_origin_opener_policy,
    has_coop,
):
    impl = _impl()
    if impl is not None:
        return impl.security_process_response(
            bool(is_secure),
            bool(has_sts_header),
            int(sts_seconds),
            bool(sts_include_subdomains),
            bool(sts_preload),
            bool(content_type_nosniff),
            bool(has_content_type_options),
            referrer_policy,
            bool(has_referrer_policy),
            cross_origin_opener_policy,
            bool(has_coop),
        )
    raise RuntimeError("native security_process_response unavailable")


def xframe_process_response(already_has_header, xframe_options_exempt, setting_value):
    impl = _impl()
    if impl is not None:
        return impl.xframe_process_response(
            bool(already_has_header),
            bool(xframe_options_exempt),
            str(setting_value or "DENY"),
        )
    raise RuntimeError("native xframe_process_response unavailable")


def common_content_length_header(streaming, already_has_content_length, content_len):
    impl = _impl()
    if impl is not None:
        return impl.common_content_length_header(
            bool(streaming), bool(already_has_content_length), int(content_len)
        )
    raise RuntimeError("native common_content_length_header unavailable")


def common_www_redirect_url(prepend_www, host, scheme, path):
    impl = _impl()
    if impl is not None:
        return impl.common_www_redirect_url(
            bool(prepend_www), str(host or ""), str(scheme or ""), str(path or "")
        )
    raise RuntimeError("native common_www_redirect_url unavailable")


def gzip_process_response_plan(
    streaming,
    content_len,
    min_len,
    has_content_encoding,
    accept_encoding,
    etag="",
):
    impl = _impl()
    if impl is not None:
        return impl.gzip_process_response_plan(
            bool(streaming),
            int(content_len),
            int(min_len),
            bool(has_content_encoding),
            str(accept_encoding or ""),
            str(etag or ""),
        )
    raise RuntimeError("native gzip_process_response_plan unavailable")


def conditional_needs_etag(cache_control: str) -> bool:
    impl = _impl()
    if impl is not None:
        return bool(impl.conditional_needs_etag(cache_control or ""))
    return fallbacks.conditional_needs_etag(cache_control)


def session_cookie_expiry(expire_at_browser_close, expiry_age_seconds, now_unix):
    impl = _impl()
    if impl is not None:
        return tuple(
            impl.session_cookie_expiry(
                bool(expire_at_browser_close),
                int(expiry_age_seconds),
                float(now_unix),
            )
        )
    raise RuntimeError("native session_cookie_expiry unavailable")


def session_process_response_plan(
    accessed,
    modified,
    empty,
    cookie_in_request,
    save_every_request,
    status_code,
    expire_at_browser_close,
    expiry_age_seconds,
    now_unix,
):
    """One-crossing session process_response decision (action/need_vary/expiry)."""
    impl = _impl()
    if impl is not None:
        return impl.session_process_response_plan(
            bool(accessed),
            bool(modified),
            bool(empty),
            bool(cookie_in_request),
            bool(save_every_request),
            int(status_code),
            bool(expire_at_browser_close),
            int(expiry_age_seconds),
            float(now_unix),
        )
    raise RuntimeError("native session_process_response_plan unavailable")


def session_load_key(cookie_value, min_length=8):
    """Validate session cookie value; return key or None if missing/invalid."""
    impl = _impl()
    if impl is not None:
        return impl.session_load_key(cookie_value, int(min_length))
    raise RuntimeError("native session_load_key unavailable")


def csrf_process_view_gate(csrf_processing_done, csrf_exempt, method, dont_enforce):
    """Early CSRF process_view gate: done|exempt|accept|check."""
    impl = _impl()
    if impl is not None:
        return impl.csrf_process_view_gate(
            bool(csrf_processing_done),
            bool(csrf_exempt),
            str(method or ""),
            bool(dont_enforce),
        )
    raise RuntimeError("native csrf_process_view_gate unavailable")


def csrf_is_safe_method(method: str) -> bool:
    impl = _impl()
    if impl is not None:
        return bool(impl.csrf_is_safe_method(str(method or "")))
    m = (method or "").upper()
    return m in ("GET", "HEAD", "OPTIONS", "TRACE")


def csrf_secrets_match(request_token, csrf_secret, secret_len=32, token_len=64):
    """Constant-time CSRF secret compare (handles masked tokens)."""
    impl = _impl()
    if impl is not None:
        return bool(
            impl.csrf_secrets_match(
                str(request_token),
                str(csrf_secret),
                int(secret_len),
                int(token_len),
            )
        )
    raise RuntimeError("native csrf_secrets_match unavailable")


def csrf_origin_verified(
    request_origin, good_origin="", exact_origins=None, subdomain_patterns=None
):
    """True if Origin is the request host or a trusted origin/subdomain."""
    impl = _impl()
    if impl is not None:
        patterns = []
        for item in subdomain_patterns or ():
            if isinstance(item, (list, tuple)) and len(item) >= 2:
                patterns.append((str(item[0]), str(item[1])))
        return bool(
            impl.csrf_origin_verified(
                str(request_origin or ""),
                str(good_origin or ""),
                [str(x) for x in (exact_origins or ())],
                patterns,
            )
        )
    raise RuntimeError("native csrf_origin_verified unavailable")


def csrf_check_referer(referer_header="", good_referer="", trusted_hosts=None):
    """Empty string if Referer ok, else no_referer|malformed|insecure|bad."""
    impl = _impl()
    if impl is not None:
        return str(
            impl.csrf_check_referer(
                str(referer_header or ""),
                str(good_referer or ""),
                [str(x) for x in (trusted_hosts or ())],
            )
        )
    raise RuntimeError("native csrf_check_referer unavailable")


def auth_login_required_gate(login_required, is_authenticated) -> int:
    """0=skip, 1=allow, 2=need redirect."""
    impl = _impl()
    if impl is not None:
        return int(
            impl.auth_login_required_gate(
                bool(login_required), bool(is_authenticated)
            )
        )
    raise RuntimeError("native auth_login_required_gate unavailable")


def is_native_stock_middleware_path(dotted_path: str) -> bool:
    """True if path is a known dual-path stock middleware class."""
    impl = _impl()
    if impl is not None:
        return bool(impl.is_native_stock_middleware_path(str(dotted_path or "")))
    return False


def native_stock_chain_call(specs, request, get_response):
    """
    Pure-C++ stock chain for fully-native header middleware stacks.

    specs: list of dicts with type security|xframe|common and config.
    get_response: Python view layer (may still run process_view hooks).
    """
    impl = _impl()
    if impl is not None:
        return impl.native_stock_chain_call(specs, request, get_response)
    raise RuntimeError("native native_stock_chain_call unavailable")


def hybrid_process_request(cfg, request):
    """Batched Security/Common process_request. Returns redirect or None."""
    impl = _impl()
    if impl is not None:
        return impl.hybrid_process_request(cfg, request)
    raise RuntimeError("native hybrid_process_request unavailable")


def hybrid_process_response(cfg, request, response):
    """Batched XFrame + Content-Length + Security process_response."""
    impl = _impl()
    if impl is not None:
        return impl.hybrid_process_response(cfg, request, response)
    raise RuntimeError("native hybrid_process_response unavailable")


def session_response_needs_work(accessed, modified, save_every_request) -> bool:
    """False when SessionMiddleware process_response is a pure no-op."""
    impl = _impl()
    if impl is not None:
        return bool(
            impl.session_response_needs_work(
                bool(accessed), bool(modified), bool(save_every_request)
            )
        )
    return bool(accessed or modified or save_every_request)


def hybrid_chain_call(cfg, bits, request, get_response):
    """C++-orchestrated hybrid middleware chain (one crossing)."""
    impl = _impl()
    if impl is not None:
        return impl.hybrid_chain_call(cfg, bits, request, get_response)
    raise RuntimeError("native hybrid_chain_call unavailable")


def cookie_header_get(cookie_header, name):
    """Extract one cookie value from a Cookie header (last-wins)."""
    impl = _impl()
    if impl is not None:
        return impl.cookie_header_get(str(cookie_header or ""), str(name or ""))
    return None
def message_tags_join(extra_tags: str, level_tag: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.message_tags_join(extra_tags or "", level_tag or "")
    return fallbacks.message_tags_join(extra_tags, level_tag)


def hashed_static_basename(root: str, hash_with_dot: str, ext: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.hashed_static_basename(root, hash_with_dot, ext)
    return fallbacks.hashed_static_basename(root, hash_with_dot, ext)


def posix_path_join(directory: str, basename: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.posix_path_join(directory or "", basename)
    return fallbacks.posix_path_join(directory, basename)


def json_use_indent_separators(has_indent: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.json_use_indent_separators(has_indent)
    return fallbacks.json_use_indent_separators(has_indent)


def datetime_iso_utc_z(iso: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.datetime_iso_utc_z(iso)
    return fallbacks.datetime_iso_utc_z(iso)


def string_has_newlines(s: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.string_has_newlines(s)
    return fallbacks.string_has_newlines(s)


def split_email_address(address: str):
    impl = _impl()
    if impl is not None:
        return tuple(impl.split_email_address(address))
    return fallbacks.split_email_address(address)


def model_meta_label(app_label: str, object_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.model_meta_label(app_label, object_name)
    return fallbacks.model_meta_label(app_label, object_name)


def manager_str(model_label: str, manager_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.manager_str(model_label, manager_name)
    return fallbacks.manager_str(model_label, manager_name)


def from_queryset_class_name(manager_cls: str, qs_cls: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.from_queryset_class_name(manager_cls, qs_cls)
    return fallbacks.from_queryset_class_name(manager_cls, qs_cls)


def migration_node_key(app_label: str, name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.migration_node_key(app_label, name)
    return fallbacks.migration_node_key(app_label, name)


def perm_codename(action: str, model_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.perm_codename(action, model_name)
    return fallbacks.perm_codename(action, model_name)


def user_can_authenticate(has_is_active: bool, is_active: bool) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.user_can_authenticate(has_is_active, is_active)
    return fallbacks.user_can_authenticate(has_is_active, is_active)


def signal_has_receivers(n_receivers: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.signal_has_receivers(n_receivers)
    return fallbacks.signal_has_receivers(n_receivers)


def split_dotted_path(dotted: str):
    impl = _impl()
    if impl is not None:
        return tuple(impl.split_dotted_path(dotted))
    return fallbacks.split_dotted_path(dotted)


def app_module_path(app_name: str, submodule: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.app_module_path(app_name, submodule)
    return fallbacks.app_module_path(app_name, submodule)


def renamed_method_warning(class_name: str, old_name: str, new_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.renamed_method_warning(class_name, old_name, new_name)
    return fallbacks.renamed_method_warning(class_name, old_name, new_name)


def path_ends_with_py(path: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.path_ends_with_py(path)
    return fallbacks.path_ends_with_py(path)


def path_has_any_suffix(path: str, suffixes) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.path_has_any_suffix(path, list(suffixes))
    return fallbacks.path_has_any_suffix(path, suffixes)


def postgres_arrayfield_path_shorten(path: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.postgres_arrayfield_path_shorten(path)
    return fallbacks.postgres_arrayfield_path_shorten(path)


def filename_needs_quotes(filename: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.filename_needs_quotes(filename)
    return fallbacks.filename_needs_quotes(filename)

def paginator_num_pages(count, per_page, orphans, allow_empty_first_page):
    impl = _impl()
    if impl is not None:
        return impl.paginator_num_pages(count, per_page, orphans, allow_empty_first_page)
    return fallbacks.paginator_num_pages(count, per_page, orphans, allow_empty_first_page)

def paginator_page_bottom(number, per_page):
    impl = _impl()
    if impl is not None:
        return impl.paginator_page_bottom(number, per_page)
    return fallbacks.paginator_page_bottom(number, per_page)

def paginator_page_top(number, per_page, orphans, count):
    impl = _impl()
    if impl is not None:
        return impl.paginator_page_top(number, per_page, orphans, count)
    return fallbacks.paginator_page_top(number, per_page, orphans, count)

def paginator_number_range_code(number, num_pages):
    impl = _impl()
    if impl is not None:
        return impl.paginator_number_range_code(number, num_pages)
    return fallbacks.paginator_number_range_code(number, num_pages)

def url_is_relative_path(to: str):
    impl = _impl()
    if impl is not None:
        return impl.url_is_relative_path(to)
    return fallbacks.url_is_relative_path(to)

def url_feels_like_url(to: str):
    impl = _impl()
    if impl is not None:
        return impl.url_feels_like_url(to)
    return fallbacks.url_feels_like_url(to)

def formset_total_forms_bound(submitted, absolute_max):
    impl = _impl()
    if impl is not None:
        return impl.formset_total_forms_bound(submitted, absolute_max)
    return fallbacks.formset_total_forms_bound(submitted, absolute_max)

def formset_total_forms_unbound(initial_forms, min_num, extra, max_num):
    impl = _impl()
    if impl is not None:
        return impl.formset_total_forms_unbound(initial_forms, min_num, extra, max_num)
    return fallbacks.formset_total_forms_unbound(initial_forms, min_num, extra, max_num)

def path_has_dotdot(path: str):
    impl = _impl()
    if impl is not None:
        return impl.path_has_dotdot(path)
    return fallbacks.path_has_dotdot(path)

def storage_normalize_name(name: str):
    impl = _impl()
    if impl is not None:
        return impl.storage_normalize_name(name)
    return fallbacks.storage_normalize_name(name)

def storage_alternative_name(root: str, random7: str, ext: str):
    impl = _impl()
    if impl is not None:
        return impl.storage_alternative_name(root, random7, ext)
    return fallbacks.storage_alternative_name(root, random7, ext)

def storage_name_available(exists, has_max_length, name_len, max_length=0):
    impl = _impl()
    if impl is not None:
        return impl.storage_name_available(
            bool(exists),
            bool(has_max_length),
            int(name_len),
            0 if max_length is None else int(max_length),
        )
    return fallbacks.storage_name_available(exists, has_max_length, name_len, max_length)

def middleware_capability_ok(sync_capable, async_capable):
    impl = _impl()
    if impl is not None:
        return impl.middleware_capability_ok(bool(sync_capable), bool(async_capable))
    return fallbacks.middleware_capability_ok(sync_capable, async_capable)

def sitemap_priority_valid(priority: float):
    impl = _impl()
    if impl is not None:
        return impl.sitemap_priority_valid(priority)
    return fallbacks.sitemap_priority_valid(priority)

def sitemap_changefreq_valid(freq: str):
    impl = _impl()
    if impl is not None:
        return impl.sitemap_changefreq_valid(freq)
    return fallbacks.sitemap_changefreq_valid(freq)

def ordinal_suffix_kind(value: int):
    impl = _impl()
    if impl is not None:
        return impl.ordinal_suffix_kind(value)
    return fallbacks.ordinal_suffix_kind(value)

def intcomma_ascii(digits: str):
    impl = _impl()
    if impl is not None:
        return impl.intcomma_ascii(digits)
    return fallbacks.intcomma_ascii(digits)

def check_is_serious(level: int, threshold: int):
    impl = _impl()
    if impl is not None:
        return impl.check_is_serious(level, threshold)
    return fallbacks.check_is_serious(level, threshold)

def path_with_query(path: str, query: str):
    impl = _impl()
    if impl is not None:
        return impl.path_with_query(path, query)
    return fallbacks.path_with_query(path, query)

def ensure_leading_slash(path: str):
    impl = _impl()
    if impl is not None:
        return impl.ensure_leading_slash(path)
    return fallbacks.ensure_leading_slash(path)

def redirect_paths_equal(a: str, b: str):
    impl = _impl()
    if impl is not None:
        return impl.redirect_paths_equal(a, b)
    return fallbacks.redirect_paths_equal(a, b)

def wkt_point(x: str, y: str):
    impl = _impl()
    if impl is not None:
        return impl.wkt_point(x, y)
    return fallbacks.wkt_point(x, y)

def postgres_empty_array_literal():
    impl = _impl()
    if impl is not None:
        return impl.postgres_empty_array_literal()
    return fallbacks.postgres_empty_array_literal()


def list_context_object_name(model_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.list_context_object_name(model_name)
    return fallbacks.list_context_object_name(model_name)


def http_method_in_names(method_lower: str, names) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.http_method_in_names(method_lower, list(names))
    return fallbacks.http_method_in_names(method_lower, names)


def page_token_is_last(page: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.page_token_is_last(page)
    return fallbacks.page_token_is_last(page)


def model_template_name(app_label: str, object_name: str, suffix: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.model_template_name(app_label, object_name, suffix)
    return fallbacks.model_template_name(app_label, object_name, suffix)


def modelform_class_name(model_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.modelform_class_name(model_name)
    return fallbacks.modelform_class_name(model_name)


def form_field_included(
    editable, fields_is_none, in_fields, exclude_active, in_exclude
) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.form_field_included(
            editable, fields_is_none, in_fields, exclude_active, in_exclude
        )
    return fallbacks.form_field_included(
        editable, fields_is_none, in_fields, exclude_active, in_exclude
    )


def admin_quote(s: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.admin_quote(s)
    return fallbacks.admin_quote(s)


def lookup_key_endswith(key: str, suffix: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.lookup_key_endswith(key, suffix)
    return fallbacks.lookup_key_endswith(key, suffix)


def prepare_lookup_isnull(value_lower: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.prepare_lookup_isnull(value_lower)
    return fallbacks.prepare_lookup_isnull(value_lower)


def paths_equal(a: str, b: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.paths_equal(a, b)
    return fallbacks.paths_equal(a, b)


def strings_ci_equal_ascii(a: str, b: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.strings_ci_equal_ascii(a, b)
    return fallbacks.strings_ci_equal_ascii(a, b)


def migration_filename(name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.migration_filename(name)
    return fallbacks.migration_filename(name)


def introspection_is_table(type_code: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.introspection_is_table(type_code)
    return fallbacks.introspection_is_table(type_code)


def combined_expression_sql(lhs: str, connector: str, rhs: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.combined_expression_sql(lhs, connector, rhs)
    return fallbacks.combined_expression_sql(lhs, connector, rhs)


def sql_cast_as_numeric(sql: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_cast_as_numeric(sql)
    return fallbacks.sql_cast_as_numeric(sql)


def cache_timestamp_expired(exp_is_none, exp, now) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.cache_timestamp_expired(bool(exp_is_none), float(exp or 0), float(now))
    return fallbacks.cache_timestamp_expired(exp_is_none, exp, now)


def cache_file_name(hexdigest: str, suffix: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.cache_file_name(hexdigest, suffix)
    return fallbacks.cache_file_name(hexdigest, suffix)


def cache_cull_needed(num_entries, max_entries) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.cache_cull_needed(int(num_entries), int(max_entries))
    return fallbacks.cache_cull_needed(num_entries, max_entries)


def cache_cull_sample_size(num_entries, cull_frequency) -> int:
    impl = _impl()
    if impl is not None:
        return impl.cache_cull_sample_size(int(num_entries), int(cull_frequency))
    return fallbacks.cache_cull_sample_size(num_entries, cull_frequency)


def wsgi_request_path(script_name: str, path_info: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.wsgi_request_path(script_name, path_info)
    return fallbacks.wsgi_request_path(script_name, path_info)


def wsgi_handler_lean_eligible(handler) -> bool:
    """True when C++ WSGI loop may run (empty middleware, no ATOMIC_REQUESTS)."""
    impl = _impl()
    if impl is not None:
        return bool(impl.wsgi_handler_lean_eligible(handler))
    return False


def wsgi_handler_call(handler, environ, start_response):
    """
    Native lean WSGIHandler request loop.

    Slim request + exact-route resolve + Python view + start_response pack.
    Raises if the extension is unavailable — callers dual-path to pure Python.
    """
    impl = _impl()
    if impl is not None:
        return impl.wsgi_handler_call(handler, environ, start_response)
    raise RuntimeError("native wsgi_handler_call unavailable")


def wsgi_pack_start_response(response, start_response, environ=None):
    """Pack HttpResponse into start_response (C++ _store walk)."""
    impl = _impl()
    if impl is not None:
        return impl.wsgi_pack_start_response(
            response, start_response, environ if environ is not None else {}
        )
    raise RuntimeError("native wsgi_pack_start_response unavailable")

def wsgi_request_try_lean_init(request, environ) -> bool:
    """GET/HEAD empty-body WSGIRequest init in C++. True if applied."""
    impl = _impl()
    if impl is not None:
        return bool(impl.wsgi_request_try_lean_init(request, environ))
    return False


def wsgi_lean_get_response(handler, request):
    """Exact-route or resolve + call view (Python). Extension required."""
    impl = _impl()
    if impl is not None:
        return impl.wsgi_lean_get_response(handler, request)
    raise RuntimeError("native wsgi_lean_get_response unavailable")


def wsgi_environ_is_lean_get(environ) -> bool:
    impl = _impl()
    if impl is not None:
        return bool(impl.wsgi_environ_is_lean_get(environ))
    return False

def exception_status_code(kind: str) -> int:
    impl = _impl()
    if impl is not None:
        return impl.exception_status_code(kind)
    return fallbacks.exception_status_code(kind)


def postgres_normalize_spaces(val: str):
    impl = _impl()
    if impl is not None:
        return impl.postgres_normalize_spaces(val)
    return fallbacks.postgres_normalize_spaces(val)


def postgres_psql_escape(query: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.postgres_psql_escape(query)
    return fallbacks.postgres_psql_escape(query)


def search_vector_match_sql(lhs: str, rhs: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.search_vector_match_sql(lhs, rhs)
    return fallbacks.search_vector_match_sql(lhs, rhs)


def feed_protocol(secure: bool) -> str:
    impl = _impl()
    if impl is not None:
        return impl.feed_protocol(secure)
    return fallbacks.feed_protocol(secure)


def feed_url_is_network_path(url: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.feed_url_is_network_path(url)
    return fallbacks.feed_url_is_network_path(url)


def feed_url_has_scheme(url: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.feed_url_has_scheme(url)
    return fallbacks.feed_url_has_scheme(url)


def feed_network_path_url(protocol: str, url: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.feed_network_path_url(protocol, url)
    return fallbacks.feed_network_path_url(protocol, url)


def feed_absolute_url(protocol: str, domain: str, url: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.feed_absolute_url(protocol, domain, url)
    return fallbacks.feed_absolute_url(protocol, domain, url)


def dotted_qualname(module: str, qualname: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.dotted_qualname(module, qualname)
    return fallbacks.dotted_qualname(module, qualname)


def strip_p_tags(value: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.strip_p_tags(value)
    return fallbacks.strip_p_tags(value)


def approximate_equal(val, other, places: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.approximate_equal(float(val), float(other), int(places))
    return fallbacks.approximate_equal(val, other, places)


def http_allow_header(methods) -> str:
    impl = _impl()
    if impl is not None:
        return impl.http_allow_header([str(m) for m in methods])
    return fallbacks.http_allow_header(methods)


def ensure_trailing_slash(url: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.ensure_trailing_slash(url)
    return fallbacks.ensure_trailing_slash(url)


def ascii_lower(s: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.ascii_lower(s)
    return fallbacks.ascii_lower(s)


def management_command_name(path: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.management_command_name(path)
    return fallbacks.management_command_name(path)


def asgi_path_info(path: str, script_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.asgi_path_info(path, script_name)
    return fallbacks.asgi_path_info(path, script_name)


def field_str(model_label: str, name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.field_str(model_label, name)
    return fallbacks.field_str(model_label, name)


def field_repr(path: str, has_name: bool, name: str = "") -> str:
    impl = _impl()
    if impl is not None:
        return impl.field_repr(path, has_name, name)
    return fallbacks.field_repr(path, has_name, name)


def verbose_name_from_name(name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.verbose_name_from_name(name)
    return fallbacks.verbose_name_from_name(name)


def field_name_check_code(name: str) -> int:
    impl = _impl()
    if impl is not None:
        return impl.field_name_check_code(name)
    return fallbacks.field_name_check_code(name)


def field_column_name(attname: str, db_column: str = "") -> str:
    impl = _impl()
    if impl is not None:
        return impl.field_column_name(attname, db_column or "")
    return fallbacks.field_column_name(attname, db_column or "")


def aggregate_default_alias(expr_name: str, agg_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.aggregate_default_alias(expr_name, agg_name)
    return fallbacks.aggregate_default_alias(expr_name, agg_name)


def sql_distinct_prefix(distinct: bool) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_distinct_prefix(distinct)
    return fallbacks.sql_distinct_prefix(distinct)


def index_column_with_order(column: str, descending: bool) -> str:
    impl = _impl()
    if impl is not None:
        return impl.index_column_with_order(column, descending)
    return fallbacks.index_column_with_order(column, descending)


def index_name_fix_leading(name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.index_name_fix_leading(name)
    return fallbacks.index_name_fix_leading(name)


def admin_can_show_all(result_count: int, list_max_show_all: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.admin_can_show_all(int(result_count), int(list_max_show_all))
    return fallbacks.admin_can_show_all(result_count, list_max_show_all)


def admin_is_multi_page(result_count: int, list_per_page: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.admin_is_multi_page(int(result_count), int(list_per_page))
    return fallbacks.admin_is_multi_page(result_count, list_per_page)


def query_string_with_prefix(encoded: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.query_string_with_prefix(encoded)
    return fallbacks.query_string_with_prefix(encoded)


def css_classes_join(classes) -> str:
    impl = _impl()
    if impl is not None:
        return impl.css_classes_join([str(c) for c in classes])
    return fallbacks.css_classes_join(classes)


def password_reset_token_join(ts_b36: str, hash_hex: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.password_reset_token_join(ts_b36, hash_hex)
    return fallbacks.password_reset_token_join(ts_b36, hash_hex)


def password_reset_token_split(token: str):
    impl = _impl()
    if impl is not None:
        return tuple(impl.password_reset_token_split(token))
    return fallbacks.password_reset_token_split(token)


def password_meets_min_length(password_len: int, min_length: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.password_meets_min_length(int(password_len), int(min_length))
    return fallbacks.password_meets_min_length(password_len, min_length)


def password_is_numeric_only(password: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.password_is_numeric_only(password)
    return fallbacks.password_is_numeric_only(password)


def migration_node_repr(cls: str, app: str, name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.migration_node_repr(cls, app, name)
    return fallbacks.migration_node_repr(cls, app, name)


def serializer_datetime_import() -> str:
    impl = _impl()
    if impl is not None:
        return impl.serializer_datetime_import()
    return fallbacks.serializer_datetime_import()


def sitemap_absolute_url(protocol: str, domain: str, path: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sitemap_absolute_url(protocol, domain, path)
    return fallbacks.sitemap_absolute_url(protocol, domain, path)


def sitemap_paged_url(absolute_url: str, page: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sitemap_paged_url(absolute_url, int(page))
    return fallbacks.sitemap_paged_url(absolute_url, page)


def x_robots_tag_value() -> str:
    impl = _impl()
    if impl is not None:
        return impl.x_robots_tag_value()
    return fallbacks.x_robots_tag_value()


def http_status_session_saveable(status_code: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.http_status_session_saveable(int(status_code))
    return fallbacks.http_status_session_saveable(status_code)


def resource_was_modified(header_missing: bool, mtime, header_mtime) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.resource_was_modified(
            bool(header_missing), float(mtime), float(header_mtime)
        )
    return fallbacks.resource_was_modified(header_missing, mtime, header_mtime)


def template_register_name(explicit_name: str, func_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.template_register_name(explicit_name or "", func_name)
    return fallbacks.template_register_name(explicit_name or "", func_name)


def normalize_ascii_whitespace(s: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.normalize_ascii_whitespace(s)
    return fallbacks.normalize_ascii_whitespace(s)


def html_boolean_attr_is_true(name: str, value: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.html_boolean_attr_is_true(name, value if value is not None else "")
    return fallbacks.html_boolean_attr_is_true(name, value if value is not None else "")


def sql_func_call(function: str, expressions: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_func_call(function, expressions)
    return fallbacks.sql_func_call(function, expressions)


def field_display_method_name(field_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.field_display_method_name(field_name)
    return fallbacks.field_display_method_name(field_name)


def optimizer_lists_equal_len(a: int, b: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.optimizer_lists_equal_len(int(a), int(b))
    return fallbacks.optimizer_lists_equal_len(a, b)

def related_name_ends_plus(name: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.related_name_ends_plus(name)
    return fallbacks.related_name_ends_plus(name)


def related_name_is_identifier(name: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.related_name_is_identifier(name)
    return fallbacks.related_name_is_identifier(name)


def related_query_name_ends_underscore(name: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.related_query_name_ends_underscore(name)
    return fallbacks.related_query_name_ends_underscore(name)


def related_query_name_has_lookup_sep(name: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.related_query_name_has_lookup_sep(name)
    return fallbacks.related_query_name_has_lookup_sep(name)


def fk_default_name(model_name: str, pk_name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.fk_default_name(model_name, pk_name)
    return fallbacks.fk_default_name(model_name, pk_name)


def related_filter_key(field_name: str, rh_field: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.related_filter_key(field_name, rh_field)
    return fallbacks.related_filter_key(field_name, rh_field)


def constraint_deconstruct_path(path: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.constraint_deconstruct_path(path)
    return fallbacks.constraint_deconstruct_path(path)


def sql_varchar_type(has_max_length: bool, max_length: int = 0) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_varchar_type(bool(has_max_length), int(max_length or 0))
    return fallbacks.sql_varchar_type(has_max_length, max_length)


def sql_decimal_type(max_digits: int, decimal_places: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_decimal_type(int(max_digits), int(decimal_places))
    return fallbacks.sql_decimal_type(max_digits, decimal_places)


def admin_selectfilter_class(is_stacked: bool) -> str:
    impl = _impl()
    if impl is not None:
        return impl.admin_selectfilter_class(bool(is_stacked))
    return fallbacks.admin_selectfilter_class(is_stacked)


def admin_site_repr(cls: str, name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.admin_site_repr(cls, name)
    return fallbacks.admin_site_repr(cls, name)


def permission_str(content_type: str, name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.permission_str(str(content_type), name)
    return fallbacks.permission_str(str(content_type), name)


def admin_facet_count_key(index: int) -> str:
    impl = _impl()
    if impl is not None:
        return impl.admin_facet_count_key(int(index))
    return fallbacks.admin_facet_count_key(index)


def extract_lookup_name(lookup: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.extract_lookup_name(lookup)
    return fallbacks.extract_lookup_name(lookup)


def sql_now_sqlite() -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_now_sqlite()
    return fallbacks.sql_now_sqlite()


def sql_now_postgresql() -> str:
    impl = _impl()
    if impl is not None:
        return impl.sql_now_postgresql()
    return fallbacks.sql_now_postgresql()


def feed_tag_uri(hostname: str, date_suffix: str, path: str, fragment: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.feed_tag_uri(hostname or "", date_suffix, path or "", fragment or "")
    return fallbacks.feed_tag_uri(hostname or "", date_suffix, path or "", fragment or "")


def progress_percent(count: int, total: int) -> int:
    impl = _impl()
    if impl is not None:
        return impl.progress_percent(int(count), int(total))
    return fallbacks.progress_percent(count, total)


def progress_done_width(percent: int, width: int) -> int:
    impl = _impl()
    if impl is not None:
        return impl.progress_done_width(int(percent), int(width))
    return fallbacks.progress_done_width(percent, width)


def backend_vendor_is(vendor: str, expected: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.backend_vendor_is(vendor, expected)
    return fallbacks.backend_vendor_is(vendor, expected)


def management_prog(basename: str, subcommand: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.management_prog(basename, subcommand)
    return fallbacks.management_prog(basename, subcommand)


def filefield_default_max_length() -> int:
    impl = _impl()
    if impl is not None:
        return impl.filefield_default_max_length()
    return fallbacks.filefield_default_max_length()


def jsonfield_internal_type() -> str:
    impl = _impl()
    if impl is not None:
        return impl.jsonfield_internal_type()
    return fallbacks.jsonfield_internal_type()


def test_label_looks_like_path(label: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.test_label_looks_like_path(label)
    return fallbacks.test_label_looks_like_path(label)


def debug_template_path(name: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.debug_template_path(name)
    return fallbacks.debug_template_path(name)


def date_year_in_range(year: int) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.date_year_in_range(int(year))
    return fallbacks.date_year_in_range(year)


def unique_constraint_name(model: str, fields_joined: str) -> str:
    impl = _impl()
    if impl is not None:
        return impl.unique_constraint_name(model, fields_joined)
    return fallbacks.unique_constraint_name(model, fields_joined)


def db_host_is_unix_socket(host: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.db_host_is_unix_socket(host or "")
    return fallbacks.db_host_is_unix_socket(host or "")


def postgres_set_timezone_sql() -> str:
    impl = _impl()
    if impl is not None:
        return impl.postgres_set_timezone_sql()
    return fallbacks.postgres_set_timezone_sql()


def mysql_isolation_level_valid(level: str) -> bool:
    impl = _impl()
    if impl is not None:
        return impl.mysql_isolation_level_valid(level)
    return fallbacks.mysql_isolation_level_valid(level)


def simple_select_eq_limit_sql(
    quoted_table: str, quoted_cols, quoted_where_col: str, limit: int
) -> str:
    impl = _impl()
    cols = [str(c) for c in quoted_cols]
    if impl is not None:
        return impl.simple_select_eq_limit_sql(
            str(quoted_table), cols, str(quoted_where_col), int(limit)
        )
    return fallbacks.simple_select_eq_limit_sql(
        quoted_table, cols, quoted_where_col, limit
    )


def simple_select_all_sql(quoted_table: str, quoted_cols, limit: int = 0) -> str:
    impl = _impl()
    cols = [str(c) for c in quoted_cols]
    if impl is not None:
        return impl.simple_select_all_sql(str(quoted_table), cols, int(limit or 0))
    return fallbacks.simple_select_all_sql(quoted_table, cols, limit)


def simple_select_in_sql(
    quoted_table: str, quoted_cols, quoted_where_col: str, n_placeholders: int
) -> str:
    impl = _impl()
    cols = [str(c) for c in quoted_cols]
    if impl is not None:
        return impl.simple_select_in_sql(
            str(quoted_table), cols, str(quoted_where_col), int(n_placeholders)
        )
    return fallbacks.simple_select_in_sql(
        quoted_table, cols, quoted_where_col, n_placeholders
    )


def simple_update_eq_sql(quoted_table: str, quoted_set_cols, quoted_where_col: str) -> str:
    impl = _impl()
    cols = [str(c) for c in quoted_set_cols]
    if impl is not None:
        return impl.simple_update_eq_sql(
            str(quoted_table), cols, str(quoted_where_col)
        )
    return fallbacks.simple_update_eq_sql(quoted_table, cols, quoted_where_col)


def render_fortune_page(rows) -> str:
    """
    Build the TechEmpower fortunes HTML page.

    ``rows`` is an iterable of ``(id, message)`` pairs, already sorted by
    message. Messages are HTML-escaped.
    """
    impl = _impl()
    pairs = [(int(i), str(m)) for i, m in rows]
    if impl is not None:
        return impl.render_fortune_page(pairs)
    return fallbacks.render_fortune_page(pairs)
