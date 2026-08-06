"""
Django's standard crypto functions and utilities.
"""

import hashlib
import hmac
import secrets

from django import native as _native
from django.conf import settings
from django.utils.encoding import force_bytes


class InvalidAlgorithm(ValueError):
    """Algorithm is not supported by hashlib."""

    pass


class _HmacResult:
    """Lightweight stand-in for hmac.HMAC when digest is precomputed in C++."""

    __slots__ = ("_d",)

    def __init__(self, d):
        self._d = d

    def digest(self):
        return self._d

    def hexdigest(self):
        return self._d.hex()


def salted_hmac(key_salt, value, secret=None, *, algorithm="sha1"):
    """
    Return the HMAC of 'value', using a key generated from key_salt and a
    secret (which defaults to settings.SECRET_KEY). Default algorithm is SHA1,
    but any algorithm name supported by hashlib can be passed.

    A different key_salt should be passed in for every application of HMAC.
    """
    if secret is None:
        secret = settings.SECRET_KEY

    key_salt = force_bytes(key_salt)
    secret = force_bytes(secret)
    value = force_bytes(value)
    try:
        hasher = getattr(hashlib, algorithm)
    except AttributeError as e:
        raise InvalidAlgorithm(
            "%r is not an algorithm accepted by the hashlib module." % algorithm
        ) from e
    if _native.AVAILABLE and algorithm in {"sha1", "sha256", "sha384", "sha512", "md5"}:
        # OpenSSL path: return an object with .digest() matching hmac.HMAC API.
        # Use a process-lifetime class (not a per-call class) — signing.loads
        # hits this on every session decode.
        digest = _native.salted_hmac_digest(algorithm, key_salt, secret, value)
        return _HmacResult(digest)
    # We need to generate a derived key from our base key. We can do this by
    # passing the key_salt and our base key through a pseudo-random function.
    key = hasher(key_salt + secret).digest()
    # If len(key_salt + secret) > block size of the hash algorithm, the above
    # line is redundant and could be replaced by key = key_salt + secret, since
    # the hmac module does the same thing for keys longer than the block size.
    # However, we need to ensure that we *always* do this.
    return hmac.new(key, msg=value, digestmod=hasher)


RANDOM_STRING_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"


def get_random_string(length, allowed_chars=RANDOM_STRING_CHARS):
    """
    Return a securely generated random string.

    The bit length of the returned value can be calculated with the formula:
        log_2(len(allowed_chars)^length)

    For example, with default `allowed_chars` (26+26+10), this gives:
      * length: 12, bit length =~ 71 bits
      * length: 22, bit length =~ 131 bits
    """
    if _native.AVAILABLE and isinstance(allowed_chars, str) and length >= 0:
        try:
            return _native.secure_random_string(length, allowed_chars)
        except Exception:
            pass
    return "".join(secrets.choice(allowed_chars) for i in range(length))


def constant_time_compare(val1, val2):
    """Return True if the two strings are equal, False otherwise."""
    b1 = force_bytes(val1)
    b2 = force_bytes(val2)
    if _native.AVAILABLE:
        return _native.constant_time_compare(b1, b2)
    return secrets.compare_digest(b1, b2)


def pbkdf2(password, salt, iterations, dklen=0, digest=None):
    """Return the hash of password using pbkdf2."""
    if digest is None:
        digest = hashlib.sha256
    password = force_bytes(password)
    salt = force_bytes(salt)
    name = digest().name
    if _native.AVAILABLE and name in {"sha1", "sha256", "sha384", "sha512", "md5"}:
        try:
            return _native.pbkdf2_hmac(name, password, salt, iterations, dklen or 0)
        except Exception:
            pass
    dklen = dklen or None
    return hashlib.pbkdf2_hmac(name, password, salt, iterations, dklen)
