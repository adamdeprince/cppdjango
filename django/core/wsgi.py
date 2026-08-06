import django
from django.core.handlers.wsgi import WSGIHandler


def get_wsgi_application():
    """
    The public interface to Django's WSGI support. Return a WSGI callable.

    Avoids making django.core.handlers.WSGIHandler a public API, in case the
    internal WSGI implementation changes or moves in the future.

    With the native extension loaded, ``WSGIHandler.__call__`` may execute the
    request loop in C++ (views remain Python). ``DJANGO_NATIVE=0`` forces the
    pure-Python path.
    """
    django.setup(set_prefix=False)
    return WSGIHandler()