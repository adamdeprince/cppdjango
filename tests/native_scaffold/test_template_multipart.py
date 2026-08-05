from django.http.multipartparser import BoundaryIter, MultiPartParser
from django.template.base import DebugLexer, Lexer, TokenType
from django.test import SimpleTestCase


class NativeTemplateLexerTests(SimpleTestCase):
    template_string = (
        "text\n"
        "{% if test %}{{ varvalue }}{% endif %}"
        "{#comment {{not a var}} {%not a block%} #}"
        "end text"
    )

    def test_lexer_matches_expected_tokens(self):
        tokens = Lexer(self.template_string).tokenize()
        tuples = [(t.token_type, t.contents, t.lineno, t.position) for t in tokens]
        expected = [
            (TokenType.TEXT, "text\n", 1, None),
            (TokenType.BLOCK, "if test", 2, None),
            (TokenType.VAR, "varvalue", 2, None),
            (TokenType.BLOCK, "endif", 2, None),
            (
                TokenType.COMMENT,
                "comment {{not a var}} {%not a block%}",
                2,
                None,
            ),
            (TokenType.TEXT, "end text", 2, None),
        ]
        self.assertEqual(tuples, expected)

    def test_debug_lexer_positions(self):
        tokens = DebugLexer(self.template_string).tokenize()
        tuples = [(t.token_type, t.contents, t.lineno, t.position) for t in tokens]
        expected = [
            (TokenType.TEXT, "text\n", 1, (0, 5)),
            (TokenType.BLOCK, "if test", 2, (5, 18)),
            (TokenType.VAR, "varvalue", 2, (18, 32)),
            (TokenType.BLOCK, "endif", 2, (32, 43)),
            (
                TokenType.COMMENT,
                "comment {{not a var}} {%not a block%}",
                2,
                (43, 85),
            ),
            (TokenType.TEXT, "end text", 2, (85, 93)),
        ]
        self.assertEqual(tuples, expected)

    def test_verbatim(self):
        src = "{% verbatim %}{{ x }}{% endverbatim %}"
        tokens = Lexer(src).tokenize()
        types = [t.token_type for t in tokens]
        self.assertEqual(types[0], TokenType.BLOCK)
        self.assertEqual(tokens[0].contents, "verbatim")
        # Inside verbatim, {{ x }} is TEXT
        self.assertEqual(tokens[1].token_type, TokenType.TEXT)
        self.assertEqual(tokens[1].contents, "{{ x }}")
        self.assertEqual(tokens[2].contents, "endverbatim")

    def test_native_api(self):
        from django import native

        self.assertTrue(native.AVAILABLE)
        raw = native.template_tokenize("{{ a }}{% b %}", False)
        self.assertEqual(raw[0][0], TokenType.VAR.value)
        self.assertEqual(raw[0][1], "a")
        self.assertEqual(raw[1][0], TokenType.BLOCK.value)
        self.assertEqual(raw[1][1], "b")


class NativeMultipartTests(SimpleTestCase):
    def test_find_boundary(self):
        from django import native

        data = b"hello\r\n--bound\r\nworld"
        boundary = b"--bound"
        self.assertEqual(
            native.find_multipart_boundary(data, boundary),
            (5, 14),  # end before CRLF, next after boundary
        )
        self.assertIsNone(native.find_multipart_boundary(b"nope", boundary))

    def test_parse_header_parameters(self):
        from django import native

        main, params = native.parse_header_parameters(
            'form-data; name="file"; filename="a.txt"'
        )
        self.assertEqual(main, "form-data")
        self.assertEqual(params["name"], "file")
        self.assertEqual(params["filename"], "a.txt")
        main, params = native.parse_header_parameters(
            "form-data; title*=UTF-8''foo-%c3%a4.html"
        )
        self.assertEqual(params["title"], "foo-ä.html")

    def test_parse_multipart_message(self):
        from django import native

        body = b"\r\n".join(
            [
                b"--B",
                b'Content-Disposition: form-data; name="a"',
                b"",
                b"hello",
                b"--B",
                b'Content-Disposition: form-data; name="f"; filename="t.txt"',
                b"Content-Type: text/plain",
                b"",
                b"filedata",
                b"--B--",
                b"",
            ]
        )
        parts = native.parse_multipart_message(body, b"B")
        fields = [p for p in parts if p["type"] == 1]
        files = [p for p in parts if p["type"] == 2]
        self.assertEqual(len(fields), 1)
        self.assertEqual(fields[0]["name"], "a")
        self.assertEqual(fields[0]["body"], b"hello")
        self.assertEqual(len(files), 1)
        self.assertEqual(files[0]["filename"], "t.txt")
        self.assertEqual(files[0]["body"], b"filedata")

    def test_boundary_iter_uses_native(self):
        from django.http.multipartparser import LazyStream

        # Sanity: BoundaryIter still works end-to-end via LazyStream.
        stream = LazyStream(iter([b"abc\r\n--B\r\ndef"]))
        bi = BoundaryIter(stream, b"--B")
        chunk = next(bi)
        self.assertEqual(chunk, b"abc")

    def test_sanitize_filename(self):
        parser = MultiPartParser(
            {"CONTENT_TYPE": "multipart/form-data; boundary=B", "CONTENT_LENGTH": "0"},
            None,
            [],
        )
        self.assertEqual(parser.sanitize_file_name("a/b/c.txt"), "c.txt")
        self.assertEqual(parser.sanitize_file_name("..\\x.txt"), "x.txt")
        self.assertIsNone(parser.sanitize_file_name(".."))
        self.assertIsNone(parser.sanitize_file_name("."))
        # html entity
        self.assertEqual(parser.sanitize_file_name("f&amp;ile.txt"), "f&ile.txt")
