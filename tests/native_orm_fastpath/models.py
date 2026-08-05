from django.db import models


class FastWorld(models.Model):
    randomnumber = models.IntegerField()

    class Meta:
        app_label = "native_orm_fastpath"


class FastFortune(models.Model):
    message = models.CharField(max_length=255)

    class Meta:
        app_label = "native_orm_fastpath"
