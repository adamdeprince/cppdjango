"File-based cache backend"

import glob
import os
import pickle
import random
import tempfile
import time
import zlib
from hashlib import md5

from django.core.cache.backends.base import DEFAULT_TIMEOUT, BaseCache
from django.core.files import locks
from django.core.files.move import file_move_safe
from django.utils._os import safe_makedirs


class FileBasedCache(BaseCache):
    cache_suffix = ".djcache"
    pickle_protocol = pickle.HIGHEST_PROTOCOL

    def __init__(self, dir, params):
        super().__init__(params)
        self._dir = os.path.abspath(dir)
        self._createdir()

    def add(self, key, value, timeout=DEFAULT_TIMEOUT, version=None):
        if self.has_key(key, version):
            return False
        self.set(key, value, timeout, version)
        return True

    def get(self, key, default=None, version=None):
        fname = self._key_to_file(key, version)
        try:
            with open(fname, "rb") as f:
                if not self._is_expired(f):
                    return pickle.loads(zlib.decompress(f.read()))
        except FileNotFoundError:
            pass
        return default

    def _write_content(self, file, timeout, value):
        expiry = self.get_backend_timeout(timeout)
        file.write(pickle.dumps(expiry, self.pickle_protocol))
        file.write(zlib.compress(pickle.dumps(value, self.pickle_protocol)))

    def set(self, key, value, timeout=DEFAULT_TIMEOUT, version=None):
        self._createdir()  # Cache dir can be deleted at any time.
        fname = self._key_to_file(key, version)
        self._cull()  # make some room if necessary
        fd, tmp_path = tempfile.mkstemp(dir=self._dir)
        renamed = False
        try:
            with open(fd, "wb") as f:
                self._write_content(f, timeout, value)
            file_move_safe(tmp_path, fname, allow_overwrite=True)
            renamed = True
        finally:
            if not renamed:
                os.remove(tmp_path)

    def touch(self, key, timeout=DEFAULT_TIMEOUT, version=None):
        try:
            with open(self._key_to_file(key, version), "r+b") as f:
                try:
                    locks.lock(f, locks.LOCK_EX)
                    if self._is_expired(f):
                        return False
                    else:
                        previous_value = pickle.loads(zlib.decompress(f.read()))
                        f.seek(0)
                        self._write_content(f, timeout, previous_value)
                        return True
                finally:
                    locks.unlock(f)
        except FileNotFoundError:
            return False

    def delete(self, key, version=None):
        return self._delete(self._key_to_file(key, version))

    def _delete(self, fname):
        if not fname.startswith(self._dir) or not os.path.exists(fname):
            return False
        try:
            os.remove(fname)
        except FileNotFoundError:
            # The file may have been removed by another process.
            return False
        return True

    def has_key(self, key, version=None):
        fname = self._key_to_file(key, version)
        try:
            with open(fname, "rb") as f:
                return not self._is_expired(f)
        except FileNotFoundError:
            return False

    def _cull(self):
        """
        Remove random cache entries if max_entries is reached at a ratio
        of num_entries / cull_frequency. A value of 0 for CULL_FREQUENCY means
        that the entire cache will be purged.
        """
        from django import native as _native

        filelist = self._list_cache_files()
        num_entries = len(filelist)
        if _native.AVAILABLE:
            if not _native.cache_cull_needed(num_entries, self._max_entries):
                return
            sample = _native.cache_cull_sample_size(
                num_entries, self._cull_frequency
            )
            if sample == 0:
                return self.clear()
            filelist = random.sample(filelist, sample)
            for fname in filelist:
                self._delete(fname)
            return
        if num_entries < self._max_entries:
            return  # return early if no culling is required
        if self._cull_frequency == 0:
            return self.clear()  # Clear the cache when CULL_FREQUENCY = 0
        # Delete a random selection of entries
        filelist = random.sample(filelist, int(num_entries / self._cull_frequency))
        for fname in filelist:
            self._delete(fname)

    def _createdir(self):
        # Workaround because os.makedirs() doesn't apply the "mode" argument
        # to intermediate-level directories.
        # https://github.com/python/cpython/issues/86533
        safe_makedirs(self._dir, mode=0o700, exist_ok=True)

    def _key_to_file(self, key, version=None):
        """
        Convert a key into a cache file path. Basically this is the
        root cache path joined with the md5sum of the key and a suffix.
        """
        from django import native as _native

        key = self.make_and_validate_key(key, version=version)
        digest = md5(key.encode(), usedforsecurity=False).hexdigest()
        if _native.AVAILABLE:
            fname = _native.cache_file_name(digest, self.cache_suffix)
        else:
            fname = "".join([digest, self.cache_suffix])
        return os.path.join(self._dir, fname)

    def clear(self):
        """
        Remove all the cache files.
        """
        for fname in self._list_cache_files():
            self._delete(fname)

    def _is_expired(self, f):
        """
        Take an open cache file `f` and delete it if it's expired.
        """
        from django import native as _native

        try:
            exp = pickle.load(f)
        except EOFError:
            exp = 0  # An empty file is considered expired.
        now = time.time()
        if _native.AVAILABLE:
            expired = _native.cache_timestamp_expired(
                exp is None, 0.0 if exp is None else float(exp), now
            )
        else:
            expired = exp is not None and exp < now
        if expired:
            f.close()  # On Windows a file has to be closed before deleting
            self._delete(f.name)
            return True
        return False

    def _list_cache_files(self):
        """
        Get a list of paths to all the cache files. These are all the files
        in the root cache dir that end on the cache_suffix.
        """
        return [
            os.path.join(self._dir, fname)
            for fname in glob.glob(f"*{self.cache_suffix}", root_dir=self._dir)
        ]
