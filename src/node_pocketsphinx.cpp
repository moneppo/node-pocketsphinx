#include <v8.h> 
#include <node.h>
#include <node_buffer.h>
#include <string>
#include <pocketsphinx.h>
#include <sphinxbase/err.h>
#include <sphinxbase/cont_ad.h>

using namespace v8;

union byteToInt16 {
    unsigned char char_buf[128];
    int int_buf[64];
};

class PocketSphinx : node::ObjectWrap {
  private:
    cont_ad_t* m_cont;
    ps_decoder_t* m_ps;
    int32 m_ts;

    bool m_calibrated;

  public:
    PocketSphinx() {
      m_cont = cont_ad_init(NULL, NULL);
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

    static Local<Object> Die(const char* error) {
      Local<Object> result = Object::New();
      result->Set(String::NewSymbol("error"), String::NewSymbol(error));

      return result;
    }

    static Handle<Value> Process(const Arguments& args) {
      v8::HandleScope scope;

      if (!node::Buffer::HasInstance(args[0])) {
        return scope.Close(Die("Argument must be a buffer."));
      }

      int16*        bufferData   = (int16*) node::Buffer::Data(args[0]);
      size_t        bufferLength = node::Buffer::Length(args[0]) / sizeof(int16);

      PocketSphinx* instance = node::ObjectWrap::Unwrap<PocketSphinx>(args.This());

      if (!instance->m_calibrated) {
        int32 result = cont_ad_calib_loop(instance->m_cont, bufferData, bufferLength);
        if (result < 0) {
          return scope.Close(Die("Silence calibration failed"));
        }

        if (result == 0) {
          instance->m_calibrated = true;
          instance->m_ts = instance->m_cont->read_ts;
          
          if (ps_start_utt(instance->m_ps, NULL) < 0) {
            return scope.Close(Die("Couldn't start a new utterance"));
          }  
          return scope.Close(Object::New());
        }
      }

      int32 k = cont_ad_read(instance->m_cont, bufferData, bufferLength);

      if (k < 0) {
        return scope.Close(Die("Silence clipping encountered an error"));
      }

      // k==0 means the buffer had silence. Was it longer than a second?
      if (k == 0) {
        if (instance->m_cont->read_ts - instance->m_ts > DEFAULT_SAMPLES_PER_SEC) {

          // Done collecting utterance. Guess the sentence send it to the callback.
          ps_end_utt(instance->m_ps);
          const char* uttid;
          int32       score;
          const char* hyp = ps_get_hyp(instance->m_ps, &score, &uttid);

          Local<Object> result = Object::New();
          result->Set(String::NewSymbol("hyp"), String::NewSymbol(hyp));
          result->Set(String::NewSymbol("utterance"), String::NewSymbol(uttid));
          result->Set(String::NewSymbol("score"), NumberObject::New(score));

          // We start again. Reset the clock, the silence detection, 
          // and start a new utterance
          instance->m_ts = instance->m_cont->read_ts;
          cont_ad_reset(instance->m_cont);
          if (ps_start_utt(instance->m_ps, NULL) < 0) {
            return scope.Close(Die("Couldn't start a new utterance"));
          }  

          return scope.Close(result);
        }
      } else {
        ps_process_raw(instance->m_ps, bufferData, k, FALSE, FALSE);
        instance->m_ts = instance->m_cont->read_ts;
      }

      return scope.Close(Object::New());
    }

};


v8::Persistent<FunctionTemplate> PocketSphinx::persistent_function_template;
extern "C" {
  static void init(Handle<Object> target) {
    PocketSphinx::Init(target);
  }
  
  NODE_MODULE(binding, init);
}