# Key Debouncer

## About
This is a quick and simple per-key keyboard input debouncer specifically designed to debounce **any standard keyboard**, regardless of input language; key name translations are provided by `libevdev` for user-friendly logs.

## Contributing
If you would like to contribute to this project, please fork this repository, make your changes, and submit a pull request! All are welcome to do so, however acceptance of pull requests is at the discretion of the project maintainer (currently me, @Giantvince1).

## How to Use
1. Clone the repository using `git pull https://github.com/VillageOfGamers/key-debouncer.git` and then `cd` into the directory that gets created.

2. Install the following items: your kernel headers, GCC, basic build tools, and libevdev headers. On Debian and Ubuntu, these following packages: `gcc libevdev-dev linux-headers-generic build-essential` are the ones you need. Find your equivalents in your package manager if you're on another distro.

3. Build the program using `make` and run `sudo ./bin/debouncectl show` to get a filtered list of potential keyboard device nodes. Find your keyboard and remember which one it is.

4. Test out the program by running `nohup ./bin/debounced &` in order to force the daemon to the background, then run `debouncectl start <device>` to make the daemon latch to your keyboard.

4. If you are satisfied with the end result, run `sudo make install` in order to install the systemD unit file inside `/etc/systemd/system/` and the binaries themselves into `/usr/local/bin/` (these are default paths for non-packaged applications in Debian).

5. Log out, and then immedately log back in; you will now be part of the `input` group on your system. This is required to be able to see nodes inside `/dev/input` without `sudo` (this is how the `show` subcommand works).

6. Once that's done, run `sudo systemctl daemon-reload` followed by `sudo systemctl enable --now debounced` to start the actual background process; without it alive, the control program does nothing whatsoever.

7. Last but not least, run `debouncectl --help`, read it for instructions on how to use it properly, and test your keyboard afterwards! If it's still bouncing, run `debouncectl stop` followed by `debouncectl start <device> [timeout]`. The timeout can be anywhere from 1 to 100, and is in milliseconds.

## Reporting Issues
If you run into problems with this software, please create an issue on the repository. I'll check on it often to make sure it's working as intended, and if issues arise, I will help diagnose problems as they come up.

## OS Support
Currently this program supports Linux in almost any flavor, whether it's Arch, Debian, Gentoo, Fedora, RHEL, Oracle Linux, CBL-Mariner, or whatever crazy distribution you find out there. Except for kernels built without `uinput` support, or if your system **somehow** lacks