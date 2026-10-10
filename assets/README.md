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

`demo.gif` is embedded in the README's demo section. It was produced from
`preview.mp4` (a real deploy recording, 4K/60fps source) with:

```sh
ffmpeg -i assets/preview.mp4 -vf "fps=10,scale=800:-1:flags=lanczos,palettegen=max_colors=256" palette.png
ffmpeg -i assets/preview.mp4 -i palette.png -lavfi "fps=10,scale=800:-1:flags=lanczos [x]; [x][1:v] paletteuse=dither=bayer:bayer_scale=5" assets/demo.gif
```

Then uncomment the image line in the README's demo section. A synthetic
recording that only *looks* like a deploy is exactly the kind of dishonest
demo this project avoids — record the real thing.
