# Chop Zone Detector — TradingView Indicator

A Pine Script v6 overlay indicator that identifies choppy, range-bound price
action and shades it on the chart as a **no-trade zone**, so you stop selling
into support and buying into resistance during consolidation.

## What it does

- **Orange boxes** mark chop zones. The box top is the local resistance, the
  box bottom is the local support, and both expand as the range develops.
- **Red triangle (above bar)** — price is pressing into the *top* of an active
  chop zone. This is where buying into resistance fails. Don't buy.
- **Green triangle (below bar)** — price is pressing into the *bottom* of an
  active chop zone. This is where selling into support fails. Don't sell.
- **Dashboard (top-right)** shows the live regime — `CHOP — STAND ASIDE`,
  `CAUTION — HTF CHOP`, or `TRADEABLE` — the current Choppiness Index and ADX
  readings, plus a CHOP/CLEAR status for each higher timeframe.
- **Higher-timeframe check** runs the same detection on two HTFs (defaults:
  11m and 1h, matching a 2-minute entry chart). A clean entry chart inside
  HTF chop is still a no-trade, and the regime row downgrades to CAUTION.
- **Background tint** while chop is active, as an at-a-glance warning.

## How it detects chop

Two independent regime filters must agree (by default):

| Filter | Choppy when | Default |
|---|---|---|
| Choppiness Index | above threshold | > 61.8 |
| ADX | below threshold | < 20 |

A zone is only drawn after **4 consecutive choppy bars** (configurable), and
only closes after **3 consecutive trending bars**, so single-bar fakeouts
don't flicker zones on and off. Defaults are tuned for fast intraday futures
(MES/MNQ on a 2-minute chart).

## Installation

1. Open any chart on TradingView.
2. Open the **Pine Editor** (bottom panel).
3. Delete the placeholder code and paste the contents of
   `chop-zone-detector.pine`.
4. Click **Save**, then **Add to chart**.

## Alerts

Right-click the chart → *Add alert* → set **Condition** to *Chop Zones* and
pick one of:

- **Chop zone started** — stand aside.
- **Chop zone ended** — trend conditions returning.
- **Price at chop resistance** — fires the moment price enters the top edge
  band of an active zone (the "don't buy here" alert).
- **Price at chop support** — fires when price enters the bottom edge band
  (the "don't sell here" alert).
- **Choppy on all timeframes** — entry chart and both higher timeframes are
  choppy at once: full stand-aside conditions.

## Tuning

| Setting | Effect |
|---|---|
| Signal mode | `Both agree` (default) = fewer, higher-confidence zones. `Either one` = flags more borderline chop — use this if zones are appearing too late or too rarely. |
| Choppiness threshold | Lower (e.g. 58) = more sensitive; higher (e.g. 65) = only deep consolidation. |
| ADX threshold | Raise to ~25 on higher timeframes or slow markets; lower to ~18 for fast movers. |
| Higher timeframes | Set to your two HTFs (defaults 11m / 1h). Turn off the check entirely with the toggle if you only want entry-chart zones. |
| Bars to confirm a chop zone | Higher = fewer false zones but later warning. 3–5 works well intraday; 5–8 on daily charts. |
| Bars to confirm chop has ended | Raise if a single breakout bar keeps closing zones that then resume chopping. |
| Edge warning band | The % of the zone height that counts as "at the edge." 20% default; tighten to 10–15% if triangles fire too often inside the range. |

Note: zone detection necessarily lags by the confirmation period — the box
backfills to where the chop actually began, but the first warning arrives a
few bars in. That's the trade-off for not painting false zones during every
brief pause in a trend.
