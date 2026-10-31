# Device benchmarks

`platforms/eval/devices/` keeps benchmark runs from real devices, one folder per model and one `scenarios.jsonl` per run, named after the date and the commit it ran. The files are the unchanged output of the device run. They show what each metric costs on a real device, so you can see how much faster or slower one device is than another. They are not baselines for a candidate: compare a candidate with a baseline run on the same device.

The table shows, for each device, the median over its runs of each scenario's median. Times are in milliseconds and memory in MiB. Every Kobo run skips `panel.footnote` and `book.open.large`, because the Kobo player can only reopen the book that is already open.

| Scenario | Clara BW, firmware 4.46.23852, Qt 5 | Clara Colour, firmware 6.0.279225, Qt 6 |
| --- | ---: | ---: |
| `book.open` | 2632 ms | 1814 ms |
| `book.close` | 819 ms | 874 ms |
| `page.turn` | 29.4 ms | 36.3 ms |
| `page.turn.chapter` | 23.0 ms | 35.0 ms |
| `panel.menu` | 394 ms | 35.3 ms |
| `panel.dictionary.long` | 308 ms | 172 ms |
| `panel.dictionary.short` | 198 ms | 119 ms |
| `reflow.font-size` | 567 ms | 605 ms |
| `memory.reading` | 200 MiB | 250 MiB |
| `memory.kobalt.reading` | 71 MiB | 86 MiB |
| `panel.settings` | 271 ms | 383 ms |
| `panel.styling.word-spacing` | 576 ms | 609 ms |
| `panel.styling.tracking` | 1207 ms | 980 ms |
| `panel.acknowledgments` | 259 ms | 396 ms |
| `panel.licence` | 493 ms | 374 ms |
| `memory.peak` | 245 MiB | 293 MiB |
| `memory.kobalt.peak` | 116 MiB | 129 MiB |

Both devices had dictionaries installed and used the default settings, with nothing else open. The Clara BW has two runs and the Clara Colour one. Between the two Clara BW runs, most medians differed by less than 5%. `panel.dictionary.short` differed by 20%, `book.close` by 13%, `page.turn` by 10% and `memory.reading` by 6%, so read those as a range. A single run gives no such range.

To add a device, run the evaluation package on it as described in [Release evaluation](release-evaluation.md#device-scenarios), collect the results with `collect-eval.cmake`, copy `scenarios.jsonl` into the device's folder and add its column to the table.
