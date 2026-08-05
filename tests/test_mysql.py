# Test settings for MySQL (Homebrew mysql).
#
# Requires databases django_test / django_test_other and user django/django
# with CREATE privileges for test_* databases; mysqlclient installed.

DATABASES = {
    "default": {
        "ENGINE": "django.db.backends.mysql",
        "NAME": "django_test",
        "USER": "django",
        "PASSWORD": "django",
        "HOST": "127.0.0.1",
        "PORT": "3306",
        "OPTIONS": {
            "charset": "utf8mb4",
        },
    },
    "other": {
        "ENGINE": "django.db.backends.mysql",
        "NAME": "django_test_other",
        "USER": "django",
        "PASSWORD": "django",
        "HOST": "127.0.0.1",
        "PORT": "3306",
        "OPTIONS": {
            "charset": "utf8mb4",
        },
    },
}

SECRET_KEY = "django_tests_secret_key"

PASSWORD_HASHERS = [
    "django.contrib.auth.hashers.MD5PasswordHasher",
]

USE_TZ = False

DEFAULT_AUTO_FIELD = "django.db.models.AutoField"
