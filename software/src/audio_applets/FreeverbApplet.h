#include "HSUtils.h"
#include "HemisphereAudioApplet.h"
#include "dsputils.h"
#include "dsputils_arm.h"
#include "synth_waveform.h"
#include "Audio/AudioMixer.h"
#include "Audio/AudioPassthrough.h"
#include "Audio/InterpolatingStream.h"
#include <Audio.h>

template <AudioChannels Channels>
class ReverbApplet : public HemisphereAudioApplet {
    public:
        const char* applet_name() override {
            return "Reverb";
        }
        void Start() override {
            reverb.Acquire();
            for (int i = 0; i < Channels; i++){
                in_conns[i].connect(input, i, complimit[i], 0);
                out_conns[i].connect(complimit[i], 0, output, i);
            }
        }

        void Unload() override {
            AllowRestart();
        }

        void Controller() override {
            float m = constrain(static_cast<float>(mix) * 0.01f + mix_cv.InF(), 0.0f, 1.0f);

            mixer.gain(1, m);
            mixer.gain(0, 1.0f - m);
        }

        void View() override {

        }

        void OnDataRequest(std::array<uint64_t, CONFIG_SIZE>& data) override {
            
        }

        void OnDataReceive(const std::array<uint64_t, CONFIG_SIZE>& data) override {

        }

        void OnButtonPress() override {

        }
        
        void OnEncoderMove(int direction) override {
            if (!EditMode()) {
                MoveCursor(cursor, direction, MIX_CV);
                return;
            }

            if (EditSelectedInputMap(direction)) return;

            switch (cursor) {
                case MIX:
                    mix = constrain(mix + direction, 0, 100);
                    break;
                case MIX_CV:
                    mix_cv.ChangeSource(direction);
                    break;
                default:
                    break;

            }
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
        enum Cursor: int8_t {
            MIX,
            MIX_CV
        };

        int8_t cursor = MIX;

        AudioPassthrough<Channels> input;
        std::array<AudioConnection, Channels> in_conns;
        AudioEffectFreeverbStereo<Channels> reverb;
        std::array<AudioConnection, Channels> out_conns;
        AudioPassthrough<Channels> output;

        int8_t mix = 100;

        CVInputMap mix_cv;
};