from django.db import models


class FastWorld(models.Model):
    randomnumber = models.IntegerField()

    class Meta:
        app_label = "native_orm_fastpath"


class FastFortune(models.Model):
    message = models.CharField(max_length=255)

    class Meta:
        app_label = "native_orm_fastpath"


class FastAuthor(models.Model):
    name = models.CharField(max_length=64)

    class Meta:
        app_label = "native_orm_fastpath"


class FastArticle(models.Model):
    title = models.CharField(max_length=64)
    author = models.ForeignKey(
        FastAuthor, on_delete=models.CASCADE, related_name="articles"
    )

    class Meta:
        app_label = "native_orm_fastpath"
