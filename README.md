# Stem Tape

I want to investigate and prototype an open-source project called Stem Tape for the unreleased Teenage Engineering × Kanye West SP‑1 Stem Player.

Before building a polished product, analyze the concept, identify what is technically feasible, and create an internal R&D web application that helps plan, simulate and eventually manage the firmware.

Product concept

Stem Tape should combine:

Every useful capability of the stock SP‑1 firmware

Every capability of the open-source SP‑1 Tape Looper firmware

Faster and easier loading of prepared stems and samples

Simultaneous stem playback and tape manipulation

There should not be separate “Stem” and “Tape” modes.

Every loaded stem should always function as an independent tape track.

A song contains four prepared stems:

Track 1: vocals

Track 2: drums

Track 3: bass

Track 4: instruments

The four stems begin synchronized like a normal song, but users can manipulate them together or individually without stopping playback.

Desired capabilities

Stem Tape should preserve or recreate the stock SP‑1’s:

Four-stem synchronized playback

Four-fader stem mixing

Mute and solo

Stock effects and Advanced Mode behavior

Tempo-based effects

Locked loops

Song navigation

MIDI clock output

Pocket Operator synchronization

Ability to act as the master clock for devices such as the EP‑133 K.O. II and Roland S‑1

It should also include everything from the current Tape Looper firmware, including:

Four independent tracks

USB audio recording

Hands-free recording

Multiple saved songs

Persistent performance states

Independent loop lengths

Fixed or variable loop behavior

Adjustable loop start and end

Loop chopping

Reverse

Tape-style varispeed and pitch

Multiple playback heads

Free or grid-based recording

MIDI clock

WAV/MP3 import and WAV export

Direct recording into individual tracks

An action performed normally should affect all linked stems. Holding a Track button while performing the action should affect only that selected stem.

Example:

All four stems begin as one synchronized four-bar loop

Drums are shortened to one beat

Bass is changed to a three-beat loop

Vocals are reversed

Instruments remain a four-bar loop

All tracks continue playing simultaneously

The user performs the arrangement using the four faders

Faster loading concept

The current stock upload process is extremely slow. Investigate an alternative importer that accepts four prepared stem files and streams or records them into the SP‑1 simultaneously.

The target workflow is:

Connect the SP‑1 to a computer.

Choose four prepared stem files.

Label vocals, drums, bass and instruments.

Align their starting points.

Resample them into a hardware-compatible format.

Stream all four channels to the SP‑1 simultaneously.

Record each incoming channel directly to its corresponding tape track.

Save the result as a new Stem Tape song.

The ideal target is for a five-minute four-stem song to load in approximately five minutes instead of taking close to an hour.

Investigate four mono channels at 48 kHz as the reliable MVP. Treat four stereo channels as a later experiment because of USB bandwidth, RAM, CPU and eMMC write constraints.

Important technical distinction

Lovable is not expected to write or flash embedded SP‑1 firmware by itself.

Clearly separate:

What belongs in the SP‑1 firmware

What belongs in the companion web or desktop application

What requires Claude Code or an embedded C/Zephyr developer

What requires physical testing on an SP‑1

What can be prototyped or simulated inside this Lovable project

Do not falsely represent simulated functionality as working hardware integration.

Existing open-source resources

Study and use these as the technical starting points:

SP‑1 Tape Looper:
https://github.com/chattock/sp1-tape-looper

SP‑1 developer documentation:
https://github.com/timknapen/SP-1-dev

SP‑1 audio format:
https://github.com/timknapen/SP-1-dev/wiki/Audio-format

SP‑1 data structure:
https://github.com/timknapen/SP-1-dev/wiki/Data-Structure

SP‑1 album metadata:
https://github.com/timknapen/SP-1-dev/wiki/Album-metadata-format

Solderless SP‑1 tools:
https://solderless.engineering/

Do not copy or redistribute Teenage Engineering’s proprietary stock firmware. Stock functionality should be independently reimplemented using documented behavior, community research and original open-source code.

Preserve all licenses and attribution from any open-source repository used.

First deliverable: feasibility investigation

Before proposing a polished interface, produce an in-app technical assessment containing:

1. Capability matrix

Compare:

Stock SP‑1

Tape Looper 2.0

Proposed Stem Tape

What is documented

What remains unknown

What requires physical verification

2. Architecture

Diagram the complete system:

Companion importer

USB audio or data transport

Firmware audio engine

Track mixer

eMMC storage

Global transport

Per-track playback state

MIDI/PO synchronization

Headphone and speaker output

3. Firmware plan

Identify the likely Tape Looper files and subsystems that would need modification, including:

USB audio input

Audio callback and ring buffers

Four-track simultaneous recording

eMMC write scheduling

Song index and metadata

Global versus per-track transport

Track linking

Loop windows

Reverse and varispeed

MIDI clock

Control gestures

LED feedback

Recovery and storage migration

4. Critical technical experiments

Define the smallest hardware tests needed before full development:

Receive four mono USB audio channels

Write all four channels simultaneously

Record five minutes without dropouts

Maintain sample-accurate synchronization

Power-cycle and restore the song

Manipulate one track while the other three continue

Preserve MIDI clock

Recover after an interrupted transfer

Verify performance on a second SP‑1

5. Risk register

Include:

USB Full Speed bandwidth

Browser multichannel-audio limitations

Native app versus browser uploader

nRF52840 RAM and CPU

eMMC sustained and simultaneous write performance

Audio glitches and buffer underruns

Storage corruption

Stereo versus mono

Firmware recovery

Hardware revisions

Stock-feature documentation gaps

Licensing

6. Development roadmap

Organize development into:

Research

Four-channel proof of concept

Synchronized four-track playback

Track-linking system

Fast importer

Stock feature recreation

Tape Looper integration

Hardware testing

Alpha

Community beta

Open-source release

R&D application to build

After presenting the investigation, create an internal Stem Tape development dashboard with:

Project overview

Capability matrix

Architecture diagram

Firmware roadmap

Experiment tracker

Risk register

Hardware test results

Build history

Device inventory

Community feedback log

Open technical questions

Links to source repositories and documentation

Also create a simulated four-track Stem Tape interface:

Four vertical faders

Vocals, drums, bass and instruments

Play/pause

Global and individual track selection

Mute and solo

Shared loop window

Independent loop windows

Reverse per track

Varispeed

Linked/unlinked track controls

Multiple-head visualization

Clear indication that this is currently a browser simulation

The simulator should help validate the interaction design before implementing it in firmware.

Companion importer prototype

Create a non-destructive front-end prototype that allows a user to:

Select four local audio files

Assign stem labels

Enter song and artist metadata

Display duration, sample rate and channel count

Preview all four stems in synchronization

Mute, solo and adjust levels

Detect unequal lengths

Pad shorter tracks with silence

Preview mono downmixing

Estimate storage size

Estimate transfer duration

Simulate transfer progress

Save the project configuration locally

Do not claim that hardware transfer is working until an actual protocol has been implemented and tested.

Design the application in a restrained Teenage Engineering-inspired visual language without copying protected logos, product graphics or branding. Use a clean, technical, tactile interface with strong typography, neutral colors and clear status indicators.

The immediate objective is not marketing. It is to determine whether Stem Tape can be built, identify the first firmware experiment and produce a useful interface specification that can be handed to Claude Code for implementation.






## Development

Prefer working locally? You need Node.js and npm — [install with nvm](https://github.com/nvm-sh/nvm#installing-and-updating).

```sh
git clone <this-repository-url>
cd <repository-name>
npm i
npm run dev
```
