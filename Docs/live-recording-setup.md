# Recording the GenBridge effect in Ableton Live

Live records what comes INTO a track, not what its plug-ins produce, so recording GenBridge takes two
tracks. In a busy set Live then holds the GenBridge track back to line up with the slowest plug-in
elsewhere, and takes land late (about 300 ms in CT's set). Reporting that delay as GenBridge's Extra
Latency fixes both the take and the monitoring (confirmed by CT, 2026-10-09). The Align role measures
and sets it (gbAlign notes §1).

## Setup

1. **Track 1 - "Guitar (GenBridge)":** an audio track with the **GenBridge** effect.
   - Role: **Capture a device**. Device: the Helix (or whatever carries the guitar); First Channel /
     Mode as needed.
   - Audio From: **No Input**. Output: your hardware outputs (or Master) - this is what you monitor.
   - Track Delay: **0**. Extra Latency: leave it - Align sets it.
2. **Track 2 - "Guitar (record)":** a second audio track.
   - Audio From: **1-Guitar (GenBridge)**, second chooser **Post FX**.
   - A second **GenBridge** effect on it, Role: **Align a recording**. It captures nothing and passes the
     track through; leave its Device at None.
   - Monitor: **In** (Live may not run a track's plug-ins with monitoring off). Output: **Sends Only**,
     or the fader down, so you do not hear the guitar twice.
   - Track Delay: **0**. Arm it - this is the track the takes land on.
3. Options menu: **Delay Compensation ON** (the normal setting).

## Use

4. Start playback (or just play, with Live running) and play something with clear attacks for a few
   seconds. The Align panel reads "N ms late - correcting", then **"aligned"**; track 1's Extra row shows
   N with "set by Align on the recording track".
5. Record. Takes land on the beat; monitoring through track 1 is immediate.
6. Added or removed heavy plug-ins? Adding is picked up by itself. After **removing** one, press
   **Re-align** (Extra Latency is then larger than needed, which makes the whole set later).

## What the Align panel can say

| Says | Means |
|---|---|
| listening | nothing measured yet - play |
| no GenBridge is capturing in this set | no capture instance is running |
| nothing arriving here while GenBridge plays | check Audio From / Post FX, and set Monitor to In |
| cannot match the audio yet | sustained or very even sound - play attacks |
| N ms late - correcting | a new Extra Latency was sent; waiting for Live to use it |
| still N ms late after the change | Live has not re-read the latency - stop and restart playback |
| N ms late - correcting after this take | it does not change anything while Live is recording |
| beyond what Extra Latency can add | the set needs more than 1000 ms |
| two GenBridges send the same audio | two capture instances on one input - it cannot tell which is recorded here |

## Doing it by hand

Without an Align instance: set track 1's **Extra** to how late a test take lands (Track Delay 0 on
both tracks), and record again. Too much makes everything in the set later, the click included.

## Draft for Read Me First.txt

```
RECORDING THE EFFECT IN ABLETON LIVE

Live records what comes INTO a track, not what its plug-ins produce, so you
need two tracks:

  1. Put GenBridge on an audio track, Role "Capture a device", and choose
     your device. Set that track's Audio From to "No Input".
  2. On a second audio track set Audio From to the first track, and the
     second chooser to "Post FX". Put a second GenBridge on it with Role
     "Align a recording". Set its Monitor to In and its output to
     "Sends Only", and arm it - this is the track you record on.

Play for a few seconds: the second GenBridge measures how far Live delays
the first track (Live lines every track up with the slowest plug-ins in
the project) and sets the first one's Extra Latency to match. Takes then
land on the beat, and the first track monitors without that delay.

After removing heavy plug-ins from the project, press Re-align.
```
