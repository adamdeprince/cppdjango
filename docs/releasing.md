# Releasing cppdjango

cppdjango releases use the upstream Django version plus a PEP 440 post-release
component. The first fork release based on Django 6.0.7 is `6.0.7.post1`, and
the Git tag has exactly the same name.

The distribution target is the `cppdjango` project on production PyPI. It must
never be uploaded as the upstream `Django` distribution.

## Release identity

Update these together:

- `django.VERSION`: the five-element compatible upstream Django version;
- `django.__version__`: the public cppdjango distribution version;
- exact-version installation examples in `README.rst`, `INSTALL`,
  `docs/installation.md`, and `html/`;
- the release tag, which must equal `django.__version__`.

The scikit-build metadata provider reads `django.__version__`. CMake embeds the
same `SKBUILD_PROJECT_VERSION` into `django._native`, so the distribution,
Python facade, and C++ extension report one release identity.

## Validate

Use a clean virtual environment with `build`, `twine`, and the platform wheel
repair tool installed:

```console
python -m pip install --upgrade build twine
# macOS:
python -m pip install --upgrade delocate
# Linux:
python -m pip install --upgrade auditwheel
python -m build
python -m twine check dist/*
```

Install both the wheel and source distribution into separate clean virtual
environments. In each one, verify:

```console
python -c "import django; from django import native; print(django.__version__, native.version(), native.compiler())"
```

The two versions must equal the proposed release and the compiler must not be
`none`. Run the native regression suites before publishing.

## Commit, tag, and upload

Commit the exact release tree and create an annotated tag matching the package
version:

```console
git tag -a 6.0.7.post1 -m "cppdjango 6.0.7.post1"
```

The fork-specific publisher refuses a dirty tree, a project name other than
`cppdjango`, or a missing exact tag. It builds into a temporary directory,
repairs native-library references with `delocate` on macOS or `auditwheel` on
Linux, checks every artifact, and uploads only `cppdjango-*` files through the
`pypi` repository configured for twine:

```console
scripts/publish_cppdjango.sh
```

Do not use Django's upstream `scripts/do_django_release.py`; it targets Django
Foundation release infrastructure and the upstream distribution.

After upload, create a clean environment and install the exact version from
PyPI. Confirm that PyPI, import metadata, and the native module agree before
announcing the release.
