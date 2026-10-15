# Audio interactions

Open **Interactions → Audio**, press **+**, select a frequency interval in the live spectrum, then choose **Target**. The interval can be dragged at either edge or entered in Hz. Regions offer Sub, Bass, Body, Presence and Air starting points. Arrow keys move the last spectrum boundary; Shift makes a finer adjustment.

The source is PINOL's playing program mix, before master volume. It does not capture a microphone or a system loopback. Measure can be a selected frequency band, the overall level, or global attacks. Frequency selection applies to the band measure. For band-specific transients, keep Band and enable the existing Pulse Detection filter.

After selecting the sound, set its input range, output target range, envelope/filter and response curve. New Audio interactions use a 12 ms attack / 200 ms release filter. Right-click a parameter → Learn Interaction → Audio opens a route to that parameter without waiting for a controller event. Frequency limits and measurement mode can also be included in presets and snapshots.

Analysis uses a stereo power spectrum (opposite-phase channels do not cancel), a 4096-sample Hann window and a 1024-sample hop. At 48 kHz this is an 85.3 ms window, refreshed every 21.3 ms. Selected-bin energy is integrated with fractional edge weights and compressed into 0–1. The graph displays log frequency from 20 Hz to 20 kHz. Overall level and global onset keep their shorter existing time-domain envelopes.

The audio callback owns all FFT working storage; it performs no analysis allocation or mutex acquisition. UI and engine read atomic spectrum bins with a bounded retry and never hold up the callback. Only the visible spectrum widget repaints, at 25 Hz; it reuses the same widget during a drag.

Older program-audio links (`Internal`, composition `audio/level`, `bass`, `mid`, `high`, `onset`) migrate to Audio on load. Other Internal links retain their source and behavior. Audio source enum values are appended so existing keyboard/MIDI/OSC/Internal files retain their indices.

`audio_analysis` checks frequency isolation, calibration, phase safety and silence/reset. `audio_interaction_ui` checks migration, save/load of custom bands, Audio tab selection, editable bounds, undo and parameter-menu access. The sound integration test checks that a real 440 Hz stream drives an in-band route and stops driving it when the band is moved away.
