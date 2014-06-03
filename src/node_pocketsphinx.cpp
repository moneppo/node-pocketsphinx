#include <v8.h> 
#include <node.h>
#include <node_buffer.h>
#include <string>
#include <pocketsphinx.h>
#include <sphinxbase/err.h>
#include <sphinxbase/cont_ad.h>
#include "samplerate.h"

using namespace v8;


enum STATE {
  CALIBRATING,
  RESETTING,
  WAITING,
  LISTENING,
  PROCESSING,
  FAILED
};

class PocketSphinx : node::ObjectWrap {
  private:
    cont_ad_t* m_cont;
    ps_decoder_t* m_ps;
    int32 m_ts;

    int m_state;

  public:
    PocketSphinx() {
      printf("Initializing...");
      fflush(stdout); 
      m_cont = cont_ad_init(NULL, NULL);
      m_state = CALIBRATING;
    }

    ~PocketSphinx() {
      cont_ad_close(m_cont);
      ps_free(m_ps);
    }

    static v8::Persistent<FunctionTemplate> persistent_function_template;

    static void Init(Handle<Object> target) {
      v8::HandleScope scope;
   
      // Bind "New" function
      v8::Local<FunctionTemplate> local_function_template = v8::FunctionTemplate::New(New);
      PocketSphinx::persistent_function_template = v8::Persistent<FunctionTemplate>::New(local_function_template);
      PocketSphinx::persistent_function_template->InstanceTemplate()->SetInternalFieldCount(1); 
      PocketSphinx::persistent_function_template->SetClassName(v8::String::NewSymbol("PocketSphinx"));

      NODE_SET_PROTOTYPE_METHOD(PocketSphinx::persistent_function_template, "process", Process);
      
      target->Set(String::NewSymbol("pocketSphinxBinding"), PocketSphinx::persistent_function_template->GetFunction());
    }

    static Handle<Value> New(const Arguments& args) {
      v8::HandleScope scope;
      Local<Object> options = args[0]->ToObject();

      //TODO: Enable passing of config options for most params. Start with the three below.
      v8::String::AsciiValue hmmValue(options->Get(String::NewSymbol("hmm")));
      v8::String::AsciiValue lmValue(options->Get(String::NewSymbol("lm")));
      v8::String::AsciiValue dictValue(options->Get(String::NewSymbol("dict")));

      PocketSphinx* instance = new PocketSphinx();

      cmd_ln_t* config = cmd_ln_init(NULL, ps_args(), TRUE,
                 "-hmm", *hmmValue,
                 "-lm", *lmValue,
                 "-dict", *dictValue,
                 NULL);

      instance->m_ps = ps_init(config);

      instance->Wrap(args.This());
      return scope.Close(args.This());
    }

    static int Reset(PocketSphinx* instance) {
      instance->m_ts = instance->m_cont->read_ts;
     // cont_ad_reset(instance->m_cont);
      if (ps_start_utt(instance->m_ps, NULL) < 0) {
        return FAILED;
      }  

      return WAITING;
    }

    static int Calibrate(PocketSphinx* instance, int16* buffer, size_t length) {
      int result = cont_ad_calib_loop(instance->m_cont, buffer, length);

      if (result < 0) {
        printf("Error calibrating.\n");
        return FAILED;
      }

      if (result == 0) {
        printf("Calibration complete.\n");
        return RESETTING;
      }

      return CALIBRATING;
    }

    static int WaitForAudio(PocketSphinx* instance, int16* buffer, size_t length) {
      int32 k = cont_ad_read(instance->m_cont, buffer, length); 

      if (k < 0) {
        printf("Error clipping silence.\n");
        return FAILED;
      }

      if (k > 0) {
        ps_process_raw(instance->m_ps, buffer, k, FALSE, FALSE);
        instance->m_ts = instance->m_cont->read_ts;
        return LISTENING;
      }

      return WAITING;
    }

    static int Listen(PocketSphinx* instance, int16* buffer, size_t length) {
      int32 k = cont_ad_read(instance->m_cont, buffer, length); 

      if (k < 0) {
        printf("Error clipping silence.\n");
        return FAILED;
      }

      if (k > 0) {
        printf("heard something.\n");
        ps_process_raw(instance->m_ps, buffer, k, FALSE, FALSE);
        instance->m_ts = instance->m_cont->read_ts;

        return LISTENING;
      }

      if (instance->m_cont->read_ts - instance->m_ts > DEFAULT_SAMPLES_PER_SEC) {
        printf("okay heard you.\n");
        // Done collecting utterance. Guess the sentence send it to the callback.
        ps_end_utt(instance->m_ps);
        return PROCESSING;
      }

      return LISTENING;
    }

    static Handle<Value> Process(const Arguments& args) {
      v8::HandleScope scope;
      Local<Object> output = Object::New();
      PocketSphinx* instance = node::ObjectWrap::Unwrap<PocketSphinx>(args.This());

      if (instance->m_state == FAILED) {
        output->Set(String::NewSymbol("error"), String::NewSymbol("An unrecoverable error occurred."));
        return scope.Close(output);
      }

      if (!node::Buffer::HasInstance(args[0])) {
        output->Set(String::NewSymbol("error"), String::NewSymbol("Argument must be a buffer."));
        return scope.Close(output);
      }

      float*        bufferData   = (float*) node::Buffer::Data(args[0]);
      size_t        bufferLength = node::Buffer::Length(args[0]) / sizeof(float);

      // First, we need to downsample to the 16 kHz sample ratepocketsphinx needs for the 
      // language models most commonly used.
      SRC_DATA srcData;
      srcData.src_ratio = (double) 44100 / (double) DEFAULT_SAMPLES_PER_SEC;
      srcData.output_frames = bufferLength * srcData.src_ratio;
      srcData.data_in = bufferData;
      srcData.data_out = new float[srcData.output_frames];
      srcData.input_frames = bufferLength;

      int result = src_simple(&srcData, SRC_SINC_MEDIUM_QUALITY, 1);
      // Now we convert to int16
      int16* downsampled = new int16[srcData.output_frames];
      for (int i = 0; i < srcData.output_frames; i++) {
        downsampled[i] = srcData.data_out[i] * 32768;
      }
      delete [] srcData.data_out;

      // Here is our state machine code.
      switch(instance->m_state) {
        case CALIBRATING:
          printf("Calibrating...\n");
          instance->m_state = Calibrate(instance, downsampled, srcData.output_frames);
          break;
        case RESETTING:
          printf("Resetting...\n");
          instance->m_state = Reset(instance);
        case WAITING:
          instance->m_state = WaitForAudio(instance, downsampled, srcData.output_frames);
          break;
        case LISTENING:
          printf("Listening...\n");
          instance->m_state = Listen(instance, downsampled, srcData.output_frames);
          break;
        case PROCESSING:
          printf("Processing...\n");
          {
            const char* uttid;
            int32       score;
            const char* hyp = ps_get_hyp(instance->m_ps, &score, &uttid);

            printf("Hyp: %s\n", hyp);
            output->Set(String::NewSymbol("hyp"), String::NewSymbol(hyp));
            output->Set(String::NewSymbol("utterance"), String::NewSymbol(uttid));
            output->Set(String::NewSymbol("score"), NumberObject::New(score));
            instance->m_state = RESETTING;
          }
          break;
      }

      delete [] downsampled;

      if (instance->m_state == FAILED) {
        output->Set(String::NewSymbol("error"), String::NewSymbol("An unrecoverable error occurred."));
      }

      return scope.Close(output);
    }
};


v8::Persistent<FunctionTemplate> PocketSphinx::persistent_function_template;
extern "C" {
  static void init(Handle<Object> target) {
    PocketSphinx::Init(target);
  }
  
  NODE_MODULE(binding, init);
}