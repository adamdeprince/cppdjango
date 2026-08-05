# Test settings for PostgreSQL (Homebrew postgresql@17).
#
# Requires databases django_test / django_test_other owned by the OS user
# (or user "django") and psycopg installed in the test venv.

DATABASES = {
    "default": {
        "ENGINE": "django.db.backends.postgresql",
        "NAME": "django_test",
        "USER": "",  # peer/trust as current OS user via Homebrew default
        "PASSWORD": "",
        "HOST": "/tmp",  # common Homebrew socket dir; empty falls back to default
        "PORT": "",
    },
    "other": {
        "ENGINE": "django.db.backends.postgresql",
        "NAME": "django_test_other",
        "USER": "",
        "PASSWORD": "",
        "HOST": "/tmp",
        "PORT": "",
    },
}

# Homebrew often uses /tmp/.s.PGSQL.5432 — prefer empty HOST for libpq default.
DATABASES["default"]["HOST"] = ""
DATABASES["other"]["HOST"] = ""

SECRET_KEY = "django_tests_secret_key"

PASSWORD_HASHERS = [
    "django.contrib.auth.hashers.MD5PasswordHasher",
]

USE_TZ = False

DEFAULT_AUTO_FIELD = "django.db.models.AutoField"
