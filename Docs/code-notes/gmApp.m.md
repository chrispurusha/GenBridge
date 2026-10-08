# gmApp.m notes

The longer comments for `monitor/gmApp.m`, numbered; the code points at each as `// notes §k`.

## 1. file scope

The whole application shell: no nib, no Xcode project (do-monitor builds it), a menu with Quit and one
window whose content is SynthLib's panel view. That view repaints itself at 30 Hz while visible and turns
clicks into logical-unit coordinates, so the app has no render loop of its own - which is also why SynthLib's
plug-in stubs (pluginStubs.c) are the right application entry points here.

Settings are saved on every change and on quit, with whether it was running; a monitor left running starts
again by itself at the next launch.

## 2. the poller

Half a second: device changes are followed by gm_engine_poll() (gmEngine notes §1), and nothing else needs
the main thread on a timer.
