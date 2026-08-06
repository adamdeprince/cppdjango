from django import native as _native
from django.utils.cache import patch_vary_headers
from django.utils.deprecation import MiddlewareMixin
from django.utils.regex_helper import _lazy_re_compile
from django.utils.text import acompress_sequence, compress_sequence, compress_string

re_accepts_gzip = _lazy_re_compile(r"\bgzip\b")


class GZipMiddleware(MiddlewareMixin):
    """
    Compress content if the browser allows gzip compression.
    Set the Vary header accordingly, so that caches will base their storage
    on the Accept-Encoding header.

    Dual-path: eligibility plan is one C++ call; compression stays Python
    (zlib) until a native compressor is added.
    """

    max_random_bytes = 100

    def process_response(self, request, response):
        ae = request.META.get("HTTP_ACCEPT_ENCODING", "")
        etag = response.get("ETag") or ""
        content_len = 0 if response.streaming else len(response.content)

        if _native.AVAILABLE:
            plan = _native.gzip_process_response_plan(
                response.streaming,
                content_len,
                200,
                response.has_header("Content-Encoding"),
                ae,
                etag,
            )
            if plan.get("early_skip"):
                return response
            if plan.get("set_vary"):
                patch_vary_headers(response, ("Accept-Encoding",))
            if not plan.get("should_compress"):
                return response
        else:
            if not response.streaming and content_len < 200:
                return response
            if response.has_header("Content-Encoding"):
                return response
            patch_vary_headers(response, ("Accept-Encoding",))
            if not re_accepts_gzip.search(ae):
                return response
            plan = None

        if response.streaming:
            if response.is_async:
                response.streaming_content = acompress_sequence(
                    response.streaming_content,
                    max_random_bytes=self.max_random_bytes,
                )
            else:
                response.streaming_content = compress_sequence(
                    response.streaming_content,
                    max_random_bytes=self.max_random_bytes,
                )
            del response.headers["Content-Length"]
        else:
            compressed_content = compress_string(
                response.content,
                max_random_bytes=self.max_random_bytes,
            )
            if len(compressed_content) >= len(response.content):
                return response
            response.content = compressed_content
            response.headers["Content-Length"] = str(len(response.content))

        if _native.AVAILABLE and plan is not None:
            weak = plan.get("weak_etag")
            if weak:
                response.headers["ETag"] = weak
        else:
            etag = response.get("ETag")
            if etag and etag.startswith('"'):
                response.headers["ETag"] = "W/" + etag
        response.headers["Content-Encoding"] = "gzip"

        return response
