
                              MANDELBULBER 2.35

0. Current macOS build (Qt 6) and MCP setup
--------------------------------------------

The steps below this section are the old Qt 5 / hand-built dependency notes.
Do not follow them. CircleCI already builds with Homebrew Qt 6. The full
click-by-click version, including Cursor and Claude Desktop, is in mcp/README.md
under "macOS: build the app, then connect Cursor or Claude".

    brew install llvm qt gsl lzo jpeg libpng libtiff libsndfile

`qt` from Homebrew is Qt 6. Use $(brew --prefix qt)/bin/qmake and macdeployqt.
Clone Khronos OpenCL-CLHPP and copy include/CL so cl.hpp exists (copy
opencl.hpp to cl.hpp). OpenCL.framework supplies the library; do not link
-lOpenCL. Put that CL directory on the qmake include path
(-I<path-to-build> -DCL_SILENCE_DEPRECATION -DCL_TARGET_OPENCL_VERSION=120).

From a build directory, pointing qmake at mandelbulber2/qmake:

    Apple Silicon:  mandelbulber-opencl-m1.pro
                    C++17, deployment target 12.0, /opt/homebrew, OpenMP off
    Intel:          mandelbulber-opencl-mac.pro
                    C++14. CircleCI symlinks Homebrew LLVM to /usr/local/opt/llvm

Then make, then:

    macdeployqt mandelbulber2.app -dmg

The built bundle name is mandelbulber2.app (qmake TARGET). A renamed copy may
be Mandelbulber2.app. The Python adapter checks both under /Applications and
~/Applications. The binary inside is Contents/MacOS/mandelbulber2.

macos.pri copies formula, examples, materials, icons, sounds, textures,
toolbar, data, language, opencl, and doc into Contents/Resources. The MCP
socket is compiled into the binary. data/mcp-dock-tooltips.json rides along
in that data directory, so an installed app has the dock-widget descriptions
without shipping qt/*.ui. The Python adapter in mcp/ is installed
separately with uv or pip and is not part of the dmg.

The app's data directory on macOS is ~/mandelbulber (not ~/.mandelbulber).
The session file is ~/mandelbulber/mcp-session.json. macOS limits a Unix
socket path to 104 bytes including the NUL. If the path under ~/mandelbulber
is 100 bytes or longer, the app listens on a shorter temp path and writes
that path into the session file.

New source files under src/ are picked up by qmake's src/*.cpp wildcard only
after you run qmake again. mcp_server.cpp and mcp_scene.cpp are already in
that wildcard.


1. What is Mandelbulber
-----------------------

In summary, Mandelbulber generates three-dimensional fractals.
Explore  trigonometric, hyper-complex, Mandelbox, IFS, Kleinian  and many other
3D fractals.  Render with a great palette  of customizable materials  to create
stunning images and videos. The possibilities are literally   i n f i n i t e !

2. Project Members and Links
----------------------------

PROJECT LEADER:  Krzysztof Marczak
PROJECT MEMBERS: Sebastian Jennen, Graeme McLaren, Bernardo Martelli,
                 Robert Pancoast, knighty, makemeunsee, Marius Schilder,
                 Ryan Hitchman, Jeff Epler, Martin Reinecke, Quazgaa, Adrian Meyer
                 github:rikardfalkeborn, github:orbitcowboy, github:brunetton,
                 github:biberino, github:luchansky, github:jeroenrijckaert,
                 github:KoviRobi, github:psyriccio, github:valera-rozuvan, 
                 github:probonopd, github:mia-0, github:gitter-badger, 
                 github:danuni, github:Starmute

CONTACT:         buddhi1980@gmail.com
DOWNLOAD:        https://sourceforge.net/projects/mandelbulber/
DEVELOPMENT:     https://github.com/buddhi1980/mandelbulber2/
COMMUNITY:       http://www.fractalforums.com/mandelbulber/
                 https://www.facebook.com/groups/mandelbulber/
FANSITE:         http://www.mandelbulber.com
                 http://www.mandelbulber.org

LICENCE:         GNU GPL v3.0

3. REQUIREMENTS
---------------

- unknown MacOS version
- very fast CPU (recommended 2GHz, 4 cores)
- 2GB RAM

4. COMPILING
------------

/Download and install the latest version of Xcode (and Developer Tools) from the Apple developer website or get it using the Mac App Store.
//Download and install the latest version of Qt from http://www.qt.io/download-open-source/
//download source from http://sourceforge.net/projects/libpng/files/libpng16/
//Build the PNG library
cd libpng-1.6.x
./configure --prefix=/usr/local/
make
make test
//Install the PNG library under /usr/local
sudo make install
//download source from ftp://ftp.gnu.org/gnu/gsl/
//Build GNU scientific library
cd gsl-latest
./configure --prefix=/usr/local/
make
//Install the GSL library under /usr/local
sudo make install
//Build TIFF library
curl --remote-name http://download.osgeo.org/libtiff/tiff-4.0.6.tar.gz
tar -xzvf tiff-4.0.6.tar.gz
cd tiff-4.0.6
./configure --prefix=/usr/local/libtiff-4.0.6
make
//Install the TIFF library under /usr/local
make install
ln -s libtiff-4.0.6 /usr/local/libtiff
//download source from https://github.com/buddhi1980/mandelbulber2
//Build mandelbulber
cd qmake
qmake -spec macx-g++
//xcode can also be used
//qmake -spec macx-xcode mandelbulber.pro
qmake -project
qmake -o makefile mandelbulber.pro
make
//copy mandelbulber's settings files
//download the share folder from https://github.com/buddhi1980/mandelbulber2/tree/master/mandelbulber2/deploy
//copy the contents of the folder inside "/usr/share/mandelbulber2"

5. INSTALLATION
---------------

see 4.

6. TESTED ON:
------------

- unknown

7. USAGE
--------

The main documentation is the PDF file  Mandelbulber_Manual.pdf,  which you can
find in this very folder.
Also you can open it from Mandelbulber under: [Menu > Help > User Manual]
A lot of useful information you can find  in the tooltips.  Just hover with the
mouse pointer over the edit fields.
More useful material can be found in the wiki documentation for old
Mandelbulber (v1) here: http://wiki.mandelbulber.com/index.php?title=Main_Page

8. COMMAND LINE INTERFACE
-------------------------

The program can be used "headless" from the console. This way the program can
be integrated in scripts or toolchains and even be run without a x-server
completely.

Usage: ./mandelbulber2 [options] settings_file

Options:
  -h, --help             Displays this help.
  -v, --version          Displays version information.
  -n, --nogui            Starts the program without a GUI.
  -o, --output <N>       Saves rendered image(s) to this file / folder.
  --logfilepath <N>      Specify custom system log filepath (default is:
                         ~/.mandelbulber_log.txt).
  -K, --keyframe         Renders keyframe animation.
  -F, --flight           Renders flight animation.
  -X, --never-delete     Never delete data, instead Exit CLI application.
  -s, --start <N>        Starts rendering from frame number <N>.
  -e, --end <N>          Stops rendering on frame number <N>.
  -L, --list             Lists all possible parameters '<KEY>' with
                         corresponding default value '<VALUE>'.
  -f, --format <FORMAT>  Image output format:
                          jpg  - JPEG format (default)
                          png  - PNG format
                          exr  - EXR format
                          tiff - TIFF format
                         Legacy formats for still frames:
                          png16 - 16-bit PNG format
                          png16alpha - 16-bit PNG with alpha channel format
  -r, --res <WxH>        Overrides image resolution. Specify as width and
                         height separated by 'x'
  --fpk <N>              Overrides frames per key parameter.
  -S, --server           Sets application as a server listening for clients.
  -H, --host <N.N.N.N>   Sets application as a client connected to server of
                         given host address (Host can be of type IPv4, IPv6 and
                         Domain name address).
  -p, --port <N>         Sets network port number for netrender (default 5555).
  -C, --no-cli-color     Starts program without ANSI colors, when execution on
                         CLI.
  -q, --queue            Renders all images from common queue.
  -t, --test             Runs testcases on the mandelbulber instance
  -b, --benchmark        Runs benchmarks on the mandelbulber instance, specify
                         optional parameter difficulty (1 -> very easy, > 20 ->
                         very hard, 10 -> default). When [output] option is set
                         to a folder, the example-test images will be stored
                         there.
  -T, --touch            Resaves a settings file (can be used to update a
                         settings file)
  -V, --voxel <FORMAT>   Renders the voxel volume. Output formats are:
                          slice - stack of PNG images into one folder (default)
                          ply   - Polygon File Format (single 3d file)

  -O, --override <...>   <KEY=VALUE> overrides item '<KEY>' from settings file
                         with new value '<VALUE>'.
                         Specify multiple KEY=VALUE pairs by separating with a
                         '#': <KEY1=VALUE1#KEY2=VALUE2>. Quote whole expression
                         to avoid whitespace parsing issues
                         Override fractal parameter in the form
                         'fractal<N>_KEY=VALUE' with <N> being index of fractal
  --stats                Shows statistics while rendering in CLI mode.
  -g, --gpu              Runs the program in opencl mode and selects first
                         available gpu device.
  -G, --gpuall           Runs the program in opencl mode and selects all
                         available gpu devices.
  --help-input           Shows help about input.
  --help-examples        Shows example commands.
  --help-opencl          Shows commands regarding OpenCL.

Arguments:
  settings_file          file with fractal settings (program also tries
                         to find file in ./mandelbulber/settings directory)
                         When settings_file is put as a command line argument
                         then program will start in noGUI mode<settings_file>
                         can also be specified as a list, see all options with
                         --help-input
