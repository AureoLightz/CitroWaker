# Optional QoL and gyro controls

Implementation exists in the local development tree; this GitHub repository does not yet include the port source or a downloadable release. The current local packaged ELF contains the gyro sensor and aiming functions, but their inclusion does not establish correct aiming behaviour. Gameplay and physical sensor validation remain pending. Check the notes for your specific build; there is no downloadable release in this repository yet.

An optional `/3ds/ww3ds/qol.ini` is read once at controller startup. Example:

```ini
sail=swift
fast_grapple=1
no_song_replay=1
instant_text=0
gyro=1
gyro_sensitivity=100
gyro_invert_x=0
gyro_invert_y=0
```

| Setting | Values | Default |
|---|---|---|
| sail | original, swift, wind | original |
| fast_grapple | 0 / 1 | 0 |
| no_song_replay | 0 / 1 | 0 |
| instant_text | 0 / 1 | 0 |
| gyro | 0 / 1 | 0 |
| gyro_sensitivity | integer 25–300 percent | 100 |
| gyro_invert_x | 0 / 1, motion yaw only | 0 |
| gyro_invert_y | 0 / 1, motion pitch only | 0 |

Swift selects the existing faster-sailing/automatic-wind hooks; wind changes wind direction without extra speed. The other gameplay options reuse SwitchWaker's existing optional hooks. No new inventory slot, quest shortening or save format is introduced by this loader. They are not promised to reproduce every WWHD detail.

Restart after changing this file. Missing file changes nothing. A valid file resets omitted supported options to the defaults above. Unknown/duplicate keys, invalid values or oversized input reject the whole file, preserving previous environment/settings values. Full-line # and ; comments and whitespace are supported. No per-frame file access. Separate from performance.ini and buttons.ini.

Gyro adds motion to the existing first-person aiming camera paths; stick input remains available. Sensitivity and inversion affect motion only. It does not add gyro to walking or automatically modify target selection. Sensor activation is cached; a failed activation is not retried each frame, but may retry after aim reset. Input blocking and virtual-controller takeover stop the sensors. Reset clears the previous orientation estimate. Disabling gyro also stops sensors.

At 3–6 FPS, the 3DS path uses actual elapsed motion up to 0.5 s; longer gaps are discarded. It reads the latest rate once per game frame, so motion entirely between samples can still be missed. No new thread or extra hidScanInput is added.

Pending before calling this player-ready: physical-axis/direction and drift/feel checks, aiming/zoom/pause/cutscene coverage, and review of sensor cleanup with the current application-exit path. The current exit path terminates the process after APT cleanup rather than calling hidExit underneath live game threads; a stop-before-hidExit hook is therefore not an accurate description of that path. Automated host tests cover the actual sensor backend with mocked libctru, actual PAD forwarding/block/virtual paths, production gyro timing/reset/inversion, configuration rejection and 32,768 unchanged stock button inputs. These cannot establish real sensor orientation or calibration.
