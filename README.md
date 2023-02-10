# wav-marker

imports a label file from audacity and embeds the labels directly into a .wav matching the output of other DAWs.

based off of [wavcuepoint.c](https://gist.github.com/TimMoore/a2dfb007004c87ac3a3858309a7911d1) originally written by jimmcgowan and forked by dhilowitz and TimMoore.

## Usage

```wav-marker WAVFILE LABELFILE OUTPUTFILE```

Label file should be in the format exported by audacity as described [here](https://manual.audacityteam.org/man/importing_and_exporting_labels.html)

Only the start times are used. End times are ignored.
