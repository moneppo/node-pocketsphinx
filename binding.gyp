{
  "targets": [
    {
      "target_name": "binding",
      "cflags": ["<!(pkg-config --cflags pocketsphinx sphinxbase)"],
	    "ldflags": ["<!(pkg-config --libs pocketsphinx sphinxbase)"],
	    "xcode_settings": {
	    	"OTHER_CFLAGS": ["<!(pkg-config --cflags pocketsphinx sphinxbase)"],
	    	"OTHER_LDFLAGS": ["<!(pkg-config --libs pocketsphinx sphinxbase)"],
			},
      "sources": [ "src/binding.cpp" ]
    }
  ]
}