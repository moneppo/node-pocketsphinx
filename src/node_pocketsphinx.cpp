#include <v8.h> 
#include <node.h>
#include <node_buffer.h>
#include <string>
#include <pocketsphinx.h>
#include <sphinxbase/err.h>
#include <sphinxbase/cont_ad.h>
#include <uv.h>

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
    Persistent<Function> m_callback;
    int m_state;
    uv_loop_t *m_loop;

    struct process_baton {
      uv_work_t* req;
      PocketSphinx* instance;
      size_t length;
      float* data;
    };

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
      m_callback.Dispose();
    }

    static v8::Persistent<FunctionTemplate> persistent_function_template;

    static void Init(Handle<Object> target) {
      v8::HandleScope scope;
   
      // Bind "New" function
      v8::Local<FunctionTemplate> local_function_template = v8::FunctionTemplate::New(New);
      PocketSphinx::persistent_function_template = v8::Persistent<FunctionTemplate>::New(local_function_template);
      PocketSphinx::persistent_function_template->InstanceTemplate()->SetInternalFieldCount(1); 
      PocketSphinx::persistent_function_template->SetClassName(v8::String::NewSymbol("PocketSphinx"));

      NODE_SET_PROTOTYPE_METHOD(PocketSphinx::persistent_function_template, "writeData", WriteData);
      
      target->Set(String::NewSymbol("pocketSphinxBinding"), PocketSphinx::persistent_function_template->GetFunction());
    }

    static Handle<Value> New(const Arguments& args) {
      v8::HandleScope scope;
      Local<Object> options = args[0]->ToObject();

      //TODO: Enable passing of config options for most params. Start with the three below.
      v8::String::AsciiValue hmmValue(options->Get(String::NewSymbol("hmm")));
      v8::String::AsciiValue lmValue(options->Get(String::NewSymbol("lm")));
      v8::String::AsciiValue dictValue(options->Get(String::NewSymbol("dict")));
      v8::String::AsciiValue samprateValue(options->Get(String::NewSymbol("samprate")));
      v8::String::AsciiValue nfftValue(options->Get(String::NewSymbol("nfft")));

      PocketSphinx* instance = new PocketSphinx();

      instance->m_callback = Persistent<Function>::New(Local<Function>::Cast(args[1]));
      printf("r\n");
      cmd_ln_t* config = cmd_ln_init(NULL, ps_args(), TRUE,
                 "-hmm", *hmmValue,
                 "-lm", *lmValue,
                 "-dict", *dictValue,
                 "-samprate", *samprateValue,
                 "-nfft", *nfftValue,
                 NULL);

      instance->m_ps = ps_init(config);
      instance->m_loop = uv_loop_new();

      instance->Wrap(args.This());
      return scope.Close(args.This());
    }

    static int Reset(PocketSphinx* instance) {
      instance->m_ts = instance->m_cont->read_ts;
     // cont_ad_reset(instance->m_cont);
      if (ps_start_utt(instance->m_ps, NULL) < 0) {
        printf("Error starting utterance.\n");
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

    static Handle<Value> WriteData(const Arguments& args) {
      v8::HandleScope scope;
      PocketSphinx* instance = node::ObjectWrap::Unwrap<PocketSphinx>(args.This());

      if (!node::Buffer::HasInstance(args[0])) {
        Local<Object> output = Object::New();
        output->Set(String::NewSymbol("error"), String::NewSymbol("Argument must be a buffer."));
        Local<Value> argv[1] = { output };
        instance->m_callback->Call(Context::GetCurrent()->Global(), 1, argv);
        return scope.Close(Undefined());
      }

      uv_work_t req;
      process_baton* baton = new process_baton();

      baton->req = &req;
      baton->instance = instance;
      baton->data = (float*) node::Buffer::Data(args[0]);
      baton->length = node::Buffer::Length(args[0]) / sizeof(float);
      req.data = (void*) baton;

      uv_queue_work(instance->m_loop, &req, Process, AfterProcess);
      return scope.Close(Undefined());
    }

    static void AfterProcess(uv_work_t* req, int status) {
      process_baton *baton = (process_baton*) req->data;
      delete baton;
    }

    static void Process(uv_work_t* req) {
      PocketSphinx* instance = ((process_baton*) req->data)->instance;     
      Local<Object> output;// = Object::New();
      float* bufferData = ((process_baton*) req->data)->data;
      size_t bufferLength = ((process_baton*) req->data)->length;

      if (instance->m_state == FAILED) {
        output->Set(String::NewSymbol("error"), String::NewSymbol("An unrecoverable error occurred."));
        Local<Value> argv[1] = { output };
        instance->m_callback->Call(Context::GetCurrent()->Global(), 1, argv);
        return;
      }
      
      int16* downsampled = new int16[bufferLength];
      for (int i = 0; i < bufferLength; i++) {
        downsampled[i] = bufferData[i] * 32768;
      }

      // Here is our state machine code.
      switch(instance->m_state) {
        case CALIBRATING:
          printf("calibrating...\n");
          instance->m_state = Calibrate(instance, downsampled, bufferLength);
          break;
        case RESETTING:
          printf("resetting...\n");
          instance->m_state = Reset(instance);
        case WAITING:
          printf("waiting...\n");
          instance->m_state = WaitForAudio(instance, downsampled, bufferLength);
          break;
        case LISTENING:
          printf("listening...\n");
          instance->m_state = Listen(instance, downsampled, bufferLength);
          break;
        case PROCESSING:
          printf("processing...\n");
          {
            const char* uttid;
            int32       score;
            const char* hyp = ps_get_hyp(instance->m_ps, &score, &uttid);

            printf("Hyp: %s\n", hyp);
            output->Set(String::NewSymbol("hyp"), String::NewSymbol(hyp));
            output->Set(String::NewSymbol("utterance"), String::NewSymbol(uttid));
            output->Set(String::NewSymbol("score"), NumberObject::New(score));
            Local<Value> argv[1] = { output };
            instance->m_callback->Call(Context::GetCurrent()->Global(), 1, argv);
            instance->m_state = RESETTING;
          }
          break;
      }

      delete [] downsampled;

      if (instance->m_state == FAILED) {
        output->Set(String::NewSymbol("error"), String::NewSymbol("An unrecoverable error occurred."));
        Local<Value> argv[1] = { output };
        instance->m_callback->Call(Context::GetCurrent()->Global(), 1, argv);
      }
    }
};


v8::Persistent<FunctionTemplate> PocketSphinx::persistent_function_template;
extern "C" {
  static void init(Handle<Object> target) {
    PocketSphinx::Init(target);
  }
  
  NODE_MODULE(binding, init);
}