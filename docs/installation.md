# Installing cppdjango

cppdjango 6.0.7.post1 is a C++ port based on Django 6.0.7. Its distribution
name is `cppdjango`; its Python import namespace and management command remain
`django` and `django-admin` for application compatibility.

## Install from PyPI

Start with a clean virtual environment:

```console
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install cppdjango==6.0.7.post1
```

Do not install the upstream `Django` and `cppdjango` distributions together.
They both install files under the `django` package. To convert an existing
environment, remove upstream Django before installing cppdjango:

```console
python -m pip uninstall -y Django
python -m pip install cppdjango==6.0.7.post1
```

Verify the package identity and native extension:

```console
python -c "import django; from django import native; print(django.__version__, native.version(), native.compiler())"
```

The first two values should be `6.0.7.post1`. The compiler value should name a
C++ compiler rather than `none`.

## Native build prerequisites

If PyPI does not have a compatible wheel, pip builds cppdjango from its source
distribution. The build needs:

- Python 3.12 or newer;
- CMake 3.26 or newer;
- Ninja or Make;
- a compiler implementing the C++26 features used by cppdjango;
- OpenSSL headers and libraries;
- zlib headers and libraries.

The release is validated with GCC 15.2 on Linux and Apple Clang 21 on macOS.

On macOS with Homebrew:

```console
brew install cmake ninja openssl@3
CMAKE_PREFIX_PATH="$(brew --prefix openssl@3)" \
  python -m pip install cppdjango==6.0.7.post1
```

On Debian-family Linux systems, package names depend on the distribution
release. A typical setup is:

```console
sudo apt install cmake ninja-build g++-15 libssl-dev zlib1g-dev
CXX=g++-15 python -m pip install cppdjango==6.0.7.post1
```

If CMake cannot find OpenSSL in a nonstandard prefix, pass it explicitly:

```console
CMAKE_ARGS="-DOPENSSL_ROOT_DIR=/path/to/openssl" \
  python -m pip install cppdjango==6.0.7.post1
```

## PostgreSQL

cppdjango deliberately leaves database drivers as application dependencies.
Install psycopg for PostgreSQL projects:

```console
python -m pip install "psycopg[binary]>=3.1"
```

Use the normal Django `DATABASES` setting. Supported primitive PostgreSQL ORM
terminals use the native plan and binder. Custom fields, expressions,
relations, generated fields, and values needing Python coercion fall back to
the compatible Python path.

## Existing applications and dependency metadata

Existing source code should continue importing `django`. No import rewrite is
required. However, Python package metadata identifies distributions by name,
and a third-party application that declares `Django>=...` does not know that
the separately named `cppdjango` distribution provides the API. Installing
such an application may cause pip to install upstream Django alongside
cppdjango.

Review dependency changes and check the environment after installing plugins:

```console
python -m pip list | grep -i django
```

Only `cppdjango` should own the `django` package. If an application pulled in
upstream Django, uninstall both distributions and reinstall cppdjango followed
by the application with its dependencies handled explicitly.

## Source and editable installs

```console
git clone https://github.com/adamdeprince/cppdjango.git
cd cppdjango
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -e ".[native-dev]"
```

The editable build directory is selected by the active Python wheel tag. To
force a compiler:

```console
CXX=g++-15 CMAKE_ARGS="-DCMAKE_CXX_COMPILER=g++-15" \
  python -m pip install -e ".[native-dev]"
```

## Diagnostics and fallback mode

Native execution is enabled by default. To verify availability:

```console
python -c "from django import native; print(native.AVAILABLE, native.compiler())"
```

Set `DJANGO_NATIVE=0` for a diagnostic run using the compatible Python paths:

```console
DJANGO_NATIVE=0 python manage.py check
```

This switch is useful for isolating extension problems. Unsupported operations
already fall back automatically and do not require the switch.

## Uninstall

```console
python -m pip uninstall cppdjango
```

Because upstream Django and cppdjango share an import namespace, create a new
virtual environment before switching back to upstream Django. This avoids files
left behind by one distribution affecting the other.
