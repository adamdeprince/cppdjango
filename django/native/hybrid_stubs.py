"""
Lightweight hybrid-path stubs for cold GET requests (no session cookie).

Avoid constructing SessionStore / SimpleLazyObject until the view actually
touches session or needs a real user load.
"""


class ColdSession:
    """
    Stand-in for SessionStore when there is no session cookie.

    ``accessed`` / ``modified`` stay False until an attribute or item access
    upgrades to a real SessionStore via ``_factory``.
    """

    __slots__ = ("_factory", "_store")

    def __init__(self, factory):
        object.__setattr__(self, "_factory", factory)
        object.__setattr__(self, "_store", None)

    def _ensure(self):
        store = object.__getattribute__(self, "_store")
        if store is None:
            store = object.__getattribute__(self, "_factory")()
            object.__setattr__(self, "_store", store)
        return store

    def _raw_store(self):
        return object.__getattribute__(self, "_store")

    @property
    def accessed(self):
        store = self._raw_store()
        return False if store is None else store.accessed

    @property
    def modified(self):
        store = self._raw_store()
        return False if store is None else store.modified

    @property
    def session_key(self):
        store = self._raw_store()
        return None if store is None else store.session_key

    def is_empty(self):
        store = self._raw_store()
        return True if store is None else store.is_empty()

    def __contains__(self, key):
        store = self._raw_store()
        if store is None:
            return False
        return key in store

    def get(self, key, default=None):
        store = self._raw_store()
        if store is None:
            return default
        return store.get(key, default)

    def __getitem__(self, key):
        return self._ensure()[key]

    def __setitem__(self, key, value):
        self._ensure()[key] = value

    def __delitem__(self, key):
        del self._ensure()[key]

    def __getattr__(self, name):
        return getattr(self._ensure(), name)

    def __setattr__(self, name, value):
        if name in ColdSession.__slots__:
            object.__setattr__(self, name, value)
        else:
            setattr(self._ensure(), name, value)
