=========
cppdjango
=========

cppdjango is an independent C++ port of Django 6.0.7. It keeps Django's
familiar ``django`` import namespace and public APIs while moving selected ORM,
request-processing, template, URL, parsing, and utility paths into a C++26
extension. Unsupported values and query shapes conservatively replay through
the compatible Python implementation.

The current PostgreSQL benchmark measures 81.4% less framework-side ORM CPU,
or 436% faster ORM-only CPU performance, across an equal-weight point-select,
ordered ``IN`` select, and point-update suite. PostgreSQL and the shared
cursor/psycopg floor are excluded from that claim. Raw measurements and the
methodology are published at https://django.goblinreactor.com/artifacts/.

Install
========

Use a fresh virtual environment. The PyPI distribution is named
``cppdjango``, but the Python package remains ``django``::

    python3 -m venv .venv
    . .venv/bin/activate
    python -m pip install --upgrade pip
    python -m pip install cppdjango==6.0.7.post1

cppdjango replaces upstream Django. Do not install the ``Django`` and
``cppdjango`` distributions in the same environment because both own the
``django`` import namespace. When converting an existing environment, remove
upstream Django first::

    python -m pip uninstall -y Django
    python -m pip install cppdjango==6.0.7.post1

The package contains native code. If no compatible wheel is available, pip
builds it from source and needs:

* Python 3.12 or newer;
* CMake 3.26 or newer and Ninja or Make;
* a C++ compiler with the C++26 features used by cppdjango;
* OpenSSL and zlib development headers and libraries.

For example, on macOS with Homebrew::

    brew install cmake ninja openssl@3
    CMAKE_PREFIX_PATH="$(brew --prefix openssl@3)" \
      python -m pip install cppdjango==6.0.7.post1

On a Debian-family Linux system, install CMake, Ninja, OpenSSL and zlib
development packages plus a recent compiler. Package names vary by release;
one typical setup is::

    sudo apt install cmake ninja-build g++-15 libssl-dev zlib1g-dev
    CXX=g++-15 python -m pip install cppdjango==6.0.7.post1

Verify both the distribution version and native extension::

    python -c "import django; from django import native; print(django.__version__, native.version(), native.compiler())"

Both versions should report ``6.0.7.post1`` and the compiler should not report
``none``. ``DJANGO_NATIVE=0`` disables the extension for comparison and
diagnostics; it is not needed for ordinary use.

PostgreSQL
==========

Database drivers remain separate, just as they are in Django. For PostgreSQL::

    python -m pip install "psycopg[binary]>=3.1"

Third-party packages that declare a dependency on the distribution name
``Django`` cannot express that ``cppdjango`` supplies the same import API.
Installing such a package may cause pip to add upstream Django. Install and
review those dependencies deliberately, and confirm with::

    python -m pip list | grep -i django

Only ``cppdjango`` should own the ``django`` package in that environment.

Source installation
===================

For development or a source checkout::

    git clone https://github.com/adamdeprince/cppdjango.git
    cd cppdjango
    python3 -m venv .venv
    . .venv/bin/activate
    python -m pip install --upgrade pip
    python -m pip install -e ".[native-dev]"

Full installation notes are in ``docs/installation.md``. Upstream Django's
documentation remains applicable unless a cppdjango document says otherwise:
https://docs.djangoproject.com/en/6.0/.

License and trademark
=====================

cppdjango retains Django's BSD license and attribution. ``Django`` is a
trademark of the Django Software Foundation. cppdjango is independent and is
not endorsed by the Django Software Foundation.
