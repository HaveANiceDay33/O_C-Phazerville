// Copyright (c) 2025, Nicholas J. Michalek
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

class MidiLoop : public HemisphereApplet {
public:

    static constexpr int MAX_LOOP_LENGTH = 64;

    enum MidiLoopCursor {
      MIDI_CHAN,
      LENGTH,
      REC_OVERDUB,

      MAX_CURSOR = REC_OVERDUB
    };

    const char* applet_name() {
        return "MidiLoop";
    }
    const uint8_t* applet_icon() { return PhzIcons::midiIn; }

    void Start() {
      Reset();
    }

    void Reset() {
        frame.MIDIState.ClearMonoBuffer();
        frame.MIDIState.ClearPolyBuffer();

        //AllNotesOff();
        SendNotesOff(step);

        step = -1;
        overdub = 0;
    }

    void Controller() {
      if (Clock(1)) {
        //Reset();
        SendNotesOff(step);

        step = -1;
        overdub = 0;
      }

      if (Clock(0)) {
        // - advance step
        ++step %= length;

        if (rec_step > step) rec_step = -1; // end of recording

        if (overdub) {
          rec_step = step;
          StartADCLag(0, ClockCycleTicks(0)/2);
        }

        // not recording
        if (rec_step < 0 || overdub) {
          if (note_timer > 0) {
            // turn old notes off
            for (auto& note : buffer[(step + length-1)%length]) {
              HS::frame.MIDIState.SendNoteOff(midi_ch, note.note);
              // TODO: tied notes
            }
          }
          // turn new notes on
          for (auto& note : buffer[step]) {
            HS::frame.MIDIState.SendNoteOn(midi_ch, note.note, note.vel);
          }
          // use trig length setting for NoteOff
          //note_timer = HS::trig_length * HEMISPHERE_CLOCK_TICKS;

          // use half of clock cycle for timed NoteOff
          note_timer = ClockCycleTicks(0)/2;
        }

        rec_gate = (rec_step == step);

        if (rec_gate && !overdub) {
          // overwrite the step
          buffer[rec_step] = HS::frame.MIDIState.note_buffer[midi_ch];
        }
      }

      // continuously capture new notes while gated or overdubbing
      if (rec_gate && (overdub || Gate(0)) && rec_step >= 0) {
        for (auto& n : HS::frame.MIDIState.note_buffer[midi_ch]) {
          auto it = std::find_if( buffer[rec_step].begin(), buffer[rec_step].end(),
              [&n](const HS::MIDINoteData& b) {
                return b.note == n.note;
              });
          if (it == buffer[rec_step].end())
            buffer[rec_step].push_back(n);
        }
      }

      if (EndOfADCLag(0) && rec_step > -1) {
        // advance overdub record step at 50% of the gap
        if (++rec_step >= length) rec_step = -1;
      }

      // gate goes low - step complete
      if (rec_gate && !(overdub || Gate(0))) {
        if (++rec_step >= length) rec_step = -1;
        rec_gate = 0;
      }

      if (!Gate(0) && --note_timer == 0) {
        SendNotesOff(step);
      }
    }

    void SendNotesOff(int stepidx) {
      if (stepidx < 0) return;
      for (auto& note : buffer[stepidx]) {
        HS::frame.MIDIState.SendNoteOff(midi_ch, note.note);
      }
    }
    void AllNotesOff() {
      for (int i = 0; i < 128; ++i) {
        HS::frame.MIDIState.SendNoteOff(midi_ch, i);
      }
    }

    void View() {
        DrawStuff();
    }

    void AuxButton() {
        rec_step = (rec_step<0)?0:-1;
        step = -1;
        CancelEdit();
    }
    void OnButtonPress() {
      if (cursor == REC_OVERDUB) {
        overdub ^= 1;
        if (!overdub) {
          rec_step = -1;
          rec_gate = 0;
        }
      } else
        CursorToggle();
    }
    void OnEncoderMove(int direction) {
        if (!EditMode()) {
            MoveCursor(cursor, direction, MAX_CURSOR);
            return;
        }

        switch (cursor) {
          case MIDI_CHAN:
            midi_ch = constrain(midi_ch + direction, 0, 15); // TODO: omni
            break;
          case LENGTH:
            length = constrain(length + direction, 1, MAX_LOOP_LENGTH);
            break;
        }

        ResetCursor();
    }

    uint64_t OnDataRequest() {
        uint64_t data = 0;
        return data;
    }

    void OnDataReceive(uint64_t data) {
    }

protected:
    void SetHelp() {
        //                    "-------" <-- Label size guide
        help[HELP_DIGITAL1] = "Clock";
        help[HELP_DIGITAL2] = "Reset";
        //help[HELP_CV1]    = "";
        //help[HELP_CV2]    = "";
        help[HELP_OUT1]     = "";
        help[HELP_OUT2]     = "";
        help[HELP_EXTRA1]   = "MIDI Thru Only";
        help[HELP_EXTRA2]   = "  No CV I/O";
        //                    "---------------------" <-- Extra text size guide
    }

private:
    // Housekeeping
    int cursor;
    uint8_t midi_ch = 0; // 0-indexed
    uint8_t length = 16;
    int step = -1;
    int rec_step = -1;
    int note_timer = 0;
    bool rec_gate = 0;
    bool overdub = 1;

    std::array<NoteBuffer, MAX_LOOP_LENGTH> buffer;

    void DrawStuff() {
      gfxPrint(2, 15, "Ch: ");
      gfxPrint(midi_ch+1);

      gfxIcon(2, 25, PLAY_ICON);
      gfxPrint(10, 25, step + 1);

      if (rec_step > -1) {
        if (step == rec_step) gfxInvert(10, 25, 18, 10);
        gfxIcon(30, 25, RECORD_ICON);
        gfxPrint(40, 25, rec_step + 1);
        if (overdub) gfxInvert(30, 25, 20, 10);
      }

      gfxPrint(2, 35, "Len:");
      gfxPrint(26, 35, length);

      gfxPrint(26, 55, overdub?"Dub":"Rec");

      if (cursor == REC_OVERDUB)
        gfxIcon(18, 55, RIGHT_ICON);
      else
        gfxCursor(26, 23 + cursor*20, 19);
    }
};
