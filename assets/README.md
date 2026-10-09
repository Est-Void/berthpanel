# Demo media

The README's "paste a URL, get a unit running" section expects a short
recording here as `demo.gif`. It requires a live host (a real terminal and a
systemd **user** manager), so it is produced by hand, not by CI — see the
`--selftest` note in dev.md §9.

Suggested recording (keep it short — one URL to a running unit):

```sh
asciinema rec demo.cast        # paste a GitHub URL, confirm the draft, wait
agg demo.cast assets/demo.gif  # asciinema/agg -> gif
```

Then uncomment the image line in the README's demo section. A synthetic
recording that only *looks* like a deploy is exactly the kind of dishonest
demo this project avoids — record the real thing.
