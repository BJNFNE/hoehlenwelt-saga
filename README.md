# hwsaga – Die Höhlenwelt-Saga für Linux, macOS und Windows

`hwsaga` lässt „Die Höhlenwelt-Saga – Das leuchtende Kristall“ (Software 2000, 1994) als
natives Programm laufen. Spieldaten sind **nicht** enthalten. Du brauchst die Original-CD bzw.
ein Abbild davon.

Technisch ist `hwsaga` eine eigene Laufzeitumgebung: Sie führt die Spiellogik aus der
originalen `HW.EXE` mit einem eingebauten 16-Bit-x86-Interpreter aus. Alles, was das Spiel von
DOS und der PC-Hardware braucht, bildet `hwsaga` selbst nach, ohne DOSBox und ohne
DOS-Installation:

* DOS-Extender (DPMI/RTM)
* Dateizugriffe
* VESA-Grafik
* Timer, Tastatur und Maus
* Sound Blaster
* CD-ROM mit CD-Audio

## Was du brauchst

Am besten ein Abbild der Spiel-CD als **`HW.CUE` + `HW.BIN`**. Nur damit bekommst du Musik,
Intro-Ton und Erzähler. Die Abbild-Dateien können auch anders heißen, es zählt die `.cue`-Datei.

Alternativ geht ein Ordner mit den Dateien der CD (`HW.EXE`, `HW.001` … `HW.015`, `SAVE.001`).
Liegen die CD-Audiospuren als `track02.wav` … `track28.wav` (oder `trackNN.cdr` von `bchunk`)
im selben Ordner, gibt es auch Musik und Sprache.

## Starten

* **Linux:** `./hwsaga /pfad/zu/HW.CUE`
* **Windows:** `HW.CUE` auf `hwsaga.exe` ziehen, oder `hwsaga.exe C:\Spiele\Hoehlenwelt\HW.CUE`
* **macOS:** `Hoehlenwelt-Saga.app` starten. Wenn das Spiel nicht gefunden wird, `HW.CUE` ins
  Fenster ziehen.

Ohne Angabe sucht das Programm an diesen Stellen:

1. das zuletzt benutzte Spiel,
2. der Ordner des Programms und dessen Unterordner `HW`,
3. bei macOS der Ordner, in dem die `.app` liegt,
4. das aktuelle Verzeichnis.

Findet es nichts, kannst du das CD-Abbild oder den Ordner mit den Spieldateien einfach in das
Fenster ziehen.

Beim ersten Start mit einem CD-Abbild werden die Spieldateien (ca. 63 MB) in das
Benutzerverzeichnis entpackt. Die Musik wird direkt aus dem Abbild gelesen.

### Optionen

| Option | Wirkung |
|---|---|
| `--fullscreen` | Vollbild (umschalten mit Alt+Enter) |
| `--scale N` | Fenstergröße N × 640×480 (Standard 2) |
| `--smooth` | weiche statt pixelgenauer Skalierung |
| `--no-music` | Musik aus |

## Bedienung (wie im Original)

| Eingabe | Wirkung |
|---|---|
| Linksklick | Hingehen bzw. Standardaktion |
| Rechtsklick auf ein Objekt | Verb-Menü; dann mit Linksklick ein Verb wählen |
| Maus an den oberen Rand | Inventarleiste, Karte, Hauptmenü |
| F5 | Spielmenü: Laden, Speichern, Lautstärke, Textgeschwindigkeit, Beenden |
| F2 | Schnellspeichern |
| F9 | Weltkarte (sobald im Spiel freigeschaltet) |
| Esc / Klick | Intro und Zwischensequenzen überspringen |
| Alt+Enter | Vollbild ein/aus |

### Neuen Spielstand anlegen

1. F5 drücken.
2. Das Symbol „Diskette mit Pfeil nach links“ anklicken.
3. Die leere rote Zeile anklicken.
4. Namen tippen und mit Enter bestätigen.
5. „ok“ anklicken.

## Wo liegen Spielstände und Einstellungen?

| System | Pfad |
|---|---|
| Linux | `~/.local/share/hwsaga/hoehlenwelt/save/` |
| Windows | `%APPDATA%\hwsaga\hoehlenwelt\save\` |
| macOS | `~/Library/Application Support/hwsaga/hoehlenwelt/save/` |

Spielstände sind die originalen Dateien `SAVE.NNN` und mit der DOS-Version austauschbar.
`HW.CFG` enthält Lautstärken und Textgeschwindigkeit. `HW.WSA` erzeugt das Programm selbst.
Die beiden Dateien legt im Original `INSTALL.EXE` an, das hier nicht gebraucht wird.

## Selbst bauen

Du brauchst CMake (ab 3.16), einen C++17-Compiler und SDL2.

**Linux**, mit dem SDL2 des Systems:

    sudo apt install cmake g++ libsdl2-dev     # bzw. dnf install cmake gcc-c++ SDL2-devel
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

**Windows** (Visual Studio 2022) und **macOS** (Xcode Command Line Tools): Mit
`-DHWSAGA_BUNDLE_SDL=ON` lädt CMake SDL 2.30.9 herunter und linkt es statisch. Das Ergebnis ist
eine einzelne `hwsaga.exe` bzw. eine `.app` ohne weitere Abhängigkeiten.

    cmake -S . -B build -DHWSAGA_BUNDLE_SDL=ON
    cmake --build build --config Release

Für eine Universal-App auf macOS zusätzlich `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"`
angeben.

`.github/workflows/build.yml` baut alle drei Plattformen in GitHub Actions und legt fertige
Pakete als Artefakte ab.

## Aufbau des Quelltexts

| Datei | Inhalt |
|---|---|
| `src/cpu.*` | x86-Interpreter (286-Befehlssatz plus die 386-Erweiterungen, die Borland Pascal 7 nutzt) |
| `src/loader.cpp` | Lader für das NE-Format (16-Bit Protected Mode): Segmente, Selektoren, Relokationen |
| `src/machine.*` | Speicher, Deskriptortabelle, Interrupts, PIC, Zeitbasis, Maus-Callback, Hauptschleife |
| `src/hle.cpp` | Ersatz für die KERNEL/RTM-Importe, DOS (INT 21h), DPMI (INT 31h), VESA, Maus, MSCDEX |
| `src/io.cpp` | Ports: PIT, PIC, Tastaturcontroller, VGA-DAC, DMA, Sound-Blaster-DSP, MPU-401 |
| `src/cdaudio.*` | CUE/BIN-Abbilder, ISO-9660-Entpacker, CD-Audio-Wiedergabe |
| `src/vfs.*` | Zuordnung der DOS-Dateinamen zu Spieldaten- und Spielstandverzeichnis |
| `src/host.*` | SDL2: Fenster, Skalierung, Eingabe, Audio-Mischer |
| `src/midi.*` | MIDI-Bytestrom des MPU-401 (Anschluss für einen Software-Synthesizer) |

## Bekannte Einschränkungen

* Musik gibt es nur mit den CD-Audiospuren. Das Spiel nutzt für alle Stücke CD-Audio, sobald
  eine CD mit Audiospuren da ist. MIDI- und AdLib-Ausgabe sind noch nicht an einen Synthesizer
  angeschlossen.
* Die Sound-Blaster-Effekte werden vom Spiel per DMA geschickt und mit 44,1 kHz ausgegeben
  (einfache Umrechnung der Abtastrate, wie bei einer echten Karte ohne Filter).

## Hilfen zur Fehlersuche

Diese Umgebungsvariablen sind für automatische Tests gedacht:

| Variable | Wirkung |
|---|---|
| `HWSAGA_SHOTS=DIR` | Verzeichnis für Bildschirmfotos (BMP) |
| `HWSAGA_SHOT_MS=N` | alle N ms ein Bildschirmfoto |
| `HWSAGA_SCRIPT=FILE` | Eingabeskript, Zeilen wie `<ms> click X Y [0/1]`, `<ms> move X Y`, `<ms> key SCANCODE`, `<ms> shot NAME`, `<ms> poke SEG OFF WERT`, `<ms> quit` |
| `HWSAGA_BREAK=21:0D77,…` | Prozessorzustand an diesen Stellen ausgeben (NE-Segment:Offset) |
| `HWSAGA_PROFILE=1` | am Ende die meistbenutzten Codestellen ausgeben |
| `--trace` | Dateizugriffe, CD- und Sound-Blaster-Befehle protokollieren |

Mit `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy` läuft das Programm ohne Fenster.
