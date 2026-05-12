It transforms audio into text, and then, text to audio, it's pretty simple actually

It has 4 executables, each one doing it's thing,

The input: takes wav files (16 bits or 32 bits little-endian) and transforms into txac without any loss.

The output: takes the txac file and transforms it back to wav (if you want to check how is the quality in audacity or smth)

The player: takes the txac file and play it for you without any new file

The player exclusive: does the exact same thing as the player, but plays in exclusive wasapi mode, but you still need to manually set the sample rate on windows to match the source, or he'll resample it

It's a lossless codec, can have a 0,0012 db imprecision (bet you can hear it), ~it doesn't use metadata so you have to put which is the frequency and channels~ it uses metadata so you don't have to put the frequency and channels, also, thanks for the use of conteiner, it can now run each channel in one core so, yes, this is a multi-core codec, the codec now uses delta encoding and rice encoding

Commands for each executable:

txacinput: txacinput <input_audio> <output.txac>

txacoutput: txacoutput <input.txac> <output.wav>

txacplay: txacplay <file.txac>

txacplaye: txacplaye <file.txac>

And yes, you can put ffmpeg in the PATH and use it with any audio source (it does NOT contain any ffmpeg code, it only puts a command line in cmd)

Updates can be check on the documentation

(i'll try to update somethings like: ~archive compression~, buffer on the txac play (it won't decode everything first), ~metadata~, a new type of compression method (but it will take too much long to see the light), and this list can expand)

txacplay.c and txacplaye.c has the qoaplay.c as a base

creator of the qoaplay.c and the QOA codec: Dominic Szablewski
