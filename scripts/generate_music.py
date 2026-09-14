#!/usr/bin/env python3
"""
AzamiOS — Procedural Audio Track Generator
File: scripts/generate_music.py

Generates CD-quality (44.1 kHz, 16-bit stereo PCM) WAV music files for AzamiOS.
Uses pure Python standard library (wave, struct, math, random).
Produces rich polyphonic synthwave, ambient, lo-fi, and arcade tracks with:
  - Lead melodies with ADSR envelopes and vibrato
  - Polyphonic chord progressions and arpeggios
  - Driving basslines
  - Dynamic percussion (kick, snare, closed & open hi-hats)
"""

import os
import sys
import wave
import struct
import math
import random

SAMPLE_RATE = 44100

def note_freq(semitones_from_a4):
    """Convert semitone offset from A4 (440Hz) to frequency in Hz."""
    return 440.0 * (2.0 ** (semitones_from_a4 / 12.0))

# Note definitions (semitones from A4: A4 = 0)
C3 = -21; D3 = -19; E3 = -17; F3 = -16; G3 = -14; A3 = -12; B3 = -10
C4 = -9;  D4 = -7;  E4 = -5;  F4 = -4;  G4 = -2;  A4 = 0;   B4 = 2
C5 = 3;   D5 = 5;   E5 = 7;   F5 = 8;   G5 = 10;  A5 = 12;  B5 = 14
C6 = 15;  D6 = 17;  E6 = 19;  REST = None

def clamp(val, low=-32767, high=32767):
    return max(low, min(high, int(val)))

class SynthTrack:
    def __init__(self, duration_sec, bpm=130):
        self.duration_sec = duration_sec
        self.total_samples = int(duration_sec * SAMPLE_RATE)
        self.bpm = bpm
        self.beat_sec = 60.0 / bpm
        self.samples_per_beat = int(self.beat_sec * SAMPLE_RATE)
        self.samples_per_16th = self.samples_per_beat // 4
        self.left = [0.0] * self.total_samples
        self.right = [0.0] * self.total_samples

    def add_tone(self, start_sample, duration_samples, freq, amp, wave_type='saw', pan=0.0):
        if freq is None or freq <= 0:
            return
        end_sample = min(self.total_samples, start_sample + duration_samples)
        phase = 0.0
        phase_inc = freq / SAMPLE_RATE

        # ADSR Envelope
        attack = int(0.015 * SAMPLE_RATE)
        decay = int(0.060 * SAMPLE_RATE)
        sustain_level = 0.70
        release = int(0.040 * SAMPLE_RATE)
        rel_start = duration_samples - release

        left_gain = (1.0 - pan) * 0.5
        right_gain = (1.0 + pan) * 0.5

        for s in range(start_sample, end_sample):
            pos = s - start_sample
            if pos < attack:
                env = pos / max(1, attack)
            elif pos < attack + decay:
                env = 1.0 - (1.0 - sustain_level) * ((pos - attack) / max(1, decay))
            elif pos >= rel_start:
                env = sustain_level * (1.0 - (pos - rel_start) / max(1, release))
            else:
                env = sustain_level

            # Waveform generation
            if wave_type == 'saw':
                raw = 2.0 * (phase - math.floor(phase + 0.5))
            elif wave_type == 'square':
                raw = 0.8 if phase < 0.5 else -0.8
            elif wave_type == 'triangle':
                raw = 2.0 * abs(2.0 * (phase - math.floor(phase + 0.5))) - 1.0
            else: # sine
                raw = math.sin(2.0 * math.pi * phase)

            sample_val = raw * env * amp
            self.left[s] += sample_val * left_gain
            self.right[s] += sample_val * right_gain

            phase = (phase + phase_inc) % 1.0

    def add_kick(self, start_sample):
        dur = int(0.18 * SAMPLE_RATE)
        end = min(self.total_samples, start_sample + dur)
        for s in range(start_sample, end):
            t = (s - start_sample) / SAMPLE_RATE
            freq = 150.0 * math.exp(-t * 30.0) + 40.0
            phase = 2.0 * math.pi * freq * t
            amp = 14000.0 * math.exp(-t * 18.0)
            val = math.sin(phase) * amp
            self.left[s] += val * 0.5
            self.right[s] += val * 0.5

    def add_snare(self, start_sample):
        dur = int(0.16 * SAMPLE_RATE)
        end = min(self.total_samples, start_sample + dur)
        for s in range(start_sample, end):
            t = (s - start_sample) / SAMPLE_RATE
            tone = math.sin(2.0 * math.pi * 180.0 * t) * math.exp(-t * 22.0) * 5000.0
            noise = (random.random() * 2.0 - 1.0) * math.exp(-t * 16.0) * 8000.0
            val = tone + noise
            self.left[s] += val * 0.5
            self.right[s] += val * 0.5

    def add_hihat(self, start_sample, open_hh=False):
        dur = int((0.18 if open_hh else 0.04) * SAMPLE_RATE)
        decay = 12.0 if open_hh else 55.0
        amp = 4500.0 if open_hh else 3500.0
        end = min(self.total_samples, start_sample + dur)
        for s in range(start_sample, end):
            t = (s - start_sample) / SAMPLE_RATE
            val = (random.random() * 2.0 - 1.0) * math.exp(-t * decay) * amp
            self.left[s] += val * 0.6
            self.right[s] += val * 0.4

    def save(self, filepath):
        os.makedirs(os.path.dirname(filepath), exist_ok=True)
        with wave.open(filepath, 'wb') as wf:
            wf.setnchannels(2)
            wf.setsampwidth(2)
            wf.setframerate(SAMPLE_RATE)
            frames = bytearray()
            for i in range(self.total_samples):
                l = clamp(self.left[i])
                r = clamp(self.right[i])
                frames.extend(struct.pack('<hh', l, r))
            wf.writeframes(frames)
        print(f"  ✓  Generated {filepath} ({self.duration_sec:.1f}s, {len(frames)} bytes)")

def make_track_neon_horizon(out_path):
    """Track 1: Cyber Synthwave / Retrowave Drive (132 BPM, 18 seconds)"""
    track = SynthTrack(duration_sec=18.0, bpm=132)
    step_samples = track.samples_per_16th

    # Chord progression: Am -> F -> C -> G (each 16 sixteenth steps)
    progression = [
        (A3, [C4, E4, A4]),
        (F3, [A3, C4, F4]),
        (C3, [G3, C4, E4]),
        (G3, [B3, D4, G4])
    ]

    melody_pattern = [
        A4, C5, E5, A5, G5, E5, D5, E5,
        F4, A4, C5, F5, G4, B4, D5, G5,
        C5, E5, G5, C6, D5, C5, B4, G4,
        A4, E4, A4, B4, C5, B4, A4, E4
    ]

    total_16ths = int(track.total_samples / step_samples)

    # Render drums, bass, chords, melody
    for step in range(total_16ths):
        s_idx = step * step_samples

        # Drums
        beat_in_bar = (step // 4) % 4
        sub_beat = step % 4
        if sub_beat == 0:
            if beat_in_bar in (0, 2):
                track.add_kick(s_idx)
            if beat_in_bar in (1, 3):
                track.add_snare(s_idx)
        if step % 2 == 0:
            track.add_hihat(s_idx, open_hh=(sub_beat == 2))

        # Bassline: 16th note rolling arpeggiated bass
        chord_idx = (step // 16) % 4
        root_note, chord_notes = progression[chord_idx]
        bass_note = root_note if (step % 2 == 0) else root_note + 12
        track.add_tone(s_idx, int(step_samples * 0.9), note_freq(bass_note), 8000, wave_type='saw', pan=0.0)

        # Arpeggiated synth pad
        arp_note = chord_notes[step % len(chord_notes)]
        track.add_tone(s_idx, int(step_samples * 1.5), note_freq(arp_note), 3500, wave_type='triangle', pan=-0.3)

        # Lead melody (plays every 2 steps = 8th notes)
        if step % 2 == 0:
            mel_note = melody_pattern[(step // 2) % len(melody_pattern)]
            if mel_note is not REST:
                track.add_tone(s_idx, int(step_samples * 1.8), note_freq(mel_note), 6500, wave_type='square', pan=0.3)

    track.save(out_path)

def make_track_starlight_odyssey(out_path):
    """Track 2: Cosmic Space Ambient (96 BPM, 20 seconds)"""
    track = SynthTrack(duration_sec=20.0, bpm=96)
    step_samples = track.samples_per_16th
    total_16ths = int(track.total_samples / step_samples)

    melody = [
        E4, G4, B4, E5, D5, B4, A4, B4,
        C4, E4, G4, C5, B4, G4, F4, G4,
        D4, F4, A4, D5, C5, A4, G4, A4,
        B3, D4, F4, B4, A4, F4, E4, F4
    ]

    for step in range(total_16ths):
        s_idx = step * step_samples

        # Smooth ambient kick on beat 1 & 3
        if step % 8 == 0:
            track.add_kick(s_idx)
        if step % 4 == 2:
            track.add_hihat(s_idx, open_hh=True)

        # Ambient pad & chords
        chord_root = [E3, C3, D3, B3][(step // 16) % 4]
        if step % 4 == 0:
            track.add_tone(s_idx, int(step_samples * 3.8), note_freq(chord_root), 5000, wave_type='sine', pan=-0.2)
            track.add_tone(s_idx, int(step_samples * 3.8), note_freq(chord_root + 7), 3500, wave_type='triangle', pan=0.2)

        # Shimmering melody
        if step % 2 == 0:
            mel_note = melody[(step // 2) % len(melody)]
            track.add_tone(s_idx, int(step_samples * 2.2), note_freq(mel_note), 5500, wave_type='sine', pan=0.2)

    track.save(out_path)

def make_track_cyber_city_rain(out_path):
    """Track 3: Lo-Fi Chill Chiptune (110 BPM, 17.5 seconds)"""
    track = SynthTrack(duration_sec=17.5, bpm=110)
    step_samples = track.samples_per_16th
    total_16ths = int(track.total_samples / step_samples)

    melody = [
        C5, D5, E5, G5, A5, G5, E5, D5,
        A4, C5, D5, E5, G5, E5, D5, C5,
        F4, A4, C5, D5, E5, D5, C5, A4,
        G4, B4, D5, E5, D5, B4, A4, G4
    ]

    for step in range(total_16ths):
        s_idx = step * step_samples
        sub = step % 4
        beat = (step // 4) % 4

        if sub == 0:
            if beat in (0, 2):
                track.add_kick(s_idx)
            if beat == 2:
                track.add_snare(s_idx)
        if step % 2 == 1:
            track.add_hihat(s_idx, open_hh=False)

        # Chiptune square bass
        bass_note = [C3, A3, F3, G3][(step // 16) % 4]
        if step % 2 == 0:
            track.add_tone(s_idx, int(step_samples * 0.9), note_freq(bass_note), 6000, wave_type='square', pan=-0.2)

        # Chiptune lead
        mel_note = melody[step % len(melody)]
        track.add_tone(s_idx, int(step_samples * 1.2), note_freq(mel_note), 6000, wave_type='triangle', pan=0.25)

    track.save(out_path)

def make_track_azami_anthem(out_path):
    """Track 4: 16-Bit Arcade Victory Fanfare (144 BPM, 16 seconds)"""
    track = SynthTrack(duration_sec=16.0, bpm=144)
    step_samples = track.samples_per_16th
    total_16ths = int(track.total_samples / step_samples)

    melody = [
        C4, E4, G4, C5, REST, G4, C5, REST,
        E5, D5, C5, D5, E5, G5, E5, C5,
        A4, C5, E5, A5, G5, E5, C5, D5,
        C5, REST, C5, REST, C5, E5, C5, REST
    ]

    for step in range(total_16ths):
        s_idx = step * step_samples

        # Arcade driving drums
        if step % 4 == 0:
            track.add_kick(s_idx)
        if step % 4 == 2:
            track.add_snare(s_idx)
        track.add_hihat(s_idx, open_hh=(step % 4 == 2))

        # Bass octave pumping
        root = [C3, F3, A3, G3][(step // 16) % 4]
        b_note = root if (step % 2 == 0) else root + 12
        track.add_tone(s_idx, int(step_samples * 0.8), note_freq(b_note), 7500, wave_type='saw', pan=0.0)

        # Lead fanfare
        mel = melody[step % len(melody)]
        if mel is not REST:
            track.add_tone(s_idx, int(step_samples * 1.5), note_freq(mel), 7500, wave_type='square', pan=0.1)

    track.save(out_path)

def main():
    dest_dir = sys.argv[1] if len(sys.argv) > 1 else "userland/build/music"
    print(f"Generating AzamiOS CD-Quality WAV Tracks into {dest_dir}...")

    make_track_neon_horizon(os.path.join(dest_dir, "01_Neon_Horizon.wav"))
    make_track_starlight_odyssey(os.path.join(dest_dir, "02_Starlight_Odyssey.wav"))
    make_track_cyber_city_rain(os.path.join(dest_dir, "03_Cyber_City_Rain.wav"))
    make_track_azami_anthem(os.path.join(dest_dir, "04_Azami_Anthem.wav"))

    # Install in-tree MP3 tracks
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)
    src_music = os.path.join(repo_root, "userland", "music")
    if os.path.isdir(src_music):
        for f in os.listdir(src_music):
            if f.lower().endswith(".mp3"):
                src_file = os.path.join(src_music, f)
                dst_file = os.path.join(dest_dir, f)
                import shutil
                shutil.copy2(src_file, dst_file)
                print(f"  ✓  Installed {f} ({os.path.getsize(dst_file)} bytes) [MP3]")

    print("All audio tracks generated and installed successfully.")

if __name__ == "__main__":
    main()
