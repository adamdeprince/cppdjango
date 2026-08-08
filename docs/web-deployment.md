# Deploying the cppdjango website

The complete static website is the repository's `html/` directory. It has no
application server or build step. Copy the *contents* of that directory to the
document root for `https://django.goblinreactor.com/` so these routes retain
their intended public URLs:

- `/` — the cppdjango product and benchmark page;
- `/install/` — installation and native-build instructions;
- `/artifacts/` — the public benchmark evidence ledger;
- `/goblin.png` — mascot, social image, and favicon;
- `/robots.txt` and `/sitemap.xml` — crawler metadata.

For example:

```console
rsync -av html/ web-user@example:/var/www/django.goblinreactor.com/
```

Do not copy the containing `html` directory as an extra path component. The
site uses a few root-relative links because it is designed to be served at the
domain root.

An nginx virtual host only needs static-file handling:

```nginx
server {
    listen 443 ssl;
    server_name django.goblinreactor.com;
    root /var/www/django.goblinreactor.com;
    index index.html;

    location / {
        try_files $uri $uri/ =404;
    }
}
```

TLS certificate and logging directives are deployment-specific and are not
included here. After copying, verify:

```console
curl -f https://django.goblinreactor.com/
curl -f https://django.goblinreactor.com/install/
curl -f https://django.goblinreactor.com/artifacts/benchmark-summary.json
curl -f https://django.goblinreactor.com/artifacts/SHA256SUMS
```

The release process also creates a copy-ready
`cppdjango-site-6.0.7.post1.tar.gz` archive from `html/`. Extract that archive
directly into the document root.
