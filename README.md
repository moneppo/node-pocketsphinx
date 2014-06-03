*Note: This library doesn't work properly yet. There is a resampling issue currently that will mke the system simply output gibberish. We're getting there, though.*

pocketsphinx
============

A Pocket Sphinx binding for Node.JS, focusing on continuous recognition.

Installing
----------

pocketsphinx requires [CMU's Pocket Sphinx libraries](http://cmusphinx.sourceforge.net/). You can build and install it yourself, but I highly recommend just getting it from a package manager. For Mac OSX I highly recommend [homebrew](http://brew.sh/). It makes this a snap:

    brew install cmu-pocketsphinx

Usage
-----

A `PocketSphinx` object is a writable stream that accepts mono 16 kHz 16-bit PCM data. When an utterance is detected, e.g. a phrase, word, or statement, the object emits an `utterance` event with the details of the detected utterance.

    var PocketSphinx = require('pocketsphinx');
    
    var ps = new PocketSphinx();
    
    ps.on('utterance', function(hyp, utt, score) {
		console.log( 'Guessed phrase: ' + hyp);
		console.log( 'Confidence score: ' + score);
		console.log( 'Unique utterance id: ' + utt);
	});
    
    ps.write(myMicrophoneData);
    
Data can be retrieved from files, but the demo shows how to use getUserMedia and Socket.io to stream data from a web frontend. This is also a good demo of how to use getUserMedia for cool stuff, considering the documentation for it is a little sparse.

Notes
-----

This is still in active development. It has only been tested on Mac OSX 10.9.2 with Pocket Sphinx installed using homebrew.


