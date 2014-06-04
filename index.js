var binding = require("./build/Release/binding");
var util = require("util");
var stream = require("stream");
var spawn = require('child_process').spawn;



var PocketSphinx = function(options) {

	var callback = function(result) {
		if (result.error) {
			this.emit("error", result.error);
			return;
		}

		this.emit('utterance', result.hyp, result.utterance, result.score);
	}

	var self = this;
	var pkgconfig = spawn('pkg-config', ['--variable=modeldir', 'pocketsphinx']);
	pkgconfig.stdout.on('data', function(path) {
		options = options || {};
		path = ('' + path).trim();
		console.log(path + '/hmm/en_US/hub4wsj_sc_8k');
		options.hmm = options.hmm || (path + '/hmm/en_US/hub4wsj_sc_8k');
		options.lm = options.lm || (path +  '/lm/en_US/hub4.5000.DMP');
    	options.dict = options.dict || (path + '/lm/en_US/cmu07a.dic');
    	options.samprate = '' + (options.samprate || 44100);
    	options.nfft = '' + (options.nfft || 2048);
    	// TODO: provide additional defaults

    	self._binding = new binding.pocketSphinxBinding(options, callback);
	});
}

util.inherits(PocketSphinx, stream.Writable);

PocketSphinx.prototype.write = function(chunk) {
	if (!this._binding) {
		if (callback) callback('PocketSphinx not yet initialized');
		return;
	}

  this._binding.writeData(chunk);
};

module.exports = PocketSphinx;


