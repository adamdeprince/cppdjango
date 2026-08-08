"""
Pure-Python implementations of the native acceleration API.

These are always available and are used when the compiled extension is missing
or when native acceleration is disabled (``DJANGO_NATIVE=0``).
"""

from __future__ import annotations

import datetime
import html as html_stdlib
import re
import unicodedata
from urllib.parse import parse_qsl as urllib_parse_qsl

from django.utils.regex_helper import _lazy_re_compile
from django.utils.timezone import get_fixed_timezone

__all__ = [
    "add",
    "compiler",
    "cxx_standard",
    "escapejs",
    "get_valid_filename",
    "html_escape",
    "parse_date",
    "parse_datetime",
    "parse_duration",
    "compile_route",
    "converter_int_to_python",
    "converter_int_to_url",
    "converter_uuid_to_python",
    "converter_uuid_to_url",
    "match_route",
    "parse_qsl",
    "parse_time",
    "slugify",
    "slugify_core",
    "version",
]


def add(a: int, b: int) -> int:
    """Integer addition (pure-Python scaffold counterpart)."""
    return a + b


def version() -> str:
    """Package version when the extension is not loaded."""
    from django import __version__

    return __version__


def cxx_standard() -> str:
    """Marker that the pure-Python path is active."""
    return "python"


def compiler() -> str:
    """Marker that no C++ compiler path is active."""
    return "none"


def html_escape(text: str) -> str:
    """Match ``html.escape(text, quote=True)`` / ``django.utils.html.escape``."""
    return html_stdlib.escape(text)


_js_escapes = {
    ord("\\"): "\\u005C",
    ord("'"): "\\u0027",
    ord('"'): "\\u0022",
    ord(">"): "\\u003E",
    ord("<"): "\\u003C",
    ord("&"): "\\u0026",
    ord("="): "\\u003D",
    ord("-"): "\\u002D",
    ord(";"): "\\u003B",
    ord("`"): "\\u0060",
    ord("\u2028"): "\\u2028",
    ord("\u2029"): "\\u2029",
}
_js_escapes.update((ord("%c" % z), "\\u%04X" % z) for z in range(32))


def escapejs(value: str) -> str:
    """Hex encode characters for use in JavaScript strings."""
    return value.translate(_js_escapes)


def slugify_core(value: str, allow_unicode: bool = False) -> str:
    """
    Core slugify steps after normalization (and lowercasing when unicode).

    ASCII mode still lowercases here to mirror the C++ core.
    """
    if not allow_unicode:
        value = value.lower()
    value = re.sub(r"[^\w\s-]", "", value)
    return re.sub(r"[-\s]+", "-", value).strip("-_")


def slugify(value: str, allow_unicode: bool = False) -> str:
    """Full slugify including Unicode normalization."""
    if allow_unicode:
        value = unicodedata.normalize("NFKC", value).lower()
    else:
        value = (
            unicodedata.normalize("NFKD", value)
            .encode("ascii", "ignore")
            .decode("ascii")
        )
    return slugify_core(value, allow_unicode=allow_unicode)


def get_valid_filename(name: str) -> str | None:
    """Return sanitized filename, or None if empty / '.' / '..'."""
    s = str(name).strip().replace(" ", "_")
    s = re.sub(r"(?u)[^-\w.]", "", s)
    if s in {"", ".", ".."}:
        return None
    return s


# --- dateparse (mirrors django.utils.dateparse regex paths) -----------------

_date_re = _lazy_re_compile(r"(?P<year>\d{4})-(?P<month>\d{1,2})-(?P<day>\d{1,2})$")
_time_re = _lazy_re_compile(
    r"(?P<hour>\d{1,2}):(?P<minute>\d{1,2})"
    r"(?::(?P<second>\d{1,2})(?:[.,](?P<microsecond>\d{1,6})\d{0,6})?)?$"
)
_datetime_re = _lazy_re_compile(
    r"(?P<year>\d{4})-(?P<month>\d{1,2})-(?P<day>\d{1,2})"
    r"[T ](?P<hour>\d{1,2}):(?P<minute>\d{1,2})"
    r"(?::(?P<second>\d{1,2})(?:[.,](?P<microsecond>\d{1,6})\d{0,6})?)?"
    r"\s*(?P<tzinfo>Z|[+-]\d{2}(?::?\d{2})?)?$"
)
_standard_duration_re = _lazy_re_compile(
    r"^"
    r"(?:(?P<days>-?\d+) (days?, )?)?"
    r"(?P<sign>-?)"
    r"((?:(?P<hours>\d+):)(?=\d+:\d+))?"
    r"(?:(?P<minutes>\d+):)?"
    r"(?P<seconds>\d+)"
    r"(?:[.,](?P<microseconds>\d{1,6})\d{0,6})?"
    r"$"
)
_iso8601_duration_re = _lazy_re_compile(
    r"^(?P<sign>[-+]?)"
    r"P"
    r"(?:(?P<days>\d+([.,]\d+)?)D)?"
    r"(?:T"
    r"(?:(?P<hours>\d+([.,]\d+)?)H)?"
    r"(?:(?P<minutes>\d+([.,]\d+)?)M)?"
    r"(?:(?P<seconds>\d+([.,]\d+)?)S)?"
    r")?"
    r"$"
)
_postgres_interval_re = _lazy_re_compile(
    r"^"
    r"(?:(?P<days>-?\d+) (days? ?))?"
    r"(?:(?P<sign>[-+])?"
    r"(?P<hours>\d+):"
    r"(?P<minutes>\d\d):"
    r"(?P<seconds>\d\d)"
    r"(?:\.(?P<microseconds>\d{1,6}))?"
    r")?$"
)


def parse_date(value: str) -> datetime.date | None:
    if match := _date_re.match(value):
        kw = {k: int(v) for k, v in match.groupdict().items()}
        return datetime.date(**kw)
    return None


def parse_time(value: str) -> datetime.time | None:
    if match := _time_re.match(value):
        kw = match.groupdict()
        kw["microsecond"] = kw["microsecond"] and kw["microsecond"].ljust(6, "0")
        kw = {k: int(v) for k, v in kw.items() if v is not None}
        return datetime.time(**kw)
    return None


def parse_datetime(value: str) -> datetime.datetime | None:
    if match := _datetime_re.match(value):
        kw = match.groupdict()
        kw["microsecond"] = kw["microsecond"] and kw["microsecond"].ljust(6, "0")
        tzinfo = kw.pop("tzinfo")
        if tzinfo == "Z":
            tzinfo = datetime.UTC
        elif tzinfo is not None:
            offset_mins = int(tzinfo[-2:]) if len(tzinfo) > 3 else 0
            offset = 60 * int(tzinfo[1:3]) + offset_mins
            if tzinfo[0] == "-":
                offset = -offset
            tzinfo = get_fixed_timezone(offset)
        kw = {k: int(v) for k, v in kw.items() if v is not None}
        return datetime.datetime(**kw, tzinfo=tzinfo)
    return None


def parse_duration(value: str) -> datetime.timedelta | None:
    match = (
        _standard_duration_re.match(value)
        or _iso8601_duration_re.match(value)
        or _postgres_interval_re.match(value)
    )
    if not match:
        return None
    kw = match.groupdict()
    sign = -1 if kw.pop("sign", "+") == "-" else 1
    if kw.get("microseconds"):
        kw["microseconds"] = kw["microseconds"].ljust(6, "0")
    kw = {k: float(v.replace(",", ".")) for k, v in kw.items() if v is not None}
    days = datetime.timedelta(kw.pop("days", 0.0) or 0.0)
    if match.re == _iso8601_duration_re:
        days *= sign
    return days + sign * datetime.timedelta(**kw)


def parse_qsl(
    qs: str,
    *,
    encoding: str = "utf-8",
    max_num_fields: int | None = None,
) -> list[tuple[str, str]]:
    """urllib.parse.parse_qsl with Django's QueryDict defaults."""
    return urllib_parse_qsl(
        qs,
        keep_blank_values=True,
        encoding=encoding,
        errors="replace",
        max_num_fields=max_num_fields,
    )


def compile_route(route: str, is_endpoint: bool = False):
    """Pure-Python: no compiled native route (always use regex path)."""
    return None


def match_route(route, path: str):
    return None


def converter_int_to_python(value: str) -> int:
    return int(value)


def converter_int_to_url(value) -> str:
    return str(value)


def converter_uuid_to_python(value: str):
    import uuid

    return uuid.UUID(value)


def converter_uuid_to_url(value) -> str:
    return str(value).lower()


def converter_str_to_python(value: str) -> str:
    return value


def converter_str_to_url(value) -> str:
    return value if isinstance(value, str) else str(value)


def converter_slug_to_python(value: str) -> str:
    import re

    if not re.fullmatch(r"[-a-zA-Z0-9_]+", value or ""):
        raise ValueError("invalid slug")
    return value


def converter_slug_to_url(value) -> str:
    return converter_slug_to_python(value if isinstance(value, str) else str(value))


def converter_path_to_python(value: str) -> str:
    if not value:
        raise ValueError("invalid path")
    return value


def converter_path_to_url(value) -> str:
    s = value if isinstance(value, str) else str(value)
    if not s:
        raise ValueError("invalid path")
    return s


def reverse_quote(decoded: str) -> str:
    from urllib.parse import quote

    from django.utils.http import RFC3986_SUBDELIMS

    url = quote(decoded, safe=RFC3986_SUBDELIMS + "/~:@")
    return escape_leading_slashes(url)


def is_valid_slug(value: str) -> bool:
    import re

    return bool(re.fullmatch(r"[-a-zA-Z0-9_]+", value or ""))


def is_valid_integer_string(value: str) -> bool:
    import re

    return bool(re.fullmatch(r"-?\d+", value or ""))


def form_integer_to_python(value):
    import re

    try:
        return int(re.sub(r"\.0*\s*$", "", str(value)))
    except (ValueError, TypeError):
        return None


def is_valid_ipv4(value: str) -> bool:
    import ipaddress

    try:
        ipaddress.IPv4Address(value)
        return True
    except ValueError:
        return False


def is_valid_ipv6(value: str) -> bool:
    from django.utils.ipv6 import is_valid_ipv6_address

    return is_valid_ipv6_address(value)


def clean_ipv6_address(ip: str, unpack_ipv4: bool = False, max_length: int = 39):
    """Pure-Python clean; returns None on failure (caller raises ValidationError)."""
    try:
        from django.utils.ipv6 import clean_ipv6_address as _clean

        return _clean(ip, unpack_ipv4=unpack_ipv4, max_length=max_length)
    except Exception:
        return None


def is_valid_ipv46(value: str) -> bool:
    return is_valid_ipv4(value) or is_valid_ipv6(value)


def is_valid_email(value: str, allowlist=None) -> bool:
    # Lightweight pure check without re-entering EmailValidator (native dual-path).
    if not value or "@" not in value or len(value) > 320:
        return False
    user, _, domain = value.rpartition("@")
    if not user or not domain:
        return False
    al = allowlist or ["localhost"]
    if domain in al:
        return True
    if domain.startswith("[") and domain.endswith("]"):
        return is_valid_ipv46(domain[1:-1])
    return "." in domain and " " not in domain


def has_null_characters(value: str) -> bool:
    return "\x00" in value


def char_field_strip(value: str, strip: bool = True) -> str:
    return value.strip() if strip else value


def b62_encode(value: int) -> str:
    alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    if value == 0:
        return "0"
    sign = "-" if value < 0 else ""
    s = abs(value)
    encoded = ""
    while s > 0:
        s, rem = divmod(s, 62)
        encoded = alphabet[rem] + encoded
    return sign + encoded


def b62_decode(s: str) -> int:
    alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    if s == "0":
        return 0
    sign = 1
    if s[:1] == "-":
        sign = -1
        s = s[1:]
    decoded = 0
    for digit in s:
        decoded = decoded * 62 + alphabet.index(digit)
    return sign * decoded


def signing_b64_encode(data: bytes) -> bytes:
    import base64

    return base64.urlsafe_b64encode(data).strip(b"=")


def signing_b64_decode(data: str) -> bytes:
    import base64

    pad = "=" * (-len(data) % 4)
    return base64.urlsafe_b64decode(data + pad)


def constant_time_compare(a, b) -> bool:
    import secrets

    from django.utils.encoding import force_bytes

    return secrets.compare_digest(force_bytes(a), force_bytes(b))


def signer_sep_unsafe(sep: str) -> bool:
    import re

    return bool(re.fullmatch(r"[A-z0-9-_=]*", sep or ""))


def truncate_chars(text: str, length: int, truncate_suffix: str = "…") -> str:
    if length <= 0:
        return ""
    if len(text) <= length:
        return text
    # Simplified fallback
    cut = max(0, length - len(truncate_suffix))
    return text[:cut] + truncate_suffix


def truncate_words(text: str, length: int, truncate_suffix: str = "…") -> str:
    words = text.split()
    if len(words) <= length:
        return " ".join(words)
    return " ".join(words[:length]) + truncate_suffix


def querydict_urlencode(pairs, safe: str = "") -> str:
    from urllib.parse import quote, urlencode

    if safe:
        return "&".join(f"{quote(k, safe)}={quote(v, safe)}" for k, v in pairs)
    return "&".join(urlencode({k: v}) for k, v in pairs)


def url_precheck(value: str, max_length: int) -> bool:
    if len(value) > max_length:
        return False
    return not any(c in value for c in "\t\r\n")


def url_structure_precheck(value: str, max_length: int, schemes_csv: str) -> bool:
    if not url_precheck(value, max_length):
        return False
    if "://" not in value:
        return False
    scheme = value.split("://", 1)[0].lower()
    schemes = {s.strip().lower() for s in schemes_csv.split(",") if s.strip()}
    return scheme in schemes and len(value.split("://", 1)[1]) > 0


def is_valid_domain_name(value: str, accept_idna: bool = True, max_length: int = 255) -> bool:
    if not isinstance(value, str) or not value or len(value) > max_length:
        return False
    if not accept_idna and not value.isascii():
        return False
    # Defer to DomainNameValidator regex would recurse; structural approx only.
    labels = value.rstrip(".").split(".")
    if len(labels) < 2:
        return False
    for lab in labels:
        if not lab or len(lab) > 63 or lab.startswith("-") or lab.endswith("-"):
            return False
    return True


def urlize_simple_url_match(middle: str) -> bool:
    import re

    return bool(re.match(r"^https?://\[?\w", middle, re.I))


def urlize_simple_url_2_match(middle: str) -> bool:
    import re

    return bool(
        re.match(
            r"^www\.|^(?!http).+\.(com|edu|gov|int|mil|net|org)($|/.*)$",
            middle,
            re.I,
        )
    )


def trim_urlize_punctuation(word: str):
    middle = word.lstrip("([")
    lead = word[: len(word) - len(middle)]
    trail = ""
    while middle and middle[-1] in ".,:;!)":
        if middle[-1] in ")]":
            # unmatched closers only — simplified
            if middle.count("(") >= middle.count(")") and middle[-1] == ")":
                break
            if middle.count("[") >= middle.count("]") and middle[-1] == "]":
                break
        trail = middle[-1] + trail
        middle = middle[:-1]
    return lead, middle, trail


def salted_hmac_digest(algorithm: str, key_salt: bytes, secret: bytes, value: bytes) -> bytes:
    import hashlib
    import hmac

    hasher = getattr(hashlib, algorithm)
    key = hasher(key_salt + secret).digest()
    return hmac.new(key, msg=value, digestmod=hasher).digest()


def pbkdf2_hmac(algorithm: str, password: bytes, salt: bytes, iterations: int, dklen: int = 0) -> bytes:
    import hashlib

    return hashlib.pbkdf2_hmac(algorithm, password, salt, iterations, dklen or None)


def secure_random_string(length: int, allowed_chars: str) -> str:
    import secrets

    return "".join(secrets.choice(allowed_chars) for _ in range(length))


def trim_url(url: str, limit: int) -> str:
    if len(url) <= limit:
        return url
    return "%s…" % url[: max(0, limit - 1)]


def urlize_word_split(text: str):
    import re

    return re.split(r"""([\s<>"']+)""", text)


def urlize_is_email_simple(value: str) -> bool:
    return is_valid_email(value, [])


def truncate_chars_html(text: str, length: int, suffix: str = "…"):
    if "<" in text:
        return None
    return truncate_chars(text, length, suffix)


def truncate_words_html(text: str, length: int, suffix: str = "…"):
    if "<" in text:
        return None
    return truncate_words(text, length, suffix)


def decimal_digit_counts(digits: str, exponent: int):
    if exponent >= 0:
        d = len(digits)
        if digits != "0":
            d += exponent
        decimals = 0
    else:
        abs_exp = abs(exponent)
        if abs_exp > len(digits):
            d = decimals = abs_exp
        else:
            d = len(digits)
            decimals = abs_exp
    return False, d, decimals, d - decimals


def sanitize_separators_ascii(value, decimal_sep, thousand_sep, use_thousand):
    parts = []
    if decimal_sep in value:
        value, decimals = value.split(decimal_sep, 1)
        parts.append(decimals)
    if use_thousand and thousand_sep:
        value = value.replace(thousand_sep, "")
    parts.append(value)
    return ".".join(reversed(parts))


def querydict_urlencode_bytes(pairs, safe: str = "") -> str:
    from urllib.parse import quote

    out = []
    for k, v in pairs:
        # k,v are latin-1 views of raw bytes
        kb = k.encode("latin-1")
        vb = v.encode("latin-1")
        out.append(
            "%s=%s"
            % (
                quote(kb, safe=safe.encode("ascii") if safe else b""),
                quote(vb, safe=safe.encode("ascii") if safe else b""),
            )
        )
    return "&".join(out)


def template_tokenize(source: str, with_position: bool = False):
    """Pure-Python template lexer via django.template.base.Lexer."""
    from django.template.base import DebugLexer, Lexer, TokenType

    # Avoid recursion: call the Python path on Lexer directly.
    lexer_cls = DebugLexer if with_position else Lexer
    lexer = lexer_cls.__new__(lexer_cls)
    lexer.template_string = source
    lexer.verbatim = False
    tokens = Lexer._tokenize_python(lexer, with_position)
    out = []
    for t in tokens:
        pos_start = pos_end = None
        if t.position is not None:
            pos_start, pos_end = t.position
        out.append((t.token_type.value, t.contents, t.lineno, pos_start, pos_end))
    return out


def find_multipart_boundary(data: bytes, boundary: bytes):
    index = data.find(boundary)
    if index < 0:
        return None
    end = index
    next_ = index + len(boundary)
    last = max(0, end - 1)
    if data[last : last + 1] == b"\n":
        end -= 1
    last = max(0, end - 1)
    if data[last : last + 1] == b"\r":
        end -= 1
    return end, next_


def boundary_chunk_slice(data: bytes, boundary: bytes, rollback: int):
    found = find_multipart_boundary(data, boundary)
    if found:
        end, next_ = found
        return True, True, end, next_
    if rollback == 0 or len(data) <= rollback:
        return False, True, len(data), len(data)
    return False, False, len(data) - rollback, len(data) - rollback


def find_header_block_end(chunk: bytes):
    pos = chunk.find(b"\r\n\r\n")
    return None if pos < 0 else pos


def sanitize_multipart_filename(file_name: str) -> str | None:
    file_name = file_name.rsplit("/")[-1]
    file_name = file_name.rsplit("\\")[-1]
    file_name = "".join([char for char in file_name if char.isprintable()])
    if file_name in {"", ".", ".."}:
        return None
    return file_name


def split_multipart_parts(body: bytes, separator: bytes):
    parts = []
    pos = 0
    while pos < len(body):
        index = body.find(separator, pos)
        if index < 0:
            break
        end = index
        if end > pos and body[end - 1 : end] == b"\n":
            end -= 1
        if end > pos and body[end - 1 : end] == b"\r":
            end -= 1
        if end > pos:
            start = pos
            if start + 1 < end and body[start : start + 2] == b"\r\n":
                start += 2
            elif start < end and body[start : start + 1] == b"\n":
                start += 1
            if end > start:
                parts.append(body[start:end])
        pos = index + len(separator)
        if pos + 1 < len(body) and body[pos : pos + 2] == b"--":
            break
        if pos + 1 < len(body) and body[pos : pos + 2] == b"\r\n":
            pos += 2
        elif pos < len(body) and body[pos : pos + 1] == b"\n":
            pos += 1
    return parts


def parse_header_parameters(line: str, max_length: int | None = 10_000):
    # Inline pure-Python algorithm (avoid recursion through django.utils.http).
    from urllib.parse import unquote

    from django.utils.http import _parseparam

    if max_length is None:
        max_length_arg = None
    else:
        max_length_arg = max_length

    if not line:
        return "", {}
    if max_length_arg is not None and len(line) > max_length_arg:
        raise ValueError("Unable to parse header parameters (value too long).")
    parts = _parseparam(";" + line)
    key = parts.__next__().lower()
    pdict = {}
    for p in parts:
        i = p.find("=")
        if i >= 0:
            has_encoding = False
            name = p[:i].strip().lower()
            if name.endswith("*"):
                name = name[:-1]
                if p.count("'") == 2:
                    has_encoding = True
            value = p[i + 1 :].strip()
            if len(value) >= 2 and value[0] == value[-1] == '"':
                value = value[1:-1]
                value = value.replace("\\\\", "\\").replace('\\"', '"')
            if has_encoding:
                encoding, lang, value = value.split("'")
                value = unquote(value, encoding=encoding)
            pdict[name] = value
    return key, pdict


def parse_multipart_headers(header_block: bytes):
    from django.http.multipartparser import FIELD, FILE, RAW

    TYPE = RAW
    outdict = {}
    for line in header_block.split(b"\r\n"):
        try:
            header_name, value_and_params = line.decode().split(":", 1)
            name = header_name.lower().rstrip(" ")
            value, params = parse_header_parameters(
                value_and_params.lstrip(" "), max_length=None
            )
            params = {k: v.encode() for k, v in params.items()}
        except ValueError:
            continue
        if name == "content-disposition":
            TYPE = FIELD
            if params.get(b"filename") or params.get("filename"):
                TYPE = FILE
        outdict[name] = value, params
    type_int = {RAW: 0, FIELD: 1, FILE: 2}[TYPE]
    # Normalize type: FILE check used bytes key after encode - fix
    # params keys are str before encode; after encode still str keys in our code
    return type_int, outdict


def parse_multipart_message(body: bytes, boundary: bytes):
    separator = b"--" + boundary
    raw_parts = split_multipart_parts(body, separator)
    parts = []
    for raw in raw_parts:
        header_end = raw.find(b"\r\n\r\n")
        if header_end < 0:
            parts.append(
                {
                    "type": 0,
                    "headers": {},
                    "body": raw,
                    "name": "",
                    "filename": "",
                    "content_type": "",
                    "transfer_encoding": "",
                }
            )
            continue
        type_int, headers = parse_multipart_headers(raw[:header_end])
        body_part = raw[header_end + 4 :]
        name = filename = content_type = transfer_encoding = ""
        if "content-disposition" in headers:
            _val, params = headers["content-disposition"]
            n = params.get("name") or params.get(b"name")
            if isinstance(n, bytes):
                n = n.decode("utf-8", "replace")
            name = n or ""
            f = params.get("filename") or params.get(b"filename")
            if isinstance(f, bytes):
                f = f.decode("utf-8", "replace")
            filename = f or ""
        if "content-type" in headers:
            content_type, _p = headers["content-type"]
        if "content-transfer-encoding" in headers:
            transfer_encoding, _p = headers["content-transfer-encoding"]
        parts.append(
            {
                "type": type_int,
                "headers": headers,
                "body": body_part,
                "name": name,
                "filename": filename,
                "content_type": content_type,
                "transfer_encoding": transfer_encoding,
            }
        )
    return parts


def smart_split(text: str):
    from django.utils.text import smart_split_re

    return [bit[0] for bit in smart_split_re.finditer(str(text))]


def unescape_string_literal(s: str) -> str:
    if not s or s[0] not in "\"'" or s[-1] != s[0]:
        raise ValueError("Not a string literal: %r" % s)
    quote = s[0]
    return s[1:-1].replace(r"\%s" % quote, quote).replace(r"\\", "\\")


def parse_variable(var: str):
    # Minimal pure-Python classifier used when extension is missing.
    return {
        "kind": 4,
        "translate": False,
        "int_value": 0,
        "float_value": 0.0,
        "string_value": "",
        "lookups": [],
        "error": "fallback",
        "error_detail": "",
    }


def parse_filter_expression(token: str):
    raise ValueError("native parse_filter_expression unavailable")


def resolve_dict_lookups(context, lookups):
    return (False, None)


def filter_addslashes(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"').replace("'", "\\'")


def filter_capfirst(value: str) -> str:
    return value and value[0].upper() + value[1:]


def filter_lower(value: str) -> str:
    return value.lower()


def filter_upper(value: str) -> str:
    return value.upper()


def filter_cut(value: str, arg: str) -> str:
    return value.replace(arg, "")


def filter_wordcount(value: str) -> int:
    return len(value.split())


def filter_ljust(value: str, width: int) -> str:
    return value.ljust(width)


def filter_rjust(value: str, width: int) -> str:
    return value.rjust(width)


def filter_center(value: str, width: int) -> str:
    if width <= 0:
        return value
    return f"{value:^{width}}"


def url_quote(value: str, safe: str = "") -> str:
    from urllib.parse import quote

    return quote(value, safe=safe)


def escape_leading_slashes(url: str) -> str:
    if url.startswith("//"):
        return "/%2F{}".format(url.removeprefix("//"))
    return url


def context_lookup(dicts, key):
    for d in reversed(dicts):
        if key in d:
            return True, d[key]
    return False, None


def phone2numeric(phone: str) -> str:
    char2number = {
        "a": "2",
        "b": "2",
        "c": "2",
        "d": "3",
        "e": "3",
        "f": "3",
        "g": "4",
        "h": "4",
        "i": "4",
        "j": "5",
        "k": "5",
        "l": "5",
        "m": "6",
        "n": "6",
        "o": "6",
        "p": "7",
        "q": "7",
        "r": "7",
        "s": "7",
        "t": "8",
        "u": "8",
        "v": "8",
        "w": "9",
        "x": "9",
        "y": "9",
        "z": "9",
    }
    return "".join(char2number.get(c, c) for c in phone.lower())


def normalize_newlines(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def parse_cookie(cookie: str) -> dict:
    from http import cookies

    cookiedict = {}
    for chunk in cookie.split(";"):
        if "=" in chunk:
            key, val = chunk.split("=", 1)
        else:
            key, val = "", chunk
        key, val = key.strip(), val.strip()
        if key or val:
            cookiedict[key] = cookies._unquote(val)
    return cookiedict


def cookie_unquote(value: str) -> str:
    from http import cookies

    return cookies._unquote(value)


def parse_http_date(date: str) -> int:
    from datetime import UTC, datetime

    from django.utils.http import ASCTIME_DATE, MONTHS, RFC1123_DATE, RFC850_DATE

    for regex in RFC1123_DATE, RFC850_DATE, ASCTIME_DATE:
        m = regex.match(date)
        if m is not None:
            break
    else:
        raise ValueError("%r is not in a valid HTTP date format" % date)
    year = int(m["year"])
    if year < 100:
        current_year = datetime.now(tz=UTC).year
        current_century = current_year - (current_year % 100)
        if year - (current_year % 100) > 50:
            year += current_century - 100
        else:
            year += current_century
    month = MONTHS.index(m["mon"].lower()) + 1
    day = int(m["day"])
    hour = int(m["hour"])
    min = int(m["min"])
    sec = int(m["sec"])
    result = datetime(year, month, day, hour, min, sec, tzinfo=UTC)
    return int(result.timestamp())


def http_date(epoch_seconds=None) -> str:
    from email.utils import formatdate

    return formatdate(epoch_seconds, usegmt=True)


def base36_to_int(s: str) -> int:
    if len(s) > 13:
        raise ValueError("Base36 input too large")
    return int(s, 36)


def int_to_base36(i: int) -> str:
    char_set = "0123456789abcdefghijklmnopqrstuvwxyz"
    if i < 0:
        raise ValueError("Negative base36 conversion input.")
    if i < 36:
        return char_set[i]
    b36 = ""
    while i != 0:
        i, n = divmod(i, 36)
        b36 = char_set[n] + b36
    return b36


def parse_etags(etag_str: str):
    from django.utils.http import ETAG_MATCH

    if etag_str.strip() == "*":
        return ["*"]
    etag_matches = (ETAG_MATCH.match(etag.strip()) for etag in etag_str.split(","))
    return [match[1] for match in etag_matches if match]


def quote_etag(etag_str: str) -> str:
    from django.utils.http import ETAG_MATCH

    if ETAG_MATCH.match(etag_str):
        return etag_str
    return '"%s"' % etag_str


def is_same_domain(host: str, pattern: str) -> bool:
    if not pattern:
        return False
    pattern = pattern.lower()
    return (
        pattern[0] == "."
        and (host.endswith(pattern) or host == pattern[1:])
        or pattern == host
    )


def split_domain_port(host: str):
    import re

    host_validation_re = re.compile(
        r"^([a-z0-9.-]+|\[[a-f0-9]*:[a-f0-9.:]+\])(?::([0-9]+))?$"
    )
    if match := host_validation_re.fullmatch(host.lower()):
        domain, port = match.groups(default="")
        return domain.removesuffix("."), port
    return "", ""


def validate_host(host: str, allowed_hosts) -> bool:
    return any(
        pattern == "*" or is_same_domain(host, pattern) for pattern in allowed_hosts
    )


def content_disposition_header(as_attachment: bool, filename=None):
    import re
    from urllib.parse import quote

    if filename:
        disposition = "attachment" if as_attachment else "inline"
        try:
            filename.encode("ascii")
            is_ascii = True
        except UnicodeEncodeError:
            is_ascii = False
        if is_ascii and re.match(r"^[\t \x21-\x7e]*$", filename):
            file_expr = 'filename="{}"'.format(
                filename.replace("\\", "\\\\").replace('"', r"\"")
            )
        else:
            file_expr = "filename*=utf-8''{}".format(quote(filename))
        return f"{disposition}; {file_expr}"
    elif as_attachment:
        return "attachment"
    return None


def urlsafe_base64_encode(s: bytes) -> str:
    import base64

    return base64.urlsafe_b64encode(s).rstrip(b"\n=").decode("ascii")


def urlsafe_base64_decode(s: str) -> bytes:
    import base64
    from binascii import Error as BinasciiError

    s = s.encode()
    try:
        return base64.urlsafe_b64decode(s.ljust(len(s) + len(s) % 4, b"="))
    except (LookupError, BinasciiError) as e:
        raise ValueError(e)


def strip_spaces_between_tags(value: str) -> str:
    import re

    return re.sub(r">\s+<", "><", value)


def camel_case_to_spaces(value: str) -> str:
    import re

    return re.sub(r"(((?<=[a-z])[A-Z])|([A-Z](?![A-Z]|$)))", r" \1", value).strip().lower()


def pluralize_suffix(singular: bool, arg: str) -> str:
    if "," not in arg:
        arg = "," + arg
    bits = arg.split(",")
    if len(bits) > 2:
        return ""
    singular_suffix, plural_suffix = bits[:2]
    return singular_suffix if singular else plural_suffix


def yesno(tri_state: int, arg: str) -> str:
    bits = arg.split(",")
    if len(bits) < 2:
        return ""
    yes, no = bits[0], bits[1]
    maybe = bits[2] if len(bits) >= 3 else bits[1]
    if tri_state < 0:
        return maybe
    return yes if tri_state else no


def get_digit(value, arg):
    try:
        arg = int(arg)
        value = int(value)
    except ValueError:
        return value
    if arg < 1:
        return value
    try:
        return int(str(value)[-arg])
    except IndexError:
        return 0


def widthratio(value: float, max_value: float, max_width: int) -> str:
    if max_value == 0:
        return "0"
    return str(round((value / max_value) * max_width))


def get_mod_func(callback: str):
    try:
        dot = callback.rindex(".")
    except ValueError:
        return callback, ""
    return callback[:dot], callback[dot + 1 :]


def iri_to_uri(iri):
    from urllib.parse import quote

    if iri is None:
        return iri
    return quote(str(iri), safe="/#%[]=:;$&()+,!?*@'~")


def uri_to_iri(uri):
    """Pure-Python selective unquote (mirrors django.utils.encoding.uri_to_iri)."""
    from urllib.parse import quote

    from django.utils.encoding import force_bytes, repercent_broken_unicode

    if uri is None:
        return uri
    uri = force_bytes(uri)
    bits = uri.split(b"%")
    if len(bits) == 1:
        iri = uri
    else:
        # Inline the same hex table policy as encoding._hextobyte for fallbacks.
        from django.utils.encoding import _hextobyte

        parts = [bits[0]]
        for item in bits[1:]:
            hexb = item[:2]
            if hexb in _hextobyte:
                parts.append(_hextobyte[hexb])
                parts.append(item[2:])
            else:
                parts.append(b"%")
                parts.append(item)
        iri = b"".join(parts)
    return repercent_broken_unicode(iri).decode()


def escape_uri_path(path: str) -> str:
    from urllib.parse import quote

    return quote(path, safe="/:@&+$,-_.!~*'()")


def filepath_to_uri(path):
    from urllib.parse import quote

    if path is None:
        return path
    return quote(str(path).replace("\\", "/"), safe="/~!*()'")


def filter_title(value: str) -> str:
    import re

    t = re.sub("([a-z])'([A-Z])", lambda m: m[0].lower(), value.title())
    return re.sub(r"\d([A-Z])", lambda m: m[0].lower(), t)


def filter_slice_string(value: str, start=None, stop=None, step=None) -> str:
    return value[slice(start, stop, step)]


def divisibleby(value, arg) -> bool:
    return int(value) % int(arg) == 0


def filter_add_int(value, arg):
    try:
        return int(value) + int(arg)
    except (ValueError, TypeError):
        return None


def linebreaks(value: str, autoescape: bool = False) -> str:
    import re

    from django.utils.html import escape
    from django.utils.text import normalize_newlines

    value = normalize_newlines(value)
    paras = re.split("\n{2,}", str(value))
    if autoescape:
        paras = ["<p>%s</p>" % escape(p).replace("\n", "<br>") for p in paras]
    else:
        paras = ["<p>%s</p>" % p.replace("\n", "<br>") for p in paras]
    return "\n\n".join(paras)


def linebreaksbr(value: str, autoescape: bool = False) -> str:
    from django.utils.html import escape
    from django.utils.text import normalize_newlines

    value = normalize_newlines(value)
    if autoescape:
        value = escape(value)
    return value.replace("\n", "<br>")


def strip_tags(value: str) -> str:
    # Fall back via HTMLParser without re-entering the dual-path facade.
    from html.parser import HTMLParser

    from django.core.exceptions import SuspiciousOperation
    from django.utils.html import MAX_STRIP_TAGS_DEPTH, long_open_tag_without_closing_re

    class MLStripper(HTMLParser):
        def __init__(self):
            super().__init__(convert_charrefs=False)
            self.reset()
            self.fed = []

        def handle_data(self, d):
            self.fed.append(d)

        def handle_entityref(self, name):
            self.fed.append("&%s;" % name)

        def handle_charref(self, name):
            self.fed.append("&#%s;" % name)

        def get_data(self):
            return "".join(self.fed)

    value = str(value)
    for long_open_tag in long_open_tag_without_closing_re.finditer(value):
        if long_open_tag.group().count("<") >= MAX_STRIP_TAGS_DEPTH:
            raise SuspiciousOperation
    strip_tags_depth = 0
    while "<" in value and ">" in value:
        if strip_tags_depth >= MAX_STRIP_TAGS_DEPTH:
            raise SuspiciousOperation
        s = MLStripper()
        s.feed(value)
        s.close()
        new_value = s.get_data()
        if value.count("<") == new_value.count("<"):
            break
        value = new_value
        strip_tags_depth += 1
    return value


def utf8_length(value: str) -> int:
    return len(value)


def utf8_first(value: str) -> str:
    return value[0] if value else ""


def utf8_last(value: str) -> str:
    return value[-1] if value else ""


def make_list_chars(value: str) -> list:
    return list(value)


def linenumbers(value: str, autoescape: bool = True) -> str:
    from django.utils.html import escape

    lines = value.split("\n")
    width = len(str(len(lines)))
    if autoescape:
        lines = [("%0" + str(width) + "d. %s") % (i + 1, escape(line))
                 for i, line in enumerate(lines)]
    else:
        lines = [("%0" + str(width) + "d. %s") % (i + 1, line)
                 for i, line in enumerate(lines)]
    return "\n".join(lines)


def wordwrap(text: str, width: int) -> str:
    import textwrap

    wrapper = textwrap.TextWrapper(
        width=width,
        break_long_words=False,
        break_on_hyphens=False,
        replace_whitespace=False,
    )
    result = []
    for line in text.splitlines():
        wrapped = wrapper.wrap(line)
        if not wrapped:
            result.append(line)
        else:
            result.extend(wrapped)
    if text.endswith("\n"):
        result.append("")
    return "\n".join(result)


def join_strings(parts, sep: str) -> str:
    return sep.join(parts)


def filter_default(value, arg):
    return value or arg


def filter_default_if_none(value, arg):
    return arg if value is None else value


def sequence_random(value):
    import random

    try:
        return random.choice(value)
    except IndexError:
        return ""


def dictsort(value, arg, reverse: bool = False):
    from django.template.defaultfilters import _property_resolver

    try:
        return sorted(value, key=_property_resolver(arg), reverse=reverse)
    except (AttributeError, TypeError):
        return ""


def unordered_list(value, autoescape: bool = True):
    # Defer to pure-Python filter body without re-entering native.
    from django.utils.html import conditional_escape
    import types

    if autoescape:
        escaper = conditional_escape
    else:

        def escaper(x):
            return x

    def walk_items(item_list):
        item_iterator = iter(item_list)
        try:
            item = next(item_iterator)
            while True:
                try:
                    next_item = next(item_iterator)
                except StopIteration:
                    yield item, None
                    break
                if isinstance(next_item, (list, tuple, types.GeneratorType)):
                    try:
                        iter(next_item)
                    except TypeError:
                        pass
                    else:
                        yield item, next_item
                        item = next(item_iterator)
                        continue
                yield item, None
                item = next_item
        except StopIteration:
            pass

    def list_formatter(item_list, tabs=1):
        indent = "\t" * tabs
        output = []
        for item, children in walk_items(item_list):
            sublist = ""
            if children:
                sublist = "\n%s<ul>\n%s\n%s</ul>\n%s" % (
                    indent,
                    list_formatter(children, tabs + 1),
                    indent,
                    indent,
                )
            output.append("%s<li>%s%s</li>" % (indent, escaper(item), sublist))
        return "\n".join(output)

    return list_formatter(value)


def format_number(
    number: str,
    decimal_sep: str,
    decimal_pos=None,
    grouping=0,
    thousand_sep: str = "",
    use_grouping: bool = False,
) -> str:
    # Minimal pure-Python mirror of numberformat.format string path.
    sign = ""
    str_number = number
    if str_number[:1] == "-":
        sign = "-"
        str_number = str_number[1:]
    if "." in str_number:
        int_part, dec_part = str_number.split(".", 1)
        if decimal_pos is not None:
            dec_part = dec_part[:decimal_pos]
    else:
        int_part, dec_part = str_number, ""
    if decimal_pos is not None:
        dec_part += "0" * (decimal_pos - len(dec_part))
    dec_part = dec_part and decimal_sep + dec_part
    if use_grouping and grouping != 0:
        try:
            intervals = list(grouping)
        except TypeError:
            intervals = [grouping, 0]
        active_interval = intervals.pop(0)
        int_part_gd = ""
        cnt = 0
        for digit in int_part[::-1]:
            if cnt and cnt == active_interval:
                if intervals:
                    active_interval = intervals.pop(0) or active_interval
                int_part_gd += thousand_sep[::-1]
                cnt = 0
            int_part_gd += digit
            cnt += 1
        int_part = int_part_gd[::-1]
    return sign + int_part + dec_part


def php_date_format(parts: dict, format_string: str):
    return None  # force pure-Python DateFormat path


def timesince_partials(d, now, depth: int = 2):
    raise RuntimeError("native timesince unavailable")


def avoid_wrapping(value: str) -> str:
    return value.replace(" ", "\xa0")


def filesize_parts(bytes_):
    try:
        bytes_ = int(bytes_)
    except (TypeError, ValueError, UnicodeDecodeError):
        return None
    negative = bytes_ < 0
    abs_bytes = -bytes_ if negative else bytes_
    KB, MB, GB, TB, PB = 1 << 10, 1 << 20, 1 << 30, 1 << 40, 1 << 50
    if abs_bytes < KB:
        unit, scaled = 0, float(abs_bytes)
    elif abs_bytes < MB:
        unit, scaled = 1, abs_bytes / KB
    elif abs_bytes < GB:
        unit, scaled = 2, abs_bytes / MB
    elif abs_bytes < TB:
        unit, scaled = 3, abs_bytes / GB
    elif abs_bytes < PB:
        unit, scaled = 4, abs_bytes / TB
    else:
        unit, scaled = 5, abs_bytes / PB
    return {
        "negative": negative,
        "unit": unit,
        "abs_bytes": abs_bytes,
        "scaled": scaled,
    }

def cc_delim_split(header: str):
    import re
    return [p for p in re.split(r"\s*,\s*", header) if p]


def parse_cache_control(header: str):
    out = []
    for field in cc_delim_split(header or ""):
        if "=" in field:
            k, v = field.split("=", 1)
            out.append((k.lower(), v))
        else:
            out.append((field.lower(), ""))
    return out


def get_max_age_from_cc(header: str):
    for k, v in parse_cache_control(header or ""):
        if k == "max-age" and v != "":
            try:
                return int(v)
            except (TypeError, ValueError):
                return None
    return None


def if_match_passes(target_etag, etags) -> bool:
    if not target_etag:
        return False
    etags = list(etags)
    if etags == ["*"]:
        return True
    if target_etag.startswith("W/"):
        return False
    return target_etag in etags


def if_none_match_passes(target_etag, etags) -> bool:
    if not target_etag:
        return True
    etags = list(etags)
    if etags == ["*"]:
        return False
    target_etag = target_etag.strip("W/")
    etags = [e.strip("W/") for e in etags]
    return target_etag not in etags


def if_unmodified_since_passes(last_modified, if_unmodified_since) -> bool:
    return bool(last_modified) and last_modified <= if_unmodified_since


def if_modified_since_passes(last_modified, if_modified_since) -> bool:
    return not last_modified or last_modified > if_modified_since


def patch_vary_headers_value(existing_vary: str, newheaders) -> str:
    vary_headers = cc_delim_split(existing_vary) if existing_vary else []
    existing = {h.lower() for h in vary_headers}
    for nh in newheaders:
        if nh.lower() not in existing:
            vary_headers.append(nh)
            existing.add(nh.lower())
    if "*" in vary_headers:
        return "*"
    return ", ".join(vary_headers)


def has_vary_header_value(vary_header: str, header_query: str) -> bool:
    if not vary_header:
        return False
    existing = {h.lower().strip() for h in cc_delim_split(vary_header)}
    return header_query.lower().strip() in existing


def merge_cache_control(existing: str, kwargs_triples) -> str:
    # Minimal fallback: rebuild via pure logic is heavy; re-use empty + kwargs only
    from collections import defaultdict
    cc = defaultdict(set)
    for field in cc_delim_split(existing or ""):
        if "=" in field:
            d, v = field.split("=", 1)
            d = d.lower()
            if d == "no-cache":
                cc[d].add(v)
            else:
                cc[d] = v
        else:
            d = field.lower()
            if d == "no-cache":
                cc[d].add(True)
            else:
                cc[d] = True
    for k, v, is_bool in kwargs_triples:
        directive = k.replace("_", "-").lower()
        if directive == "max-age" and "max-age" in cc:
            try:
                v = str(min(int(cc["max-age"]), int(v)))
            except (TypeError, ValueError):
                pass
        if "private" in cc and directive == "public":
            del cc["private"]
        elif "public" in cc and directive == "private":
            del cc["public"]
        if directive == "no-cache":
            cc[directive].add(True if is_bool else v)
        else:
            cc[directive] = True if is_bool else v
    directives = []
    for directive, values in cc.items():
        if isinstance(values, set):
            if True in values:
                values = {True}
            for value in values:
                directives.append(directive if value is True else f"{directive}={value}")
        else:
            directives.append(directive if values is True else f"{directive}={values}")
    return ", ".join(directives)


def mvd_last_value(values):
    values = list(values)
    if not values:
        return []
    return values[-1]


def node_add_action(self_connector, conn_type, data_is_node, data_negated,
                    data_connector, data_len) -> int:
    if self_connector != conn_type:
        return 0
    if data_is_node and not data_negated and (
        data_connector == conn_type or data_len == 1
    ):
        return 1
    return 2


def form_add_prefix(prefix, field_name: str) -> str:
    return f"{prefix}-{field_name}" if prefix else field_name


def form_add_initial_prefix(prefix, field_name: str) -> str:
    return "initial-%s" % form_add_prefix(prefix, field_name)


def pretty_name(name: str) -> str:
    if not name:
        return ""
    return name.replace("_", " ").capitalize()


def form_auto_id(auto_id, html_name: str) -> str:
    if not auto_id:
        return ""
    auto_id = str(auto_id)
    if "%s" in auto_id:
        return auto_id % html_name
    return html_name


def checkbox_bool_value(key_present: bool, value: str = "") -> bool:
    if not key_present:
        return False
    values = {"true": True, "false": False}
    if isinstance(value, str):
        mapped = values.get(value.lower(), value)
        return bool(mapped)
    return bool(value)


def flatatt_build(key_values, boolean_keys) -> str:
    parts = []
    for k, v in sorted(key_values):
        parts.append(f' {k}="{v}"')
    for k in sorted(boolean_keys):
        parts.append(f" {k}")
    return "".join(parts)


def json_script_escape(json_str: str) -> str:
    return (
        json_str.replace(">", "\u003E")
        .replace("<", "\u003C")
        .replace("&", "\u0026")
    )


def json_script_wrap(escaped_json: str, element_id: str = "") -> str:
    if element_id:
        return f'<script id="{element_id}" type="application/json">{escaped_json}</script>'
    return f'<script type="application/json">{escaped_json}</script>'


def floatformat_simple(decimal_str: str, p: int):
    try:
        v = float(decimal_str)
    except (TypeError, ValueError):
        return None
    if p <= 0 and v == int(v):
        return str(int(v))
    if p < 0 or p > 20:
        return None
    return f"%0.{p}f" % v

def sql_quote_name(name: str, style: str = "double") -> str:
    q = "`" if style in ("backtick", "`") else '"'
    if len(name) >= 2 and name[0] == q and name[-1] == q:
        return name
    return f"{q}{name}{q}"


def where_needed_counts(connector: str, n_children: int):
    if connector == "AND":
        return n_children, 1
    return 1, n_children


def where_combine_sql(connector: str, parts, negated: bool = False, resolved: bool = False) -> str:
    parts = list(parts)
    if not parts:
        return ""
    sql = f" {connector} ".join(parts)
    if negated:
        return f"NOT ({sql})"
    if len(parts) > 1 or resolved:
        return f"({sql})"
    return sql


def sql_in_placeholders(n: int) -> str:
    if n <= 0:
        return ""
    return ", ".join(["%s"] * n)


def sql_isnull_sql(negated: bool = False) -> str:
    return "IS NOT NULL" if negated else "IS NULL"


def sql_comparison_rhs(lookup_name: str) -> str:
    return {
        "exact": " = %s",
        "gt": " > %s",
        "gte": " >= %s",
        "lt": " < %s",
        "lte": " <= %s",
    }.get(lookup_name, "")


def is_form_empty_string(value: str) -> bool:
    return value == ""


def field_str_has_changed(initial: str, data: str) -> bool:
    return initial != data


def boolean_field_to_python(value: str) -> bool:
    if isinstance(value, str) and value.lower() in ("false", "0"):
        return False
    return bool(value)


def null_boolean_to_python(value: str):
    if value in (True, "True", "true", "1"):
        return True
    if value in (False, "False", "false", "0"):
        return False
    return None


def header_key_valid(key: str) -> bool:
    if not key:
        return False
    try:
        key.encode("ascii")
    except UnicodeEncodeError:
        return False
    return "\n" not in key and "\r" not in key


def header_value_no_newlines(value: str) -> bool:
    return "\n" not in value and "\r" not in value


def charset_from_content_type(content_type: str) -> str:
    import re
    m = re.search(r";\s*charset=(?P<charset>[^\s;]+)", content_type, re.I)
    if not m:
        return ""
    return m["charset"].replace('"', "")


def path_ends_with_slash(path: str) -> bool:
    return path.endswith("/")


def force_append_slash_path(full_path: str) -> str:
    if "?" in full_path:
        path, rest = full_path.split("?", 1)
        rest = "?" + rest
    elif "#" in full_path:
        path, rest = full_path.split("#", 1)
        rest = "#" + rest
    else:
        path, rest = full_path, ""
    if path.endswith("/"):
        return full_path
    return path + "/" + rest


def serialize_header_lines(headers) -> list:
    return [f"{k}: {v}" for k, v in headers]


def stringformat_simple(value: str, spec: str):
    try:
        return ("%" + spec) % (value if spec.endswith("s") else float(value) if any(c in spec for c in "fFeEgG") else int(float(value)))
    except Exception:
        return None


def floatformat_ascii(decimal_str: str, p: int):
    try:
        v = float(decimal_str)
    except (TypeError, ValueError):
        return None
    if p <= 0 and v == int(v):
        return str(int(v))
    if p < 0:
        s = f"%0.{abs(p)}f" % v
        if "." in s:
            s = s.rstrip("0").rstrip(".")
        return s
    if p > 20:
        return None
    return f"%0.{p}f" % v

def sql_join_dotted(parts) -> str:
    return ".".join(parts)


def sql_pattern_wrap(value: str, kind: str) -> str:
    if kind in ("contains", "icontains"):
        return f"%{value}%"
    if kind in ("startswith", "istartswith"):
        return f"{value}%"
    if kind in ("endswith", "iendswith"):
        return f"%{value}"
    return value


def choice_valid_value(text_value: str, choice_keys) -> bool:
    return text_value in {str(k) for k in choice_keys}


def is_decimal_string(value: str) -> bool:
    if not value:
        return False
    try:
        float(value)
        return True
    except ValueError:
        return False


def form_float_to_python(value: str):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def is_valid_session_key(key: str, min_length: int = 8, check_charset: bool = False) -> bool:
    if not key or len(key) < min_length:
        return False
    if check_charset:
        return all(c in "abcdefghijklmnopqrstuvwxyz0123456789" for c in key)
    return True


def is_valid_samesite(value: str) -> bool:
    if not value:
        return True
    return value.lower() in ("lax", "none", "strict")


def cookie_delete_secure(key: str, samesite: str = "") -> bool:
    return key.startswith(("__Secure-", "__Host-")) or (
        samesite and samesite.lower() == "none"
    )


def cookie_max_age_seconds(total_seconds: float) -> int:
    return int(total_seconds)


def signing_split(signed_value: str, sep: str = ":"):
    return signed_value.split(sep)


def signing_is_compressed(b64_payload: str) -> bool:
    return bool(b64_payload) and b64_payload[0] == "."


def where_child_outcome(child_kind: int, negated: bool, full_needed: int, empty_needed: int):
    f, e = full_needed, empty_needed
    if child_kind == 1:
        e -= 1
    elif child_kind in (2, 3):
        f -= 1
    if e == 0:
        return (2 if negated else 1), f, e
    if f == 0:
        return (1 if negated else 2), f, e
    return 0, f, e

def sql_comma_join(parts) -> str:
    return ", ".join(parts)


def sql_order_by_clause(parts) -> str:
    return "ORDER BY " + ", ".join(parts) if parts else ""


def sql_group_by_clause(parts) -> str:
    return "GROUP BY " + ", ".join(parts) if parts else ""


def sql_expr_as(expr_sql: str, quoted_alias: str = "") -> str:
    if not quoted_alias:
        return expr_sql
    return f"{expr_sql} AS {quoted_alias}"


def sql_limit_offset_clause(limit, offset: int = 0) -> str:
    parts = []
    if limit is not None and limit != 0:
        parts.append(f"LIMIT {int(limit)}")
    if offset:
        parts.append(f"OFFSET {int(offset)}")
    return " ".join(parts)


def join_promoter_effective_connector(connector: str, negated: bool) -> str:
    if not negated:
        return connector
    return "OR" if connector == "AND" else "AND"


def join_promoter_should_promote(effective_connector: str, votes: int, num_children: int) -> bool:
    return effective_connector == "OR" and votes < num_children


def join_promoter_should_demote(effective_connector: str, votes: int, num_children: int) -> bool:
    if effective_connector == "AND":
        return True
    return effective_connector == "OR" and votes == num_children


def quote_name_is_alias(in_alias_map_not_table: bool, in_extra_select: bool,
                        external_alias_not_table: bool) -> bool:
    return in_alias_map_not_table or in_extra_select or external_alias_not_table


def q_is_empty(n_children: int) -> bool:
    return n_children <= 0


def split_lookup_path(path: str):
    return path.split("__") if path else []


def lookup_path_head(path: str) -> str:
    return path.split("__", 1)[0]


def q_combine_empty_flags(self_empty: bool, other_empty: bool) -> int:
    if self_empty:
        return 1
    if other_empty:
        return 2
    return 0

def join_lookup_path(parts) -> str:
    return "__".join(parts)


def lookup_field_parts(lookup_splitted, n_lookup_parts: int):
    n = len(lookup_splitted)
    keep = max(0, n - n_lookup_parts)
    return list(lookup_splitted[:keep])


def lookup_or_exact(lookups):
    return list(lookups) if lookups else ["exact"]


def refs_expression_match(lookup_parts, annotation_keys):
    keys = set(annotation_keys)
    for n in range(1, len(lookup_parts) + 1):
        key = "__".join(lookup_parts[0:n])
        if key in keys:
            return key, list(lookup_parts[n:])
    return None, ()


def next_numbered_alias(prefix: str, alias_map_size: int) -> str:
    return f"{prefix}{alias_map_size + 1}"


def alias_refcount_add(current: int, amount: int, clamp_non_negative: bool = False) -> int:
    v = current + amount
    if clamp_non_negative and v < 0:
        return 0
    return v


def alias_refcount_increased(pre: dict, post: dict):
    return [k for k, v in post.items() if v > pre.get(k, 0)]


def lookup_invalid_without_field(n_lookup_parts: int, n_field_parts: int) -> bool:
    return n_lookup_parts > 1 and n_field_parts == 0


def split_order_by_item(item: str):
    if item.startswith("-"):
        return item[1:], True
    return item, False

def values_list_flags(flat: bool, named: bool, n_fields: int) -> int:
    if flat and named:
        return 1
    if flat and n_fields > 1:
        return 2
    return 0


def unique_field_alias(base: str, start_counter: int, existing_keys) -> str:
    keys = set(existing_keys)
    counter = start_counter
    candidate = f"{base}{counter}"
    while candidate in keys:
        counter += 1
        candidate = f"{base}{counter}"
    return candidate


def session_cache_key(prefix: str, session_key: str) -> str:
    return prefix + session_key


def session_expiry_age_seconds(cookie_age: int, modification_age=None, expiry=None) -> int:
    # Match Django: not expiry → cookie_age; int → return as remaining seconds.
    if expiry is None or expiry == 0:
        return cookie_age
    return int(expiry)


def session_delta_seconds(days: int, seconds: int) -> int:
    return days * 86400 + seconds


def session_key_missing(session_key: str) -> bool:
    return not session_key


def sql_for_update(no_key=False, nowait=False, skip_locked=False, of=None) -> str:
    of = of or ()
    return "FOR%s UPDATE%s%s%s" % (
        " NO KEY" if no_key else "",
        " OF %s" % ", ".join(of) if of else "",
        " NOWAIT" if nowait else "",
        " SKIP LOCKED" if skip_locked else "",
    )


def sql_combinator_keyword(combinator: str, all: bool = False) -> str:
    mapping = {"union": "UNION", "intersection": "INTERSECT", "difference": "EXCEPT"}
    c = mapping.get(combinator, combinator.upper())
    if all and combinator == "union":
        c += " ALL"
    return c


def sql_combinator_join(combinator_sql: str, parts, wrap_parens: bool = False) -> str:
    if wrap_parens:
        parts = [f"({p})" for p in parts]
    return f" {combinator_sql} ".join(parts)


def sql_distinct_clause(fields, allow_on: bool = False) -> str:
    if not fields:
        return "DISTINCT"
    if not allow_on:
        return ""
    return "DISTINCT ON (" + ", ".join(fields) + ")"


def multi_choice_has_changed(initial, data) -> bool:
    if len(initial) != len(data):
        return True
    return {str(x) for x in initial} != {str(x) for x in data}


def json_looks_valid(value: str) -> bool:
    s = value.lstrip()
    if not s:
        return False
    if s[0] in "{[\"-0123456789":
        return True
    return s.startswith(("true", "false", "null"))


def sql_from_tables(clauses) -> str:
    if not clauses:
        return ""
    return "FROM " + " ".join(clauses)

def queryset_count_from_cache(has_cache: bool, cache_len: int) -> int:
    return cache_len if has_cache else -1


def queryset_exists_from_cache(has_cache: bool, cache_nonempty: bool) -> bool:
    return has_cache and cache_nonempty


def queryset_use_cache_for_first_last(has_cache: bool, ordered: bool, cache_nonempty: bool) -> bool:
    return has_cache and ordered


def iterator_chunk_validate(chunk_size_none: bool, chunk_size: int, has_prefetch: bool) -> int:
    if chunk_size_none:
        return 1 if has_prefetch else 0
    if chunk_size <= 0:
        return 2
    return 0


def iterator_chunk_size_or_default(chunk_size_none: bool, chunk_size: int, default_size: int = 2000) -> int:
    return default_size if chunk_size_none else chunk_size


def in_bulk_empty(id_list_is_none: bool, id_list_len: int) -> bool:
    return (not id_list_is_none) and id_list_len == 0


def in_bulk_filter_key(field_name: str) -> str:
    return f"{field_name}__in"


def in_bulk_batch_ranges(n_ids: int, batch_size: int):
    if n_ids <= 0:
        return []
    if batch_size <= 0 or batch_size >= n_ids:
        return [(0, n_ids)]
    return [(o, min(o + batch_size, n_ids)) for o in range(0, n_ids, batch_size)]


def get_result_kind(num_results: int, limit: int = 0) -> int:
    if num_results == 1:
        return 0
    if num_results == 0:
        return 1
    return 2

def bulk_insert_sql(row_sqls) -> str:
    return "VALUES " + ", ".join(f"({r})" for r in row_sqls)


def bulk_placeholder_row(cols) -> str:
    return ", ".join(cols)


def validate_positive_batch_size(is_none: bool, batch_size: int = 0) -> int:
    if is_none:
        return 0
    return 0 if batch_size > 0 else 1


def effective_batch_size(user_set: bool, user_batch: int, max_batch: int, n_objs: int) -> int:
    maxb = max_batch if max_batch > 0 else (n_objs if n_objs > 0 else 1)
    if not user_set:
        return maxb
    return min(user_batch, maxb) if user_batch > 0 else maxb


def queryset_write_guard(combinator: bool, is_sliced: bool, has_distinct_fields: bool,
                         has_values_fields: bool) -> int:
    if combinator:
        return 1
    if is_sliced:
        return 2
    if has_distinct_fields:
        return 3
    if has_values_fields:
        return 4
    return 0


def sql_update_set_clause(assignments) -> str:
    return ", ".join(assignments)


def multi_batch_needs_atomic(n_batches: int) -> bool:
    return n_batches > 1


def key_has_lookup_sep(key: str) -> bool:
    return "__" in key


def keys_without_lookup_sep(keys):
    return [k for k in keys if "__" not in k]


def create_defaults_use_update(create_defaults_is_none: bool) -> bool:
    return bool(create_defaults_is_none)


def join_sorted_comma(names) -> str:
    return ", ".join(sorted(names))


def bulk_create_conflict_kind(ignore_conflicts: bool, update_conflicts: bool) -> int:
    if ignore_conflicts and update_conflicts:
        return -1
    if ignore_conflicts:
        return 1
    if update_conflicts:
        return 2
    return 0


def contains_preflight(has_values_fields: bool, pk_set: bool) -> int:
    if has_values_fields:
        return 1
    if not pk_set:
        return 2
    return 0


def aggregate_distinct_fields_error(has_distinct_fields: bool) -> bool:
    return bool(has_distinct_fields)


def filter_after_slice_error(has_filters: bool, is_sliced: bool) -> bool:
    return bool(has_filters and is_sliced)


def prohibited_filter_kwargs(keys):
    return sorted(k for k in keys if k in ("_connector", "_negated"))


def select_for_update_options_conflict(nowait: bool, skip_locked: bool) -> bool:
    return bool(nowait and skip_locked)


def union_empty_self_kind(nonempty_other_count: int) -> int:
    if nonempty_other_count <= 0:
        return 0
    if nonempty_other_count == 1:
        return 1
    return 2


def combinator_return_empty_self(self_is_empty: bool) -> bool:
    return bool(self_is_empty)


def save_force_conflict(
    force_insert: bool, force_update: bool, has_update_fields: bool
) -> bool:
    return bool(force_insert and (force_update or has_update_fields))


def save_skip_empty_update_fields(
    update_fields_is_none: bool, n_update_fields: int
) -> bool:
    return (not update_fields_is_none) and n_update_fields <= 0


def save_force_update_no_pk(
    pk_set: bool, force_update: bool, has_update_fields: bool
) -> bool:
    return (not pk_set) and (force_update or has_update_fields)


def collector_add_empty(n_objs: int) -> bool:
    return n_objs <= 0


def collector_delete_empty(n_models: int, n_fast_deletes: int) -> bool:
    return n_models <= 0 and n_fast_deletes <= 0


def collector_single_fast_path(n_models: int, n_instances: int) -> bool:
    return n_models == 1 and n_instances == 1


def can_fast_delete_result(
    from_field_blocks: bool,
    model_ok: bool,
    has_signal_listeners: bool,
    parents_ok: bool,
    relations_ok: bool,
    no_bulk_related: bool,
) -> bool:
    if from_field_blocks or not model_ok or has_signal_listeners:
        return False
    return bool(parents_ok and relations_ok and no_bulk_related)


def sql_assignment(quoted_col: str, rhs: str) -> str:
    return f"{quoted_col} = {rhs}"


def sql_null_assignment(quoted_col: str) -> str:
    return f"{quoted_col} = NULL"


def sql_parenthesized_list(cols) -> str:
    return "(" + ", ".join(cols) + ")"


def sql_values_row(placeholders: str) -> str:
    return f"VALUES ({placeholders})"


def sql_aggregate_subquery(select_sql: str, inner_sql: str) -> str:
    return f"SELECT {select_sql} FROM ({inner_sql}) subquery"


def sql_space_join(parts) -> str:
    return " ".join(parts)


def row_count_or_zero(is_none: bool, row_count: int = 0) -> int:
    return 0 if is_none else row_count


def queryset_sliced_error(is_sliced: bool) -> bool:
    return bool(is_sliced)


def clear_none_arg(single_none: bool) -> bool:
    return bool(single_none)


def only_none_arg_error(single_none: bool) -> bool:
    return bool(single_none)


def reverse_standard_ordering(standard_ordering: bool) -> bool:
    return not standard_ordering


def queryset_index_validate(is_int: bool, is_slice: bool, has_negative: bool) -> int:
    if not is_int and not is_slice:
        return 1
    if has_negative:
        return 2
    return 0


def qs_and_empty_kind(self_empty: bool, other_empty: bool) -> int:
    if other_empty:
        return 1
    if self_empty:
        return 2
    return 0


def qs_or_empty_kind(self_empty: bool, other_empty: bool) -> int:
    if self_empty:
        return 1
    if other_empty:
        return 2
    return 0


def date_kind_valid(kind: str) -> bool:
    return kind in ("year", "month", "week", "day")


def datetime_kind_valid(kind: str) -> bool:
    return kind in ("year", "month", "week", "day", "hour", "minute", "second")


def date_order_valid(order: str) -> bool:
    return order in ("ASC", "DESC")


def order_by_desc_prefix(order: str) -> str:
    return "-" if order == "DESC" else ""


def earliest_missing_fields(has_fields: bool, has_get_latest_by: bool) -> bool:
    return (not has_fields) and (not has_get_latest_by)


def save_base_needs_atomic(has_parents: bool) -> bool:
    return bool(has_parents)


def save_created_flag(updated: bool) -> bool:
    return not updated


def do_update_empty_values_kind(has_update_fields: bool, exists: bool) -> int:
    return 0 if (has_update_fields or exists) else 1


def clean_field_skip(name_in_exclude: bool, generated: bool) -> bool:
    return bool(name_in_exclude or generated)


def clean_field_skip_blank_empty(blank: bool, in_empty_values: bool) -> bool:
    return bool(blank and in_empty_values)


def validation_has_errors(n_error_keys: int) -> bool:
    return n_error_keys > 0


def is_non_field_errors_key(name: str) -> bool:
    return name == "__all__"


def fixed_timezone_name(offset_minutes: int) -> str:
    sign = "-" if offset_minutes < 0 else "+"
    hh, mm = divmod(abs(offset_minutes), 60)
    return f"{sign}{hh:02d}{mm:02d}"


def datetime_is_aware(utcoffset_not_none: bool) -> bool:
    return bool(utcoffset_not_none)


def datetime_is_naive(utcoffset_is_none: bool) -> bool:
    return bool(utcoffset_is_none)


def mark_safe_kind(has_html: bool, is_callable: bool) -> int:
    if has_html:
        return 0
    if is_callable:
        return 1
    return 2


def lookup_head(lookup: str) -> str:
    return lookup.split("__", 1)[0]



def queryset_is_ordered(
    is_empty_qs: bool,
    has_extra_order: bool,
    has_order_by: bool,
    default_ordering: bool,
    has_meta_ordering: bool,
    has_group_by: bool,
) -> bool:
    if is_empty_qs:
        return True
    if has_extra_order or has_order_by:
        return True
    if default_ordering and has_meta_ordering and not has_group_by:
        return True
    return False


def annotation_alias_conflicts(alias_in_names: bool) -> bool:
    return bool(alias_in_names)


def complex_filter_is_q(is_q_instance: bool) -> bool:
    return bool(is_q_instance)


def using_is_none(using_is_none: bool) -> bool:
    return bool(using_is_none)


def refresh_fields_empty(n_fields: int) -> bool:
    return n_fields <= 0


def refresh_fields_have_lookup_sep(fields) -> bool:
    return any("__" in f for f in fields)


def unique_check_excluded(check_names, exclude) -> bool:
    ex = set(exclude)
    return any(name in ex for name in check_names)


def unique_lookup_skip_value(
    is_none: bool, is_empty_str: bool, empty_as_null: bool
) -> bool:
    return bool(is_none or (is_empty_str and empty_as_null))


def unique_check_incomplete(n_check: int, n_kwargs: int) -> bool:
    return n_check != n_kwargs


def unique_error_is_single_field(n_check: int) -> bool:
    return n_check == 1


def in_lookup_empty(n_rhs: int) -> bool:
    return n_rhs <= 0


def sql_lhs_rhs(lhs: str, rhs_op: str) -> str:
    return f"{lhs} {rhs_op}"


def sql_or_join(parts) -> str:
    return " OR ".join(parts)


def is_password_usable(encoded_is_none: bool, starts_with_unusable: bool) -> bool:
    return bool(encoded_is_none or not starts_with_unusable)


def identify_hasher_kind(
    encoded_len: int,
    has_dollar: bool,
    starts_md5_dollar: bool,
    starts_sha1_dollar: bool,
) -> int:
    if (encoded_len == 32 and not has_dollar) or (
        encoded_len == 37 and starts_md5_dollar
    ):
        return 1
    if encoded_len == 46 and starts_sha1_dollar:
        return 2
    return 0


def hasher_algorithm_prefix(encoded: str) -> str:
    return encoded.split("$", 1)[0]


def cache_default_key(key_prefix: str, version: int, key: str) -> str:
    return f"{key_prefix}:{version}:{key}"


def cache_timeout_kind(
    is_default_sentinel: bool, is_none: bool, timeout: int = 0
) -> int:
    if is_default_sentinel:
        return 0
    if is_none:
        return 2
    if timeout == 0:
        return 1
    return 3


def file_multiple_chunks(size: int, chunk_size: int) -> bool:
    return size > chunk_size


def mask_hash(hash: str, show: int = 6, mask_char: str = "*") -> str:
    return hash[:show] + mask_char * len(hash[show:])


def result_cache_populated(cache_is_none: bool) -> bool:
    return not cache_is_none


def prefetch_still_needed(has_lookups: bool, prefetch_done: bool) -> bool:
    return bool(has_lookups and not prefetch_done)


def queryset_cache_truthy(cache_len: int) -> bool:
    return cache_len > 0


def sticky_filter_active(sticky: bool) -> bool:
    return bool(sticky)


def csrf_token_length_ok(len_: int, secret_len: int, token_len: int) -> int:
    return 0 if len_ in (secret_len, token_len) else 1


def csrf_token_chars_valid(token: str) -> bool:
    import string
    allowed = string.ascii_letters + string.digits
    return all(c in allowed for c in token)


def csrf_unmask_token(token: str, secret_len: int) -> str:
    import string
    chars = string.ascii_letters + string.digits
    if len(token) != 2 * secret_len:
        return ""
    mask = token[:secret_len]
    cipher = token[secret_len:]
    pairs = zip((chars.index(x) for x in cipher), (chars.index(x) for x in mask))
    return "".join(chars[x - y] for x, y in pairs)


def csrf_mask_secret(secret: str, mask: str) -> str:
    import string
    chars = string.ascii_letters + string.digits
    if len(secret) != len(mask) or not secret:
        return ""
    pairs = zip((chars.index(x) for x in secret), (chars.index(x) for x in mask))
    cipher = "".join(chars[(x + y) % len(chars)] for x, y in pairs)
    return mask + cipher


def hsts_header_value(seconds: int, include_subdomains: bool, preload: bool) -> str:
    sts = f"max-age={seconds}"
    if include_subdomains:
        sts += "; includeSubDomains"
    if preload:
        sts += "; preload"
    return sts


def https_redirect_url(host: str, full_path: str) -> str:
    return f"https://{host}{full_path}"


def referrer_policy_header(policies) -> str:
    return ",".join(policies)  # no spaces after commas


def route_looks_like_regex(route: str) -> bool:
    return "(?P<" in route or route.startswith("^") or route.endswith("$")


def route_simple_match(is_endpoint: bool, route: str, path: str):
    if is_endpoint:
        if route == path:
            return (1, "")
        return (0, "")
    if path.startswith(route):
        return (2, path[len(route):])
    return (0, "")


def engine_loaders_app_dirs_conflict(app_dirs: bool, loaders_defined: bool) -> bool:
    return bool(app_dirs and loaders_defined)


def template_cache_key_plain(template_name: str) -> str:
    return template_name


def to_language(locale: str) -> str:
    p = locale.find("_")
    if p >= 0:
        return locale[:p].lower() + "-" + locale[p + 1 :].lower()
    return locale.lower()


def to_locale(language: str) -> str:
    lang, _, country = language.lower().partition("-")
    if not country:
        return language[:3].lower() + language[3:]
    country, _, tail = country.partition("-")
    country = country.title() if len(country) > 2 else country.upper()
    if tail:
        country += "-" + tail
    return lang + "_" + country


def plural_index_default(n: int) -> int:
    return 0 if n == 1 else 1


def language_code_too_long(len_: int, max_len: int) -> bool:
    return len_ > max_len


def sql_create_table(quoted_table: str, columns_sql: str) -> str:
    return f"CREATE TABLE {quoted_table} ({columns_sql})"


def migration_describe(class_name: str, constructor_args: str) -> str:
    return f"{class_name}: {constructor_args}"


def migration_formatted_description(category: str, description: str) -> str:
    return f"{category} {description}"


def http_status_code_valid(code: int) -> bool:
    return 100 <= code <= 599


def weak_etag_if_strong(etag: str) -> str:
    return ("W/" + etag) if etag.startswith('"') else etag


def accepts_gzip(accept_encoding: str) -> bool:
    import re
    return re.search(r"\bgzip\b", accept_encoding, re.I) is not None


def gzip_content_too_short(content_len: int, min_len: int = 200) -> bool:
    return content_len < min_len


def host_needs_www_prefix(host: str) -> bool:
    return bool(host) and not host.startswith("www.")


def www_redirect_url(scheme: str, host: str, path: str) -> str:
    return f"{scheme}://www.{host}{path}"


def conditional_needs_etag(cache_control: str) -> bool:
    parts = (cache_control or "").split(",")
    return all(p.strip().lower() != "no-store" for p in parts)


def xframe_options_value(setting_value: str) -> str:
    return (setting_value or "DENY").upper()


def message_tags_join(extra_tags: str, level_tag: str) -> str:
    return " ".join(tag for tag in [extra_tags, level_tag] if tag)


def hashed_static_basename(root: str, hash_with_dot: str, ext: str) -> str:
    return f"{root}{hash_with_dot}{ext}"


def posix_path_join(directory: str, basename: str) -> str:
    if not directory:
        return basename
    if directory.endswith("/"):
        return directory + basename
    return directory + "/" + basename


def json_use_indent_separators(has_indent: bool) -> bool:
    return bool(has_indent)


def datetime_iso_utc_z(iso: str) -> str:
    if iso.endswith("+00:00"):
        return iso.removesuffix("+00:00") + "Z"
    return iso


def string_has_newlines(s: str) -> bool:
    return "\n" in s or "\r" in s


def split_email_address(address: str):
    if "@" not in address:
        return (False, "", "")
    local, domain = address.rsplit("@", 1)
    if not local or not domain:
        return (False, "", "")
    return (True, local, domain)


def model_meta_label(app_label: str, object_name: str) -> str:
    return f"{app_label}.{object_name}"


def manager_str(model_label: str, manager_name: str) -> str:
    return f"{model_label}.{manager_name}"


def from_queryset_class_name(manager_cls: str, qs_cls: str) -> str:
    return f"{manager_cls}From{qs_cls}"


def migration_node_key(app_label: str, name: str) -> str:
    return f"{app_label}.{name}"


def perm_codename(action: str, model_name: str) -> str:
    return f"{action}_{model_name}"


def user_can_authenticate(has_is_active: bool, is_active: bool) -> bool:
    return (not has_is_active) or is_active


def signal_has_receivers(n_receivers: int) -> bool:
    return n_receivers > 0


def split_dotted_path(dotted: str):
    try:
        module, attr = dotted.rsplit(".", 1)
    except ValueError:
        return (False, "", "")
    if not module or not attr:
        return (False, "", "")
    return (True, module, attr)


def app_module_path(app_name: str, submodule: str) -> str:
    return f"{app_name}.{submodule}"


def renamed_method_warning(class_name: str, old_name: str, new_name: str) -> str:
    return f"`{class_name}.{old_name}` is deprecated, use `{class_name}.{new_name}` instead."


def path_ends_with_py(path: str) -> bool:
    return path.endswith(".py")


def path_has_any_suffix(path: str, suffixes) -> bool:
    return any(path.endswith(s) for s in suffixes if s)


def postgres_arrayfield_path_shorten(path: str) -> bool:
    return path == "django.contrib.postgres.fields.array.ArrayField"


def filename_needs_quotes(filename: str) -> bool:
    return any(c in filename for c in '"\\\n\r; ')


def paginator_num_pages(count, per_page, orphans, allow_empty_first_page) -> int:
    if per_page <= 0:
        per_page = 1
    if count == 0 and not allow_empty_first_page:
        return 0
    hits = max(1, count - orphans)
    return -(-hits // per_page)  # ceil


def paginator_page_bottom(number, per_page) -> int:
    return (max(number, 1) - 1) * max(per_page, 0)


def paginator_page_top(number, per_page, orphans, count) -> int:
    bottom = paginator_page_bottom(number, per_page)
    top = bottom + per_page
    return count if top + orphans >= count else top


def paginator_number_range_code(number, num_pages) -> int:
    if number < 1:
        return 2
    if number > num_pages:
        return 3
    return 0


def url_is_relative_path(to: str) -> bool:
    return to.startswith(("./", "../"))


def url_feels_like_url(to: str) -> bool:
    return "/" in to or "." in to


def formset_total_forms_bound(submitted, absolute_max) -> int:
    return min(submitted, absolute_max)


def formset_total_forms_unbound(initial_forms, min_num, extra, max_num) -> int:
    total_forms = max(initial_forms, min_num) + extra
    if initial_forms > max_num >= 0:
        return initial_forms
    if total_forms > max_num >= 0:
        return max_num
    return total_forms


def path_has_dotdot(path: str) -> bool:
    from pathlib import PurePath
    return ".." in PurePath(path).parts


def storage_normalize_name(name: str) -> str:
    return str(name).replace("\\", "/")


def storage_alternative_name(root: str, random7: str, ext: str) -> str:
    return f"{root}_{random7}{ext}"


def storage_name_available(exists, has_max_length, name_len, max_length=0) -> bool:
    return (not exists) and not (has_max_length and name_len > max_length)


def middleware_capability_ok(sync_capable, async_capable) -> bool:
    return bool(sync_capable or async_capable)


def sitemap_priority_valid(priority: float) -> bool:
    return 0.0 <= priority <= 1.0


def sitemap_changefreq_valid(freq: str) -> bool:
    return freq in ("always", "hourly", "daily", "weekly", "monthly", "yearly", "never")


def ordinal_suffix_kind(value: int) -> int:
    if value < 0:
        return -1
    if value % 100 in (11, 12, 13):
        return 11
    return value % 10


def intcomma_ascii(digits: str) -> str:
    sign = ""
    s = digits
    if s[:1] in "-+":
        sign, s = s[0], s[1:]
    if "." in s:
        intpart, frac = s.split(".", 1)
        frac = "." + frac
    else:
        intpart, frac = s, ""
    out = []
    for i, ch in enumerate(reversed(intpart)):
        if i and i % 3 == 0:
            out.append(",")
        out.append(ch)
    return sign + "".join(reversed(out)) + frac


def check_is_serious(level: int, threshold: int) -> bool:
    return level >= threshold


def path_with_query(path: str, query: str) -> str:
    return path if not query else f"{path}?{query}"


def ensure_leading_slash(path: str) -> str:
    if not path:
        return "/"
    return path if path.startswith("/") else "/" + path


def redirect_paths_equal(a: str, b: str) -> bool:
    return a == b


def wkt_point(x: str, y: str) -> str:
    return f"POINT({x} {y})"


def postgres_empty_array_literal() -> str:
    return "{}"


def list_context_object_name(model_name: str) -> str:
    return f"{model_name}_list"


def http_method_in_names(method_lower: str, names) -> bool:
    return method_lower in names


def page_token_is_last(page: str) -> bool:
    return page == "last"


def model_template_name(app_label: str, object_name: str, suffix: str) -> str:
    return f"{app_label}/{object_name}{suffix}.html"


def modelform_class_name(model_name: str) -> str:
    return f"{model_name}Form"


def form_field_included(
    editable, fields_is_none, in_fields, exclude_active, in_exclude
) -> bool:
    if not editable:
        return False
    if not fields_is_none and not in_fields:
        return False
    if exclude_active and in_exclude:
        return False
    return True


def admin_quote(s: str) -> str:
    specials = b'":/_#?;@&=+$,"[]<>%\n\\'
    out = []
    for ch in s:
        o = ord(ch)
        if o < 256 and o in specials:
            out.append("_%02X" % o)
        else:
            out.append(ch)
    return "".join(out)


def lookup_key_endswith(key: str, suffix: str) -> bool:
    return key.endswith(suffix)


def prepare_lookup_isnull(value_lower: str) -> bool:
    return value_lower not in ("", "false", "0")


def paths_equal(a: str, b: str) -> bool:
    return a == b


def strings_ci_equal_ascii(a: str, b: str) -> bool:
    return a.lower() == b.lower()


def migration_filename(name: str) -> str:
    return f"{name}.py"


def introspection_is_table(type_code: str) -> bool:
    return type_code == "t"


def combined_expression_sql(lhs: str, connector: str, rhs: str) -> str:
    if not connector and not rhs:
        return f"({lhs})"
    return f"({lhs} {connector} {rhs})"


def sql_cast_as_numeric(sql: str) -> str:
    return f"(CAST({sql} AS NUMERIC))"


def cache_timestamp_expired(exp_is_none, exp, now) -> bool:
    if exp_is_none:
        return False
    return exp < now


def cache_file_name(hexdigest: str, suffix: str) -> str:
    return f"{hexdigest}{suffix}"


def cache_cull_needed(num_entries, max_entries) -> bool:
    return num_entries >= max_entries


def cache_cull_sample_size(num_entries, cull_frequency) -> int:
    if cull_frequency == 0:
        return 0
    return int(num_entries / cull_frequency)


def wsgi_request_path(script_name: str, path_info: str) -> str:
    # path_info.replace("/", "", 1) removes the first slash anywhere, not only
    # a leading slash (matters when PATH_INFO has no leading slash).
    return "%s/%s" % (script_name.rstrip("/"), path_info.replace("/", "", 1))


def exception_status_code(kind: str) -> int:
    return {
        "http404": 404,
        "permission": 403,
        "bad": 400,
        "suspicious": 400,
        "multipart": 400,
    }.get(kind, 500)


def postgres_normalize_spaces(val: str):
    import re

    if not (val := val.strip()):
        return ""
    return re.sub(r"\s{2,}", " ", val)


def postgres_psql_escape(query: str) -> str:
    import re

    query = re.sub(r"['\0\[\]()|&:*!@<>\\]", " ", query)
    return postgres_normalize_spaces(query)


def search_vector_match_sql(lhs: str, rhs: str) -> str:
    return f"{lhs} @@ {rhs}"


def feed_protocol(secure: bool) -> str:
    return "https" if secure else "http"


def feed_url_is_network_path(url: str) -> bool:
    return url.startswith("//")


def feed_url_has_scheme(url: str) -> bool:
    return url.startswith(("http://", "https://", "mailto:"))


def feed_network_path_url(protocol: str, url: str) -> str:
    return f"{protocol}:{url}"


def feed_absolute_url(protocol: str, domain: str, url: str) -> str:
    return f"{protocol}://{domain}{url}"


def dotted_qualname(module: str, qualname: str) -> str:
    return f"{module}.{qualname}"


def strip_p_tags(value: str) -> str:
    return value.replace("<p>", "").replace("</p>", "")


def approximate_equal(val: float, other: float, places: int) -> bool:
    return val == other or round(abs(val - other), places) == 0


def http_allow_header(methods) -> str:
    return ", ".join(methods)


def ensure_trailing_slash(url: str) -> str:
    if not url:
        return "/"
    return url if url.endswith("/") else url + "/"


def ascii_lower(s: str) -> str:
    return s.lower()


def management_command_name(path: str) -> str:
    import os

    base = os.path.basename(path)
    if base.endswith(".py"):
        base = base[:-3]
    return base


def asgi_path_info(path: str, script_name: str) -> str:
    if script_name and path.startswith(script_name):
        return path[len(script_name) :]
    return path


def field_str(model_label: str, name: str) -> str:
    return f"{model_label}.{name}"


def field_repr(path: str, has_name: bool, name: str = "") -> str:
    if not has_name:
        return f"<{path}>"
    return f"<{path}: {name}>"


def verbose_name_from_name(name: str) -> str:
    return name.replace("_", " ")


def field_name_check_code(name: str) -> int:
    if not name:
        return 0
    if name.endswith("_"):
        return 1
    if "__" in name:
        return 2
    if name == "pk":
        return 3
    return 0


def field_column_name(attname: str, db_column: str = "") -> str:
    return db_column or attname


def aggregate_default_alias(expr_name: str, agg_name: str) -> str:
    return f"{expr_name}__{agg_name.lower()}"


def sql_distinct_prefix(distinct: bool) -> str:
    return "DISTINCT " if distinct else ""


def index_column_with_order(column: str, descending: bool) -> str:
    return f"-{column}" if descending else column


def index_name_fix_leading(name: str) -> str:
    if name and (name[0] == "_" or name[0].isdigit()):
        return "D" + name[1:]
    return name


def admin_can_show_all(result_count: int, list_max_show_all: int) -> bool:
    return result_count <= list_max_show_all


def admin_is_multi_page(result_count: int, list_per_page: int) -> bool:
    return result_count > list_per_page


def query_string_with_prefix(encoded: str) -> str:
    return f"?{encoded}"


def css_classes_join(classes) -> str:
    return " ".join(classes)


def password_reset_token_join(ts_b36: str, hash_hex: str) -> str:
    return f"{ts_b36}-{hash_hex}"


def password_reset_token_split(token: str):
    try:
        ts_b36, rest = token.split("-", 1)
    except ValueError:
        return (False, "", "")
    return (True, ts_b36, rest)


def password_meets_min_length(password_len: int, min_length: int) -> bool:
    return password_len >= min_length


def password_is_numeric_only(password: str) -> bool:
    return bool(password) and password.isdigit()


def migration_node_repr(cls: str, app: str, name: str) -> str:
    return f"<{cls}: ({app!r}, {name!r})>"


def serializer_datetime_import() -> str:
    return "import datetime"


def sitemap_absolute_url(protocol: str, domain: str, path: str) -> str:
    return f"{protocol}://{domain}{path}"


def sitemap_paged_url(absolute_url: str, page: int) -> str:
    return f"{absolute_url}?p={page}"


def x_robots_tag_value() -> str:
    return "noindex, noodp, noarchive"


def http_status_session_saveable(status_code: int) -> bool:
    return status_code < 500


def resource_was_modified(header_missing: bool, mtime: float, header_mtime: float) -> bool:
    if header_missing:
        return True
    return mtime > header_mtime


def template_register_name(explicit_name: str, func_name: str) -> str:
    return explicit_name or func_name


def normalize_ascii_whitespace(s: str) -> str:
    import re

    return re.sub(r"[ \t\n\r\f\v]+", " ", s)


def html_boolean_attr_is_true(name: str, value: str) -> bool:
    return (not value) or value == name


def sql_func_call(function: str, expressions: str) -> str:
    return f"{function}({expressions})"


def field_display_method_name(field_name: str) -> str:
    return f"get_{field_name}_display"


def optimizer_lists_equal_len(a: int, b: int) -> bool:
    return a == b

def related_name_ends_plus(name: str) -> bool:
    return bool(name) and name.endswith("+")


def related_name_is_identifier(name: str) -> bool:
    return bool(name) and name.isidentifier()


def related_query_name_ends_underscore(name: str) -> bool:
    return bool(name) and name.endswith("_")


def related_query_name_has_lookup_sep(name: str) -> bool:
    return "__" in name


def fk_default_name(model_name: str, pk_name: str) -> str:
    return f"{model_name}_{pk_name}"


def related_filter_key(field_name: str, rh_field: str) -> str:
    return f"{field_name}__{rh_field}"


def constraint_deconstruct_path(path: str) -> str:
    return path.replace("django.db.models.constraints", "django.db.models")


def sql_varchar_type(has_max_length: bool, max_length: int = 0) -> str:
    if not has_max_length:
        return "varchar"
    return "varchar(%s)" % max_length


def sql_decimal_type(max_digits: int, decimal_places: int) -> str:
    return "numeric(%s, %s)" % (max_digits, decimal_places)


def admin_selectfilter_class(is_stacked: bool) -> str:
    return "selectfilterstacked" if is_stacked else "selectfilter"


def admin_site_repr(cls: str, name: str) -> str:
    return f"{cls}(name={name!r})"


def permission_str(content_type: str, name: str) -> str:
    return f"{content_type} | {name}"


def admin_facet_count_key(index: int) -> str:
    return f"{index}__c"


def extract_lookup_name(lookup: str) -> str:
    return lookup.lower()


def sql_now_sqlite() -> str:
    return "CURRENT_TIMESTAMP"


def sql_now_postgresql() -> str:
    return "STATEMENT_TIMESTAMP()"


def feed_tag_uri(hostname: str, date_suffix: str, path: str, fragment: str) -> str:
    return "tag:%s%s:%s/%s" % (hostname, date_suffix, path, fragment)


def progress_percent(count: int, total: int) -> int:
    if total <= 0:
        return 0
    return count * 100 // total


def progress_done_width(percent: int, width: int) -> int:
    return percent * width // 100


def backend_vendor_is(vendor: str, expected: str) -> bool:
    return vendor == expected


def management_prog(basename: str, subcommand: str) -> str:
    return f"{basename} {subcommand}"


def filefield_default_max_length() -> int:
    return 100


def jsonfield_internal_type() -> str:
    return "JSONField"


def test_label_looks_like_path(label: str) -> bool:
    return ("/" in label) or ("\\" in label) or label.endswith(".py")


def debug_template_path(name: str) -> str:
    return name


def date_year_in_range(year: int) -> bool:
    return 1 <= year <= 9999


def unique_constraint_name(model: str, fields_joined: str) -> str:
    return f"{model}_{fields_joined}_uniq"


def db_host_is_unix_socket(host: str) -> bool:
    return bool(host) and host.startswith("/")


def postgres_set_timezone_sql() -> str:
    return "SELECT set_config('TimeZone', %s, false)"


def mysql_isolation_level_valid(level: str) -> bool:
    return level in (
        "read uncommitted",
        "read committed",
        "repeatable read",
        "serializable",
    )


def simple_select_eq_limit_sql(
    quoted_table: str, quoted_cols, quoted_where_col: str, limit: int
) -> str:
    if limit < 1:
        limit = 1
    cols = ", ".join(quoted_cols)
    return (
        f"SELECT {cols} FROM {quoted_table} "
        f"WHERE {quoted_where_col} = %s LIMIT {int(limit)}"
    )


def simple_select_all_sql(quoted_table: str, quoted_cols, limit: int = 0) -> str:
    cols = ", ".join(quoted_cols)
    sql = f"SELECT {cols} FROM {quoted_table}"
    if limit and limit > 0:
        sql += f" LIMIT {int(limit)}"
    return sql


def simple_select_in_sql(
    quoted_table: str, quoted_cols, quoted_where_col: str, n_placeholders: int
) -> str:
    if n_placeholders < 1:
        n_placeholders = 1
    cols = ", ".join(quoted_cols)
    ph = sql_in_placeholders(n_placeholders)
    return (
        f"SELECT {cols} FROM {quoted_table} "
        f"WHERE {quoted_where_col} IN ({ph})"
    )


def simple_update_eq_sql(quoted_table: str, quoted_set_cols, quoted_where_col: str) -> str:
    sets = ", ".join(f"{c} = %s" for c in quoted_set_cols)
    return f"UPDATE {quoted_table} SET {sets} WHERE {quoted_where_col} = %s"


def render_fortune_page(rows) -> str:
    """rows: iterable of (id, message) already sorted by message."""
    parts = [
        "<!DOCTYPE html>\n",
        "<html>\n",
        "<head>\n",
        "<title>Fortunes</title>\n",
        "</head>\n",
        "<body>\n",
        "  \n",
        "<table>\n",
        "<tr>\n",
        "<th>id</th>\n",
        "<th>message</th>\n",
        "</tr>\n",
    ]
    # Local escape matching html.escape(quote=True)
    _esc = (
        ("&", "&amp;"),
        ("<", "&lt;"),
        (">", "&gt;"),
        ('"', "&quot;"),
        ("'", "&#x27;"),
    )

    def esc(s: str) -> str:
        for a, b in _esc:
            s = s.replace(a, b)
        return s

    for row_id, message in rows:
        parts.append("<tr>\n<td>")
        parts.append(str(int(row_id)))
        parts.append("</td>\n<td>")
        parts.append(esc(str(message)))
        parts.append("</td>\n</tr>\n")
    parts.append("</table>\n\n</body>\n</html>")
    return "".join(parts)
