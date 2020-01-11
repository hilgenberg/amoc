# AMOC
Fork of [MOC](http://moc.daper.net/) with the following changes:

* C++ port (in progress, but the interface is somewhat done. Lots of things still removed (lyrics f.e.))
* ratings
* three-column layout (artist/album/title)
* multi-selection: Shift+arrows extend the selection, which can then be moved, deleted or added all at once
* mouse interface (actually quite useful on a tablet!) - layout can be dragged, timebar can be clicked for sseking,
double-click files to play them, ...
* clients no longer sync the playlist amongst themselves - the server holds the playlist and the "clear playlist" command
now creates an empty local playlist for the client. Playing from that list sends it off to become the new server playlist.
Clearing an empty playlist goes back to and fetches the server playlist again.
* seeking through EOF plays the next file
* full paths are printed relative to the music directory, ~ or the longest common prefix
* works well with small terminals (down to 8 columns x 6 lines)
* switched from AutoTools to SConstruct

To build: Install all the libraries for sound formats and outputs (no options to remove things from the makefile yet).
The list is in the SConstruct file (near the bottom: libs = '...').
libtimidity is [here](https://sourceforge.net/p/libtimidity/libtimidity/ci/master/tree/).
Then just ```scons```. Copy your config from ~/.moc/ to ~/.config/amoc/ or ~/.amoc/ and
```ln -s themes/your_favorite colors```. Check config.example for new options.
