/*
 * Radio Music applet
 *    emulation of Music Thing Modular's Radio Music module
 *    based on WAVPlayerApplet / OneShotPlayerApplet by djphazer & Tom Whiston,
 *    using the TeensyVariablePlayback library by Nic Newdigate
 *
 * SD card structure -- numbered folders in the root, as the original module
 * expects. Folder numbers need not be contiguous:
 *   /0/ARCHERS.raw
 *   /1/BECKETT.raw
 *   /15/TONE1.raw
 *   ...
 *
 * Files are headerless 16-bit signed mono PCM at 44.1kHz (.raw), which is
 * what the official Radio Music sample collection ships. 16-bit .wav files
 * are also accepted and are detected by extension.
 *
 * The defining behaviour of Radio Music is that every station shares one
 * free-running playhead: stations keep "broadcasting" whether or not you
 * are listening, so changing station drops you in partway through rather
 * than starting the new file from its beginning. That is what separates
 * this from an ordinary sample player.
 *
 * Controls (cursor-based, one encoder + AuxButton):
 *   Bank     -- which numbered folder, wraps around
 *   Station  -- which file in that folder, CV assignable
 *   Start    -- offset as a percentage of file length, CV assignable
 *   Reset    -- trigger input; jumps to the start offset
 *   Level    -- output gain in dB, CV assignable
 *   AuxButton (X or Y) -- reset to start, by hand
 *
 * Station changes and resets are declicked with a short gain fade, and all
 * SD access happens in mainloop() rather than the audio ISR.
 *
 */

#include <TeensyVariablePlayback.h>

class RadioMusicApplet : public HemisphereAudioApplet {
public:
  const char* applet_name() {
    return "RadioMus";
  }

  void Start() {
    PatchCable(input, 0, mixer, 1);
    mixer.gain(1, 1.0f);
    PatchCable(wavplayer, 0, mixer, 0);
    mixer.gain(0, 0.0f);
    PatchCable(mixer, 0, output, 0);

    if (!SDcard_Ready) {
      Serial.println("Radio Music: unable to access the SD card");
      return;
    }
    wavplayer.enableInterpolation(true);
    wavplayer.setBufferInPSRAM(false);

    scan_banks = true;
  }

  void Unload() {
    wavplayer.stop();
    AllowRestart();
  }

  void Reset() override {
    reset_cv.Reset();
  }

  // --- runs in the audio ISR: no SD access here, only flags -------------
  void Controller() {
    // CV-modulated station and start offset
    if (station_count > 0) {
      station_mod = constrain(
        station + station_cv.InRescaled(station_count), 0, station_count - 1
      );
    } else {
      station_mod = 0;
    }
    start_mod = constrain(start_pct + start_cv.InRescaled(101), 0, 100);

    if (reset_cv.Clock()) want_reset = true;

    // a station change or a reset both fade out first
    bool want_load = (station_mod != loaded_station) || reload_station;
    if (want_load || want_reset) fade_target = 0.0f;
    else if (station_ready) fade_target = 1.0f;

    if (fade < fade_target) fade = fminf(fade + FADE_STEP, fade_target);
    else if (fade > fade_target) fade = fmaxf(fade - FADE_STEP, fade_target);

    float gain = dbToScalar(level) + level_cv.InF(0.0f);
    if (gain < 0.0f) gain = 0.0f;
    mixer.gain(0, fade * gain);
  }

  // --- runs in the main loop: SD access lives here ----------------------
  void mainloop() {
    if (!SDcard_Ready) return;

    if (scan_banks) {
      ScanBanks();
      scan_banks = false;
      scan_stations = true;
    }
    if (scan_stations) {
      ScanStations();
      scan_stations = false;
      reload_station = true;
      if (station >= station_count) station = station_count ? station_count - 1 : 0;
    }

    // advance the free-running playhead in real time, whether or not the
    // current station is audible -- this is what makes it a radio
    uint32_t now = millis();
    if (station_ready) playhead_ms += now - last_millis;
    last_millis = now;

    // wait for the declick fade before touching the card
    if (fade > 0.0f) return;

    if (reload_station || station_mod != loaded_station) {
      LoadStation(station_mod);
      reload_station = false;
      want_reset = false;
      return;
    }
    if (want_reset) {
      playhead_ms = 0;
      SeekToPlayhead();
      want_reset = false;
    }
  }

  void View() {
    if (!SDcard_Ready) {
      gfxPrint(4, 25, "NO SD CARD!");
      return;
    }
    if (bank_count == 0) {
      gfxPrint(1, 25, "NO BANKS");
      gfxPrint(1, 35, "on SD root");
      return;
    }

    int y = 13;
    gfxIcon(1, y, station_ready ? PLAY_ICON : STOP_ICON);

    gfxStartCursor(12, y);
    graphics.printf("B%02u", BankNumber());
    gfxEndCursor(cursor == BANK, "Bank");

    gfxStartCursor(38, y);
    if (station_count > 0) graphics.printf("%02u", station_mod + 1);
    else gfxPrint("--");
    gfxEndCursor(cursor == STATION, "Station");
    if (station_mod != station) gfxIcon(54, y, CV_ICON);

    // station name, extension stripped
    y += 10;
    if (station_count > 0) gfxPrint(1, y, display_name);
    else gfxPrint(1, y, "(empty)");

    // elapsed position of the shared playhead
    y += 10;
    if (station_ready) {
      uint32_t tsec = wavplayer.positionMillis() / 1000;
      gfxPos(1, y);
      graphics.printf("%02lu:%02lu", tsec / 60, tsec % 60);
    }

    y += 10;
    if (cursor < LEVEL) {
      gfxIcon(1, y, PULSES_ICON);
      gfxStartCursor(11, y);
      graphics.printf(
        "%3u%%", (cursor == START && EditMode()) ? start_pct : start_mod
      );
      gfxEndCursor(cursor == START, "Start");
      if (start_mod != start_pct) gfxIcon(40, y, CV_ICON);
      gfxStartCursor(48, y);
      gfxPrint(start_cv);
      gfxEndCursor(cursor == START_CV, false, start_cv.InputName(), "Start CV");
    } else {
      gfxStartCursor(1, y);
      gfxPrintDb(level);
      gfxEndCursor(cursor == LEVEL, "Gain");
      gfxStartCursor(48, y);
      gfxPrint(level_cv);
      gfxEndCursor(cursor == LEVEL_CV, false, level_cv.InputName(), "Gain CV");
    }

    y += 10;
    gfxIcon(1, y, WAVEFORM_ICON);
    gfxStartCursor(11, y);
    gfxPrint(station_cv);
    gfxEndCursor(
      cursor == STATION_CV, false, station_cv.InputName(), "Station CV"
    );
    gfxIcon(34, y, CLOCK_ICON);
    gfxStartCursor(44, y);
    gfxPrint(reset_cv);
    gfxEndCursor(cursor == RESET_TRIG, false, reset_cv.InputName(), "Reset");

    gfxDisplayInputMapEditor();
  }

  void AuxButton() {
    want_reset = true;
  }

  void OnButtonPress() {
    if (CheckEditInputMapPress(
          cursor,
          IndexedInput(STATION_CV, station_cv),
          IndexedInput(START_CV, start_cv),
          IndexedInput(RESET_TRIG, reset_cv),
          IndexedInput(LEVEL_CV, level_cv)
        ))
      return;
    CursorToggle();
  }

  void OnEncoderMove(int direction) {
    if (!EditMode()) {
      MoveCursor(cursor, direction, NUM_PARAMS - 1);
      return;
    }
    if (EditSelectedInputMap(direction)) return;
    switch (cursor) {
      case BANK:
        if (bank_count > 0) {
          // wrap around at both ends, as asked
          bank_idx = (bank_idx + direction + bank_count) % bank_count;
          station = 0;
          scan_stations = true;
        }
        break;
      case STATION:
        if (station_count > 0)
          station = constrain(station + direction, 0, station_count - 1);
        break;
      case STATION_CV:
        station_cv.ChangeSource(direction);
        break;
      case START:
        start_pct = constrain(start_pct + direction, 0, 100);
        break;
      case START_CV:
        start_cv.ChangeSource(direction);
        break;
      case RESET_TRIG:
        reset_cv.ChangeSource(direction);
        break;
      case LEVEL:
        level = constrain(level + direction, LVL_MIN_DB, LVL_MAX_DB);
        break;
      case LEVEL_CV:
        level_cv.ChangeSource(direction);
        break;
    }
  }

  void OnDataRequest(std::array<uint64_t, CONFIG_SIZE>& data) override {
    // stop playback so a preset save can't collide with SD streaming
    wavplayer.stop();
    station_ready = false;
    uint8_t bank_num = BankNumber();
    uint8_t station_u8 = (uint8_t)constrain(station, 0, 255);
    uint8_t start_u8 = (uint8_t)start_pct;
    data[0] = PackPackables(bank_num, station_u8, start_u8, level);
    data[1] = PackPackables(station_cv, start_cv, level_cv);
    data[2] = PackPackables(reset_cv);
    data[3] = 0;
    reload_station = true;
  }

  void OnDataReceive(const std::array<uint64_t, CONFIG_SIZE>& data) override {
    uint8_t bank_num = 0;
    uint8_t station_u8 = 0;
    uint8_t start_u8 = 0;
    UnpackPackables(data[0], bank_num, station_u8, start_u8, level);
    UnpackPackables(data[1], station_cv, start_cv, level_cv);
    UnpackPackables(data[2], reset_cv);
    saved_bank_number = bank_num;
    station = station_u8;
    start_pct = constrain((int)start_u8, 0, 100);
    scan_banks = true;
  }

  AudioStream* InputStream() override {
    return &input;
  }
  AudioStream* OutputStream() override {
    return &output;
  }

protected:
  void SetHelp() override {}

private:
  enum RadioCursor {
    BANK,
    STATION,
    START,
    START_CV,
    STATION_CV,
    RESET_TRIG,
    LEVEL,
    LEVEL_CV,

    NUM_PARAMS
  };

  static constexpr int MAX_BANKS = 16;
  static constexpr int MAX_STATIONS = 32;
  static constexpr int NAME_LEN = 16;

  // headerless .raw files carry no sample rate; the Radio Music collection
  // is 44.1kHz. Change this if you build a card at another rate.
  static constexpr float FILE_SAMPLE_RATE = 44100.0f;

  // ~5ms declick fade, in Controller ticks
  static constexpr float FADE_STEP = 1.0f / (5.0f * HEMISPHERE_CLOCK_TICKS);

  int cursor = 0;
  int8_t level = 0; // dB

  CVInputMap station_cv;
  CVInputMap start_cv;
  CVInputMap level_cv;
  DigitalInputMap reset_cv;

  AudioPassthrough<MONO> input;
  AudioPlaySdResmp wavplayer;
  AudioMixer4 mixer;
  AudioPassthrough<MONO> output;

  // bank/station tables, built by scanning the card
  uint8_t bank_numbers[MAX_BANKS];
  uint8_t bank_count = 0;
  uint8_t bank_idx = 0;
  uint8_t saved_bank_number = 0;

  char station_names[MAX_STATIONS][NAME_LEN];
  uint32_t station_samples[MAX_STATIONS];
  int16_t station_count = 0;

  int16_t station = 0;     // knob value
  int16_t station_mod = 0; // after CV
  int16_t loaded_station = -1;
  char display_name[NAME_LEN] = "";

  int16_t start_pct = 0;
  int16_t start_mod = 0;

  // the shared, free-running playhead -- the heart of the emulation
  uint32_t playhead_ms = 0;
  uint32_t last_millis = 0;

  // ISR -> mainloop flags
  volatile bool scan_banks = false;
  volatile bool scan_stations = false;
  volatile bool reload_station = false;
  volatile bool want_reset = false;
  volatile bool station_ready = false;

  float fade = 0.0f;
  float fade_target = 0.0f;

  uint8_t BankNumber() const {
    return bank_count ? bank_numbers[bank_idx] : 0;
  }

  static bool AllDigits(const char* s) {
    if (!s || !*s) return false;
    for (const char* p = s; *p; ++p)
      if (*p < '0' || *p > '9') return false;
    return true;
  }

  static bool HasExt(const char* name, const char* ext) {
    size_t len = strlen(name);
    size_t elen = strlen(ext);
    return len > elen && strcasecmp(name + len - elen, ext) == 0;
  }

  // find numbered folders in the root, numerically sorted
  void ScanBanks() {
    bank_count = 0;
    File root = SD.open("/");
    if (!root) return;

    while (bank_count < MAX_BANKS) {
      File entry = root.openNextFile();
      if (!entry) break;
      if (entry.isDirectory() && AllDigits(entry.name())) {
        int n = atoi(entry.name());
        if (n >= 0 && n <= 255) {
          // insertion sort, so /2/ lands before /10/
          int i = bank_count;
          while (i > 0 && bank_numbers[i - 1] > n) {
            bank_numbers[i] = bank_numbers[i - 1];
            --i;
          }
          bank_numbers[i] = (uint8_t)n;
          bank_count++;
        }
      }
      entry.close();
    }
    root.close();

    // restore a saved bank by folder number, not by index
    bank_idx = 0;
    for (uint8_t i = 0; i < bank_count; ++i) {
      if (bank_numbers[i] == saved_bank_number) {
        bank_idx = i;
        break;
      }
    }
  }

  // list the playable files in the current bank, alphabetically
  void ScanStations() {
    station_count = 0;
    loaded_station = -1;
    station_ready = false;
    display_name[0] = '\0';
    if (bank_count == 0) return;

    char path[12];
    snprintf(path, sizeof(path), "/%u", (unsigned)BankNumber());
    File dir = SD.open(path);
    if (!dir) return;

    while (station_count < MAX_STATIONS) {
      File entry = dir.openNextFile();
      if (!entry) break;
      const char* name = entry.name();
      if (!entry.isDirectory() && name[0] != '.'
          && (HasExt(name, ".raw") || HasExt(name, ".wav"))) {
        uint32_t bytes = entry.size();
        int i = station_count;
        while (i > 0 && strcasecmp(station_names[i - 1], name) > 0) {
          memcpy(station_names[i], station_names[i - 1], NAME_LEN);
          station_samples[i] = station_samples[i - 1];
          --i;
        }
        strncpy(station_names[i], name, NAME_LEN - 1);
        station_names[i][NAME_LEN - 1] = '\0';
        // 16-bit mono; .wav is close enough for the position readout
        station_samples[i] = bytes / 2;
        station_count++;
      }
      entry.close();
    }
    dir.close();
  }

  uint32_t StationLengthMs(int idx) const {
    if (idx < 0 || idx >= station_count) return 0;
    return (uint32_t)((uint64_t)station_samples[idx] * 1000ull
                      / (uint64_t)FILE_SAMPLE_RATE);
  }

  // where the shared playhead puts us in the current file
  uint32_t PlayheadSample(int idx) const {
    uint32_t len_ms = StationLengthMs(idx);
    if (len_ms == 0) return 0;
    uint32_t offset_ms = (uint32_t)((uint64_t)start_mod * len_ms / 100ull);
    uint32_t pos_ms = (offset_ms + playhead_ms) % len_ms;
    return (uint32_t)((uint64_t)pos_ms * (uint64_t)FILE_SAMPLE_RATE / 1000ull);
  }

  void LoadStation(int idx) {
    station_ready = false;
    if (idx < 0 || idx >= station_count) {
      wavplayer.stop();
      return;
    }

    char path[NAME_LEN + 12];
    snprintf(path, sizeof(path), "/%u/%s", (unsigned)BankNumber(), station_names[idx]);

    bool ok = HasExt(station_names[idx], ".wav")
      ? wavplayer.playWav(path)
      : wavplayer.playRaw(path, 1);
    if (!ok) {
      loaded_station = idx; // don't retry every pass
      return;
    }

    // .raw carries no rate, so correct for the build's audio rate by hand
    wavplayer.setPlaybackRate(FILE_SAMPLE_RATE / AUDIO_SAMPLE_RATE_EXACT);
    wavplayer.setLoopType(looptype_repeat);
    wavplayer.setLoopStart(0);
    wavplayer.setLoopFinish(station_samples[idx]);
    wavplayer.setPlayStart(play_start_arbitrary, PlayheadSample(idx));
    wavplayer.play();

    loaded_station = idx;
    station_ready = true;

    // trim the extension for the display
    strncpy(display_name, station_names[idx], NAME_LEN - 1);
    display_name[NAME_LEN - 1] = '\0';
    char* dot = strrchr(display_name, '.');
    if (dot) *dot = '\0';
  }

  void SeekToPlayhead() {
    if (!station_ready) return;
    wavplayer.setPlayStart(play_start_arbitrary, PlayheadSample(loaded_station));
    wavplayer.play();
  }
};
