var PocketSphinx = require('../');
var app = require('express')();
var http = require('http').Server(app);
var io = require('socket.io')(http);

app.get('/', function(req, res){
  res.sendfile('index.html');
});

io.on('connection', function(socket){
	var ps = new PocketSphinx();

	ps.on('utterance', function(hyp, utt, score) {
		socket.emit('utterance', {hyp: hyp, utterance: utt, score:score});
	});

  socket.on('audio', function(data) {
  	console.log("audio data!");
    ps.write(data);
  });
});

http.listen(3000, function(){
  console.log('listening on *:3000');
});

