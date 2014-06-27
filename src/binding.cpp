#include <node.h>
#include <node_buffer.h>
#include <pocketsphinx.h>
#include <sphinxbase/err.h>
#include <sphinxbase/cont_ad.h>

using namespace v8;

typedef struct AsyncData {
  float* data;
  size_t length; 
  Persistent<Function> callback;
  bool hasHyp;
  const char* uttid;
  int32 score;
  const char* hyp;
} AsyncData;

enum STATE {
  CALIBRATING,
  RESETTING,
  WAITING,
  LISTENING,
  PROCESSING,
  FAILED
};

// GLOBALS
cont_ad_t* cont;
ps_decoder_t* ps;
STATE state = CALIBRATING;
int32 ts;
Persistent<Function> callback;
bool ready = false;

STATE Calibrate(int16* buffer, size_t length) {
  int result = cont_ad_calib_loop(cont, buffer, length);

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

STATE Reset() {
  ts = cont->read_ts;
  // cont_ad_reset(cont);
  if (ps_start_utt(ps, NULL) < 0) {
    printf("Error starting utterance.\n");
    return FAILED;
  }  

  return WAITING;
}

STATE WaitForAudio(int16* buffer, size_t length) {
  int32 k = cont_ad_read(cont, buffer, length); 

  if (k < 0) {
    printf("Error clipping silence.\n");
    return FAILED;
  }

  if (k > 0) {
    printf("processing...\n");
    ps_process_raw(ps, buffer, k, FALSE, FALSE);
    ts = cont->read_ts;
    return LISTENING;
  }

  return WAITING;
}

STATE Listen(int16* buffer, size_t length) {
  int32 k = cont_ad_read(cont, buffer, length); 

  if (k < 0) {
    printf("Error clipping silence.\n");
    return FAILED;
  }

  if (k > 0) {
    printf("heard something.\n");
    ps_process_raw(ps, buffer, k, FALSE, FALSE);
    ts = cont->read_ts;

    return LISTENING;
  }

  if (cont->read_ts - ts > DEFAULT_SAMPLES_PER_SEC) {
    printf("okay heard you.\n");
    // Done collecting utterance. Guess the sentence send it to the callback.
    ps_end_utt(ps);
    return PROCESSING;
  }

  return LISTENING;
}

void Process(AsyncData* data) { 
  printf("Processing...\n");  
  
  int16* downsampled = new int16[data->length];
  for (size_t i = 0; i < data->length; i++) {
    downsampled[i] = data->data[i] * 32768;
  }

  // Here is our state machine code.
  switch(state) {
    case CALIBRATING:
      printf("calibrating...\n");
      state = Calibrate(downsampled, data->length);
      break;
    case RESETTING:
      printf("resetting...\n");
      state = Reset();
    case WAITING:
      printf("waiting...\n");
      state = WaitForAudio(downsampled, data->length);
      break;
    case LISTENING:
      printf("listening...\n");
      state = Listen(downsampled, data->length);
      break;
    case PROCESSING:
      printf("processing...\n");
      data->hasHyp = true;
      data->hyp = ps_get_hyp(ps, &data->score, &data->uttid);   
      state = RESETTING;
      break;
    case FAILED:
    default:
      printf("Something really bad happened");
      break;
  }

  delete [] downsampled;
}

// Function to execute inside the worker-thread.
// It is not safe to access V8, or V8 data structures
// here, so everything we need for input and output
// should go on our req->data object.
void AsyncWork(uv_work_t *req) {
  // fetch our data structure
  AsyncData *asyncData = (AsyncData *)req->data;
  // run Estimate() and assign the result to our data structure
  Process(asyncData);
}

// Function to execute when the async work is complete
// this function will be run inside the main event loop
// so it is safe to use V8 again
void AsyncAfter(uv_work_t *req) {
  HandleScope scope;

  // fetch our data structure
  AsyncData *asyncData = (AsyncData *)req->data;

  if (asyncData->hasHyp) {
    Handle<Object> output = Object::New();

    printf("Hyp: %s\n", asyncData->hyp);
    output->Set(String::NewSymbol("hyp"), String::NewSymbol(asyncData->hyp));
    output->Set(String::NewSymbol("utterance"), String::NewSymbol(asyncData->uttid));
    output->Set(String::NewSymbol("score"), NumberObject::New(asyncData->score));
    Handle<Value> argv[1] = { output };

    // surround in a try/catch for safety
    TryCatch try_catch;
    
    // execute the callback function
    asyncData->callback->Call(Context::GetCurrent()->Global(), 2, argv);
    if (try_catch.HasCaught())
      node::FatalException(try_catch);
  }

  // dispose the Persistent handle so the callback
  // function can be garbage-collected
  asyncData->callback.Dispose();
  // clean up any memory we allocated
  delete asyncData;
  delete req;
}

Handle<Value> WriteDataAsync(const Arguments& args) {
  HandleScope scope;
  if (!ready) {
    return scope.Close(Undefined());
  }

  // create an async work token
  uv_work_t *req = new uv_work_t;

  // assign our data structure that will be passed around
  AsyncData *asyncData = new AsyncData;
  req->data = asyncData;

  // expect a number as the first argument

  asyncData->data = (float*) node::Buffer::Data(args[0]);
  asyncData->length = node::Buffer::Length(args[0]) / sizeof(float);
  asyncData->hasHyp = false;

  // expect a function as the second argument
  // we create a Persistent reference to it so
  // it won't be garbage-collected
  asyncData->callback = callback;

  // pass the work token to libuv to be run when a
  // worker-thread is available to
  uv_queue_work(
    uv_default_loop(),
    req,                          // work token
    AsyncWork,                    // work function
    (uv_after_work_cb)AsyncAfter  // function to run when complete
  );

  return scope.Close(Undefined());
}

// Asynchronous access to the `Estimate()` function
Handle<Value> Init(const Arguments& args) {
  HandleScope scope;

  Handle<Object> options = args[0]->ToObject();
  callback = Persistent<Function>::New(Local<Function>::Cast(args[1]));

  //TODO: Enable passing of config options for most params. Start with the three below.
  v8::String::AsciiValue hmmValue(options->Get(String::NewSymbol("hmm")));
  v8::String::AsciiValue lmValue(options->Get(String::NewSymbol("lm")));
  v8::String::AsciiValue dictValue(options->Get(String::NewSymbol("dict")));
  v8::String::AsciiValue samprateValue(options->Get(String::NewSymbol("samprate")));
  v8::String::AsciiValue nfftValue(options->Get(String::NewSymbol("nfft")));

  cmd_ln_t* config = cmd_ln_init(NULL, ps_args(), TRUE,
    "-hmm", *hmmValue,
    "-lm", *lmValue,
    "-dict", *dictValue,
    "-samprate", *samprateValue,
    "-nfft", *nfftValue,
    NULL);

  ps = ps_init(config);
  cont = cont_ad_init(NULL, NULL);

  ready = true;
  return scope.Close(Undefined());
}



void InitAll(Handle<Object> exports) {
  exports->Set(String::NewSymbol("WriteDataAsync"),
      FunctionTemplate::New(WriteDataAsync)->GetFunction());
  exports->Set(String::NewSymbol("Init"),
      FunctionTemplate::New(Init)->GetFunction());
}

NODE_MODULE(addon, InitAll)