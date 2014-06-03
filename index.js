var binding = require("./build/Release/binding");
var util = require("util");
var stream = require("stream");
var spawn = require('child_process').spawn;



var PocketSphinx = function(options) {
	var self = this;
	var pkgconfig = spawn('pkg-config', ['--variable=modeldir', 'pocketsphinx']);
	pkgconfig.stdout.on('data', function(path) {
		options = options || {};
		path = ('' + path).trim();
		console.log(path + '/hmm/en_US/hub4wsj_sc_8k');
		options.hmm = options.hmm || (path + '/hmm/en_US/hub4wsj_sc_8k');
	//	options.lm = options.lm || (path +  '/lm/en_US/hub4.5000.DMP');
  //  options.dict = options.dict || (path + '/lm/en_US/cmu07a.dic');
   
		options.lm = options.lm || (path +  "/lm/en/turtle.DMP");
		options.dict = options.dict || (path + "/lm/en/turtle.dic");

    	// TODO: provide additional defaults

    self._binding = new binding.pocketSphinxBinding(options);
	});
}

util.inherits(PocketSphinx, stream.Writable);

PocketSphinx.prototype.write = function(chunk, encoding, callback) {
	if (!this._binding) {
		if (callback) callback('PocketSphinx not yet initialized');
		return;
	}

  var result = this._binding.process(chunk);
  if (result.error) {
  	if (callback)  callback(result.error);
  } else if (result.hyp) {
  	this.emit('utterance', result.hyp, result.utterance, result.score);
  	if (callback) callback();
  }
};

module.exports = PocketSphinx;


