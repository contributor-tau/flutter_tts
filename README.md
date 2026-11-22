# Text To Speech

[![pub package](https://img.shields.io/pub/v/flutter_tts.svg?style=for-the-badge&colorB=green)](https://pub.dartlang.org/packages/flutter_tts)

A flutter text to speech plugin (Swift,Kotlin)

## Features

- [x] Android, iOS, Web, Windows, macOS & Linux
  - [x] speak
  - [x] stop
  - [x] get languages
  - [x] set language
  - [x] set speech rate
  - [x] set speech volume
  - [x] set speech pitch
  - [x] get voices
  - [x] set voice
- [x] Android, iOS, Web, macOS & Linux
  - [x] is language available
- [x] Android, iOS, Web & Linux
  - [x] speech marks (requires iOS 7+, Android 26+, and default voice engine for web; sentence-level on Linux)
- [x] Android, iOS
  - [x] synthesize to file (requires iOS 13+)
- [x] Android, iOS, Web, Windows & Linux
  - [x] pause
- [x] Android & Linux
  - [x] get engines
  - [x] set engine
  - [x] get default engine
  - [x] set queue mode
- [x] Android
  - [x] set silence
  - [x] is language installed
  - [x] are languages installed
  - [x] get default voice
  - [x] get max speech input length
- [x] iOS
  - [x] set shared instance
  - [x] set audio session category

## Usage

## macOS

```bash
OSX version: 10.15
```

[Example App](https://github.com/dlutton/flutter_tts/tree/macOS_app) from the macOS_app branch

## Web

[Website](https://dlutton.github.io/flutter_tts) from the example directory.

**Progress updates on Web**

Progress updates are only supported for native speech synsthesis. Use the default engine to ensure support for progress updates. [Chromium#41195426](https://issues.chromium.org/issues/41195426#comment8)

## Linux

On Linux, flutter_tts uses [Speech Dispatcher](https://github.com/brailcom/speechd), the standard
text-to-speech service on Linux desktops. Speech Dispatcher passes speech to whichever synthesizer
the user has installed (espeak-ng, Piper, RHVoice, Festival, …), so your app automatically uses the
voices and settings the user has configured.

### Requirements

Building needs nothing beyond Flutter's usual Linux toolchain: the plugin loads the Speech Dispatcher
client library (`libspeechd.so.2`) at runtime, so the libspeechd development headers aren't needed.

At runtime, the user's system needs Speech Dispatcher and at least one synthesizer. Most desktop
distributions install them by default (they're used by the screen reader). Otherwise:

```bash
# Debian / Ubuntu
sudo apt install speech-dispatcher speech-dispatcher-espeak-ng
# Fedora
sudo dnf install speech-dispatcher speech-dispatcher-espeak-ng
# Arch
sudo pacman -S speech-dispatcher espeak-ng
```

If Speech Dispatcher is missing, your app still starts: `speak` returns `0` and the error handler
receives a message explaining what's missing, and voice/language queries return empty lists.

### Behaviour

- **Engines** are Speech Dispatcher output modules (`getEngines` returns e.g. `["espeak-ng", "piper"]`).
  `setEngine` switches modules; voices and languages then come from the new module.
- **Voices** are returned as `{"name": ..., "locale": ...}` maps, with locales in BCP-47 form
  (`en-US`). espeak-ng reports every voice combined with every voice variant (over 10,000 entries),
  so `voice+variant` combinations are left out of `getVoices` when the base voice is listed. You can
  still select one with `setVoice({"name": "English (America)+Alicia", "locale": "en-US"})`.
- **Speech rate** `0.0`–`1.0` (normal `0.5`), **volume** `0.0`–`1.0` and **pitch** `0.5`–`2.0`
  (normal `1.0`) are mapped onto Speech Dispatcher's ranges.
- **Text is spoken sentence by sentence.** This is what makes `pause` reliable:
  `pause()` stops right away, and calling `speak()` again with the same text resumes at the start of
  the interrupted sentence (calling it with different text starts over, as on Android).
- **Progress events** (`setProgressHandler`) are sentence-level: `start`/`end` cover the sentence
  being spoken and `word` is that sentence.
- **Queue mode** works as on Android: by default (`0`, flush) a new `speak` interrupts the current one;
  with `setQueueMode(1)` utterances are queued. With `awaitSpeakCompletion(true)`, `speak` completes
  with `1` when speech finishes and `0` if it is paused, stopped or interrupted.
- Speech uses Speech Dispatcher's *text* priority, so a screen reader can interrupt it; the cancel
  handler is called when that happens.
- If Speech Dispatcher is restarted, the plugin reconnects on the next call.
- Not supported on Linux: `synthesizeToFile`, `getDefaultVoice`, `setSilence`,
  `isLanguageInstalled`, `areLanguagesInstalled` and `getMaxSpeechInputLength`.

### Flatpak

Speech Dispatcher runs on the host, outside your app's sandbox. Two things are needed:

**1. Permission to reach the host's Speech Dispatcher socket.** Add this to `finish-args` in your
manifest:

```yaml
finish-args:
  - --filesystem=xdg-run/speech-dispatcher:ro
```

The user's host must run Speech Dispatcher. A sandboxed app can't start it, so the host needs either
systemd socket activation (`speech-dispatcher.socket`, shipped by current distribution packages) or a
running daemon (e.g. because a screen reader started it).

**2. The client library inside the sandbox.** Host libraries aren't visible inside a Flatpak, so
`libspeechd.so.2` must come from the runtime or from your app. Check whether your runtime ships it:

```bash
flatpak run --command=sh org.gnome.Platform//47 -c 'ls /usr/lib/*/libspeechd.so.2'
```

If it doesn't, bundle it. Only the client library is needed, not the Speech Dispatcher server. It's a
single C file that depends only on GLib, so this module builds it without the server's dependencies:

```yaml
modules:
  - name: libspeechd
    buildsystem: simple
    sources:
      - type: git
        url: https://github.com/brailcom/speechd.git
        tag: 0.12.1
        commit: 6781ff1709eca1c3d7f748e5361a6aa157dd5f18
    build-commands:
      - sed -e 's/@LIBSPEECHD_MAJOR_VERSION@/0/' -e 's/@LIBSPEECHD_MINOR_VERSION@/12/'
        -e 's/@LIBSPEECHD_MICRO_VERSION@/1/' src/api/c/libspeechd_version.h.in
        > src/api/c/libspeechd_version.h
      - cc -shared -fPIC -O2 -DHAVE_STRNDUP -Iinclude -Isrc/api/c
        -DSPD_SPAWN_CMD='"/usr/bin/speech-dispatcher"'
        $(pkg-config --cflags glib-2.0) src/api/c/libspeechd.c
        $(pkg-config --libs glib-2.0) -lpthread
        -Wl,-soname,libspeechd.so.2 -o libspeechd.so.2
      - install -Dm755 libspeechd.so.2 ${FLATPAK_DEST}/lib/libspeechd.so.2
```

The client library is licensed LGPL-2.1-or-later.

If either piece is missing, the error handler's message says which one, with a pointer to this
section.

### Snap

Snaps currently can't use the host's Speech Dispatcher: snapd has no interface that grants access to
its socket, and inside a snap libspeechd looks for the socket under the snap's private
`$XDG_RUNTIME_DIR`. The working approach is to ship a private Speech Dispatcher inside the snap and
let libspeechd start it on demand. An outline for `snapcraft.yaml`:

```yaml
apps:
  my-app:
    command: my_app
    extensions: [gnome]
    plugs: [audio-playback]
    environment:
      # libspeechd starts this when the app first speaks.
      SPEECHD_CMD: $SNAP/bin/speech-dispatcher-wrapper
      # Use your architecture's triplet, e.g. aarch64-linux-gnu on arm64.
      ESPEAK_DATA_PATH: $SNAP/usr/lib/x86_64-linux-gnu/espeak-ng-data

parts:
  my-app:
    # ... your Flutter part ...
    stage-packages:
      - libspeechd2
      - speech-dispatcher
      - speech-dispatcher-audio-plugins
      - speech-dispatcher-espeak-ng
      - espeak-ng-data
  speechd-wrapper:
    plugin: dump
    source: snap/local
    organize:
      speech-dispatcher-wrapper: bin/speech-dispatcher-wrapper
```

Here `snap/local/speech-dispatcher-wrapper` points the bundled daemon at the snap's modules and configuration:

```sh
#!/bin/sh
exec "$SNAP/usr/bin/speech-dispatcher" \
  --module-dir "$SNAP/usr/lib/speech-dispatcher-modules" \
  --config-dir "$SNAP/etc/speech-dispatcher" "$@"
```

Because this daemon is private to your snap, it doesn't share the user's Speech Dispatcher voices or
settings, and it doesn't coordinate with other apps' speech. Package names above are Ubuntu's; adjust
them to your snap's base.

## Android

Change the minimum Android sdk version to 21 (or higher) in your `android/app/build.gradle` file.

```groovy
minSdkVersion 21
```

**Update the Kotlin Gradle Plugin Version**

Change the verision of the Kotlin Gradle plugin to `1.9.10`. <br>
If your project was created with a version of Flutter before 3.19, go to the `android/build.gradle` file and update the `ext.kotlin_version`:
```groovy
ext.kotlin_version = '1.9.10'
```

Otherwise go to `android/settings.gradle` and update the verion of the plugin `org.jetbrains.kotlin.android`:
```groovy
id "org.jetbrains.kotlin.android" version "1.9.10" apply false
```


Apps targeting Android 11 that use text-to-speech should
declare [`TextToSpeech.Engine.INTENT_ACTION_TTS_SERVICE`](https://developer.android.com/reference/android/speech/tts/TextToSpeech.Engine#INTENT_ACTION_TTS_SERVICE)
in the `queries` elements of their manifest.

```xml
<queries>
  <intent>
    <action android:name="android.intent.action.TTS_SERVICE" />
  </intent>
</queries>
```

### Pausing on Android

Android TTS does not support the pause function natively, so we have implemented a work around. We utilize the native `onRangeStart()` method to determine the index of start when `pause` is invoked. We use that index to create a new text the next time `speak` is invoked.  Due to using `onRangeStart()`, pause works on SDK versions >= 26.  Also, if using `start` and `end` offsets inside of `setProgressHandler()`, you'll need to keep a track of them if using `pause` since they will update once the new text is created when `speak` is called after being paused.

```dart
await flutterTts.pause()
```

## iOS

There's a known issue with integrating plugins that use Swift into a Flutter project created with the Objective-C template. [Flutter#16049](https://github.com/flutter/flutter/issues/16049)

[Example](https://github.com/dlutton/flutter_tts/blob/master/example/lib/main.dart)

To use this plugin :

- add the dependency to your [pubspec.yaml](https://github.com/dlutton/flutter_tts/blob/master/example/pubspec.yaml) file.

```yaml
  dependencies:
    flutter:
      sdk: flutter
    flutter_tts:
```

- instantiate FlutterTts

```dart
FlutterTts flutterTts = FlutterTts();

```

To set shared audio [instance](https://developer.apple.com/documentation/avfoundation/avaudiosession/1616504-sharedinstance) (iOS only):

```dart
await flutterTts.setSharedInstance(true);
```

To set audio [category and options](https://developer.apple.com/documentation/avfoundation/avaudiosession) with optional [mode](https://developer.apple.com/documentation/avfaudio/avaudiosession/mode) (iOS only). The following setup allows background music and in-app audio session to continue simultaneously:

```dart
await flutterTts.setIosAudioCategory(IosTextToSpeechAudioCategory.ambient,
     [
          IosTextToSpeechAudioCategoryOptions.allowBluetooth,
          IosTextToSpeechAudioCategoryOptions.allowBluetoothA2DP,
          IosTextToSpeechAudioCategoryOptions.mixWithOthers
     ],
     IosTextToSpeechAudioMode.voicePrompt
);
```

To await speak completion.

```dart
await flutterTts.awaitSpeakCompletion(true);
```

To await synthesize to file completion.

```dart
await flutterTts.awaitSynthCompletion(true);
```

### speak, stop, getLanguages, setLanguage, setSpeechRate, getVoices, setVoice, setVolume, setPitch, isLanguageAvailable, setSharedInstance

```dart
Future _speak() async{
    var result = await flutterTts.speak("Hello World");
    if (result == 1) setState(() => ttsState = TtsState.playing);
}

Future _stop() async{
    var result = await flutterTts.stop();
    if (result == 1) setState(() => ttsState = TtsState.stopped);
}

List<dynamic> languages = await flutterTts.getLanguages;

await flutterTts.setLanguage("en-US");

await flutterTts.setSpeechRate(1.0);

await flutterTts.setVolume(1.0);

await flutterTts.setPitch(1.0);

await flutterTts.isLanguageAvailable("en-US");

// iOS, Android, Web, Windows and Linux only
//see the "Pausing on Android" section for more info
await flutterTts.pause();

// iOS, macOS, and Android only
// The last parameter is an optional boolean value for isFullPath (defaults to false)
await flutterTts.synthesizeToFile("Hello World", Platform.isAndroid ? "tts.wav" : "tts.caf", false);

// Each voice is a Map containing at least these keys: name, locale
// - Windows (UWP voices) only: gender, identifier
// - iOS, macOS only: quality, gender, identifier
// - Android only: quality, latency, network_required, features 
List<Map> voices = await flutterTts.getVoices;

await flutterTts.setVoice({"name": "Karen", "locale": "en-AU"});
// iOS, macOS only
await flutterTts.setVoice({"identifier": "com.apple.voice.compact.en-AU.Karen"});

// iOS only
await flutterTts.setSharedInstance(true);

// Android only
await flutterTts.speak("Hello World", focus: true);

await flutterTts.setSilence(2);

await flutterTts.getEngines;

await flutterTts.getDefaultVoice;

await flutterTts.isLanguageInstalled("en-AU");

await flutterTts.areLanguagesInstalled(["en-AU", "en-US"]);

await flutterTts.setQueueMode(1);

await flutterTts.getMaxSpeechInputLength;

await flutterTts.setAudioAttributesForNavigation();
```

### Listening for platform calls

```dart
flutterTts.setStartHandler(() {
  setState(() {
    ttsState = TtsState.playing;
  });
});

flutterTts.setCompletionHandler(() {
  setState(() {
    ttsState = TtsState.stopped;
  });
});

flutterTts.setProgressHandler((String text, int startOffset, int endOffset, String word) {
  setState(() {
    _currentWord = word;
  });
});

flutterTts.setErrorHandler((msg) {
  setState(() {
    ttsState = TtsState.stopped;
  });
});

flutterTts.setCancelHandler((msg) {
  setState(() {
    ttsState = TtsState.stopped;
  });
});

// Android, iOS and Web
flutterTts.setPauseHandler((msg) {
  setState(() {
    ttsState = TtsState.paused;
  });
});

flutterTts.setContinueHandler((msg) {
  setState(() {
    ttsState = TtsState.continued;
  });
});

```

## Getting Started

For help getting started with Flutter, view our online
[documentation](https://flutter.dev/).

For help on editing plugin code, view the [documentation](https://flutter.dev/platform-plugins/#edit-code).
