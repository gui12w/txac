It transforms audio into text, and then, text to audio, it's pretty simple actually

It has 3 executables, each one doing it's thing, 

The input: takes wav files (16 bits or 32 bits little-endian) and transforms into txac without any loss.

The output: takes the txac file and transforms it back to wav (if you want to check how is the quality in audacity or smth)

The player: takes the txac file and play it for you without any new file

It's a lossless codec, can have a 0,0012 db imprecision (bet you can hear it), it doesn't use metadata so you have to put which is the frequency and channels (sorry)

Commands for each executable:

txacinput: txacinput <input_audio> <output.txac>

txacoutput: txacoutput <input.txac> <output.wav> [sample_rate] [channels]

txacplay: txacplay <file.txac> [sample_rate] [channels]

And yes, you can put [ffmpeg](https://www.ffmpeg.org) in the PATH and use it with any audio source (it does NOT contain any ffmpeg code, it only puts a command line in cmd)

(i'll try to update somethings like: archive compression, buffer on the txac play (it won't decode everything first), metadata, a new type of compression method (but it will take too much long to see the light), and this list can expand)

txacplay.c has the [qoaplay.c](https://github.com/phoboslab/qoa) as a base

creator of the qoaplay.c and the QOA codec: Dominic Szablewski
