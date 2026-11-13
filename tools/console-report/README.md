# Console log report

A standalone Python 3 tool for the port's console log. No dependencies, game files, network access or emulator required.

```sh
python3 report.py /path/to/log.txt --start-frame 360
python3 report.py /path/to/before.txt /path/to/after.txt --start-frame 360
python3 -m unittest -v test_report.py
```

The report uses interval-weighted presentation blocks, labels sparse process/render samples separately, reports logged memory extrema, and summarizes depth probes, settings, captures and texture activity. Capture stalls are flagged rather than silently removed. Optional `--full-depth` prints every parsed probe row.

A comparison is descriptive: it does not establish that the logs used identical scenes/settings or prove a causal speedup. Process and render overlap, so their wall times cannot simply be added. Missing information remains “not logged.” The tool prints source path/build/diagnostic strings from the input: review a generated report before sharing it. No private or real-console log is shipped; tests use synthetic samples.
