#!/usr/bin/env bash
# Build, validate, and publish the tagged cppdjango release to production PyPI.

set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

project_name=$(python - <<'PY'
import tomllib

with open("pyproject.toml", "rb") as pyproject:
    print(tomllib.load(pyproject)["project"]["name"])
PY
)
release_version=$(python - <<'PY'
import django

print(django.__version__)
PY
)

if [[ "$project_name" != "cppdjango" ]]; then
    echo "Refusing to publish project '$project_name'; expected 'cppdjango'." >&2
    exit 1
fi

if [[ -n "$(git status --porcelain)" ]]; then
    echo "Refusing to publish from a dirty working tree." >&2
    exit 1
fi

if ! git tag --points-at HEAD | grep -Fxq "$release_version"; then
    echo "HEAD must have the exact release tag '$release_version'." >&2
    exit 1
fi

release_root=$(mktemp -d "${TMPDIR:-/tmp}/cppdjango-release.XXXXXX")
trap 'rm -rf "$release_root"' EXIT
artifact_dir="$release_root/artifacts"
raw_wheel_dir="$release_root/raw-wheel"
mkdir -p "$artifact_dir" "$raw_wheel_dir"

python -m build --sdist --outdir "$artifact_dir"
python -m build --wheel --outdir "$raw_wheel_dir"

case "$(uname -s)" in
    Darwin)
        if ! command -v delocate-wheel >/dev/null; then
            echo "Install delocate before publishing a macOS wheel." >&2
            exit 1
        fi
        delocate-wheel \
            --require-archs "$(uname -m)" \
            --wheel-dir "$artifact_dir" \
            "$raw_wheel_dir"/cppdjango-*.whl
        ;;
    Linux)
        if ! command -v auditwheel >/dev/null; then
            echo "Install auditwheel before publishing a Linux wheel." >&2
            exit 1
        fi
        auditwheel repair \
            --wheel-dir "$artifact_dir" \
            "$raw_wheel_dir"/cppdjango-*.whl
        ;;
    *)
        echo "No portable-wheel repair is configured for $(uname -s)." >&2
        exit 1
        ;;
esac

python -m twine check "$artifact_dir"/*
python -m twine upload --repository pypi "$artifact_dir"/cppdjango-*
