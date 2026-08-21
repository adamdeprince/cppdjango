from django.utils.version import get_version as get_upstream_version

VERSION = (6, 1, 1, "alpha", 0)

# cppdjango follows the compatible upstream release and adds a PEP 440 post
# release component. Keep VERSION as Django's public five-element compatibility
# tuple; distribution and native-extension identity use __version__.
UPSTREAM_VERSION = get_upstream_version(VERSION)
__version__ = "6.1.1a0.post0"

# This fork ships an optional nanobind/C++26 acceleration layer (django.native).
NATIVE_FORK = True


def get_version(version=None):
    """Return the cppdjango release, or an explicitly supplied Django version."""
    if version is None:
        return __version__
    return get_upstream_version(version)


def setup(set_prefix=True):
    """
    Configure the settings (this happens as a side effect of accessing the
    first setting), configure logging and populate the app registry.
    Set the thread-local urlresolvers script prefix if `set_prefix` is True.
    """
    from django.apps import apps
    from django.conf import settings
    from django.urls import set_script_prefix
    from django.utils.log import configure_logging

    configure_logging(settings.LOGGING_CONFIG, settings.LOGGING)
    if set_prefix:
        set_script_prefix(
            "/" if settings.FORCE_SCRIPT_NAME is None else settings.FORCE_SCRIPT_NAME
        )
    apps.populate(settings.INSTALLED_APPS)
