#include <v8.h> 
#include <node.h>
#include <node_buffer.h>
#include <string>
#include <pocketsphinx.h>
#include <sphinxbase/err.h>
#include <sphinxbase/cont_ad.h>
#include <uv.h>
#include <vector>

using namespace v8;
using namespace std;


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
    bool stop;

    struct Data {
      size_t length;
      float* data;
    };

    uv_cond_t cv;
    uv_mutex_t mutex;
    uv_thread_t work_thread;
    vector<Data*> queue;

  public:
    PocketSphinx() {
      printf("Initializing...");
      fflush(stdout); 
      m_cont = cont_ad_init(NULL, NULL);
      m_state = CALIBRATING;
      stop = false;
      uv_mutex_init(&mutex);
      uv_cond_init(&cv);

      uv_thread_create(&work_thread, Thread, this);
    }

    ~PocketSphinx() {
      cont_ad_close(m_cont);
      ps_free(m_ps);
      stop = true;
      uv_thread_join(&work_thread);
      uv_mutex_destroy(&mutex);
      uv_cond_destroy(&cv);
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
        printf("processing...\n");
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

    static void Thread(void* data) {
      PocketSphinx* instance = (PocketSphinx*) data;
      vector<Data*> consumer_work;
      while(!instance->stop) {
        uv_mutex_lock(&instance->mutex);
      
        while(instance->queue.size() == 0) { 
          uv_cond_wait(&instance->cv, &instance->mutex);
        }
   
        std::copy(instance->queue.begin(), 
                  instance->queue.end(), 
                  std::back_inserter(consumer_work));
        instance->queue.clear();
        
        uv_mutex_unlock(&instance->mutex);
     
        for(vector<Data*>::iterator it = consumer_work.begin(); it != consumer_work.end(); ++it) {
          Data* data = *it;   
          Process(data, instance);  
          printf("Deleting.");
          delete data;
          printf("Made it.");
          data = NULL;
        }    
        consumer_work.clear();
  }
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
      

      Data* data = new Data();
      data->data = (float*) node::Buffer::Data(args[0]);
      data->length = node::Buffer::Length(args[0]) / sizeof(float);
 
      uv_mutex_lock(&instance->mutex);
      {
        instance->queue.push_back(data);
      }
      uv_cond_signal(&instance->cv);
      uv_mutex_unlock(&instance->mutex);

      return scope.Close(Undefined());

    }

    static void Process(Data* buffer, PocketSphinx* instance) { 
      printf("Processing...\n");  

      if (instance->m_state == FAILED) {
        Local<Object> output = Object::New();
        output->Set(String::NewSymbol("error"), String::NewSymbol("An unrecoverable error occurred."));
        Local<Value> argv[1] = { output };
        instance->m_callback->Call(Context::GetCurrent()->Global(), 1, argv);
        return;
      }
      
      int16* downsampled = new int16[buffer->length];
      for (int i = 0; i < buffer->length; i++) {
        downsampled[i] = buffer->data[i] * 32768;
      }

      // Here is our state machine code.
      switch(instance->m_state) {
        case CALIBRATING:
          printf("calibrating...\n");
          instance->m_state = Calibrate(instance, downsampled, buffer->length);
          break;
        case RESETTING:
          printf("resetting...\n");
          instance->m_state = Reset(instance);
        case WAITING:
          printf("waiting...\n");
          instance->m_state = WaitForAudio(instance, downsampled, buffer->length);
          break;
        case LISTENING:
          printf("listening...\n");
          instance->m_state = Listen(instance, downsampled, buffer->length);
          break;
        case PROCESSING:
          printf("processing...\n");
          {
            const char* uttid;
            int32       score;
            const char* hyp = ps_get_hyp(instance->m_ps, &score, &uttid);
            Local<Object> output = Object::New();

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

      printf("Deleting...");
      delete [] downsampled;
      printf("Made it.");

      if (instance->m_state == FAILED) {
        Local<Object> output = Object::New();
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