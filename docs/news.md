# News (Headlines)

On the ESP-IDF firmware the Headlines screen reads Norwegian news straight from the publishers'
RSS feeds, over HTTPS, with no gateway in between (`main/news.cpp`). Stage 7 of
`REWRITE_PLAN.md`. The screen itself is the one the Arduino firmware had (`src/intel_view.cpp`);
only where its stories come from changed.

## Where the stories come from

Two publishers, NRK and VG, one feed each, chosen by the theme's topic
(`intel_style.json`, `"topic"`):

| Topic | NRK | VG |
|---|---|---|
| `general` (and anything else) | `nrk.no/toppsaker.rss` | `vg.no/rss/feed/?categories=1068` (Nyheter) |
| `world` | `nrk.no/urix/toppsaker.rss` | `?categories=1070` (Utenriks) |
| `sports` | `nrk.no/sport/toppsaker.rss` | `?categories=1072` (Sport) |
| `science`, `tech`, `space` | `nrk.no/viten/toppsaker.rss` | none: VG has no such section |

**Settings > News** picks which of the two are read: NRK and VG (the default), NRK only, or VG
only. It is saved on the Orb (NVS `newsSrc`) and a change is fetched at once. The theme's old
`"source"` (`bbc`, `guardian`) belonged to the Orb Studio gateway and is ignored here.

## How they are read

- **Streamed.** The XML is parsed with expat as it arrives (`espressif/expat` 2.8.4), and reading
  stops once a feed has given 20 stories, so most of NRK's 109 KB top-stories feed is never
  downloaded (about 22 KB is).
- **Merged** newest first by each story's `<pubDate>`, every story credited to its source on
  screen ("NRK", "VG"). Headlines are cut to 70 characters on a word boundary, with an ellipsis;
  full UTF-8 is kept, which the built-in fonts draw since stage 6.
- **Summaries.** Pressing a headline shows the story's `<description>`. It is kept with the list
  (in PSRAM), so it appears at once and needs no second request.
- **Politely.** Conditional requests through `orb_net`'s cache (`If-Modified-Since`; VG sends
  `Last-Modified`), and never more than one round in five minutes whatever the theme's
  `pollMinutes` asks. A feed that has not changed costs one short request.

## Your own feeds: /news.json

A card can name its own feeds instead. Put `news.json` in the card's top folder:

```json
{
  "feeds": [
    { "label": "Rogaland",     "url": "https://www.nrk.no/rogaland/toppsaker.rss" },
    { "label": "VG Sport",     "url": "https://www.vg.no/rss/feed/?categories=1072" }
  ]
}
```

- Up to six feeds, each an `https://` RSS 2.0 address and a short label (it is what the screen
  credits; 11 characters at most).
- When the file is there and readable it replaces the built-in list entirely, and Settings > News
  and the theme's topic no longer choose anything. A file that is not valid JSON is reported in
  the log and the built-in feeds are used.
- NRK has a feed per region (`/rogaland/`, `/vestland/`, `/tromsogfinnmark/` and so on) and per
  section (`/norge/`, `/kultur/`); VG's categories are numbers (1068 news, 1069 domestic,
  1070 foreign, 1072 sport, 1073 football).

## Credits

NRK and VG are credited beside every story and on the About page ("News: NRK, VG"). The Orb
shows headlines and the feeds' own short summaries with the source named, as a feed reader does;
it never shows the articles themselves.
