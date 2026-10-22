# Weather

On the ESP-IDF firmware the Weather app shows MET Norway's forecast, the data yr.no is built
on, for the Orb's location (the centre the Flight Tracker uses; Settings > Location). Stage 8
of `REWRITE_PLAN.md`. It replaces nothing: the old radar app that shared the name was left out
on purpose, because it was the memory problem.

## On the screen

- **Now** (where the app opens): the place, an icon, the temperature, the wind (speed and the
  direction it comes from), the rain in the next hour, and "MET Norway · n min ago".
- **Turn the knob** into a wheel: the next 12 hours (time, icon, temperature, rain), then today
  and the next six days (lowest and highest temperature, the day's rain, the icon for the six
  hours around midday). The selected row is the large one in the middle.
- **Press** to come back to now. Rock the knob to leave, as everywhere.

Units follow Settings > Units: °C, m/s and mm, or °F, mph and inches.

## Where it comes from

`main/yr.cpp` asks MET's Locationforecast 2.0 (`/compact`) for the Orb's coordinates, rounded to
four decimals, through `orb_net`:

- **MET's terms**, which this keeps: an identifying User-Agent (`TheOrbOS/<version>
  github.com/bobba99/orb-os`), coordinates to four decimals, honour `Expires` and send
  `If-Modified-Since` (`orb_net`'s cache does both), and no more requests than the forecast
  changes. It asks every ten minutes at most; an unchanged forecast costs a 304. A new location
  is fetched at once.
- **Parsed as it arrives**, with a filter that keeps only the time, the temperature, the wind and
  each period's symbol and rain (about 35 KB on the wire).
- **Days** are local dates. A day's rain adds each time step's own period, the hourly steps while
  MET gives hours and the six-hourly ones after, so no hour is counted twice.

The screen (`src/yr_view.cpp`) reads the result through `src/yr.h`, so it is the same code in the
simulator, where `--yrshot <prefix>` draws it from a fixed example forecast.

## Icons

Twelve plain shapes (sun, moon, sun or moon behind a cloud, cloud, fog, light rain, rain, heavy
rain, sleet, snow, thunder) drawn by `tools/make_weather_icons.py` into `src/wx_icons_data.c` as
alpha-only images, so they take the theme's icon colour. MET's ~40 symbol codes map onto them in
`src/wx_icons.cpp`; a code it does not know draws as a cloud. Original work, under this
repository's licence.

## Theming

`weather_style.json` keys for this screen (the file's other keys belong to the old radar app):

| Key | What |
|---|---|
| `bg` | Background colour; `weather_plate.png` goes over it when the theme ships one |
| `yrText` | Text |
| `yrDim` | Quieter text: the place, the wheel's other rows, the credit |
| `yrAccent` | The wheel's heading |
| `yrIcon` | The icons |

An Orb Studio theme's `apps.weather` and `names.weather` described the old radar app, so on
ESP-IDF only a schema-1 theme (docs/themes.md) can hide this app or rename it.

## Credits

MET Norway's data is CC BY 4.0: "MET Norway" under the forecast, and "Weather: MET Norway
(CC BY 4.0)" on the About page.
